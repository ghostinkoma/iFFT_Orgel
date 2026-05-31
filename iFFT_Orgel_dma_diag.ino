// ============================================================
// iFFT_Orgel v14 (Core0/Core1 分担版 - ユーザー設計準拠)
//
// 重要: picoprobe経由書き込み後はtarget Picoをpower cycle
//       (USBケーブル抜き挿し)してください。
//       Arduino-Picoの既知バグでpicoprobe書き込み直後はCore1が
//       起動しません。
//
// Core0: MIDI読み出し → bins[]へ書き込み + OLED描画
// Core1: bins[]を読んで iFFT 波形生成 → dma_buf[]
// DMA:   dma_buf[] → PWM CC → GP2/GP3 差動出力
//
// 共有メモリ:
//   bins[128]            : Core0=書き Core1=読み (volatileで保護)
//   shared_playback_ms   : Core0=書き Core1=読み
//   dma_buf[2][BUF]      : Core1=書き DMA=読み
//   buf_fill, buf_ready  : Core0(DMA_IRQ) / Core1 ハンドシェイク
// ============================================================

#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "midi_data.h"
#include <hardware/pwm.h>
#include <hardware/dma.h>
#include <hardware/clocks.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/sync.h>

void setup();
void loop();
void setup1();
void loop1();

// ================================================================
// 定数定義
// ================================================================

#define SAMPLE_RATE        22050u
#define BUF_SAMPLES        1024u

#define SYNTH_DECAY_COEF   0.99985f
#define SYNTH_REL_COEF     0.998f
#define SYNTH_ATTACK_SAMP  10
#define SYNTH_LPF_ALPHA    0.5f
#define SYNTH_VOL_SCALE    1.0f

#define MIDI_NOTE_A0       21
#define MIDI_NOTE_C8       108
#define IFFT_BINS          128
#define IFFT_ACTIVE_MAX    32

#define H0_WEIGHT          0.709f
#define H1_WEIGHT          0.284f
#define H2_WEIGHT          0.007f

#define Q15_ONE            32767
#define Q15(x)             ((int16_t)((x) * 32767.0f))

// Core0→Core1で共有するiFFTビン
struct IfftBin {
    int16_t  re,  im;
    int16_t  re2, im2;
    int16_t  re3, im3;
    int16_t  cos_w,  sin_w;
    int16_t  cos_w2, sin_w2;
    int16_t  cos_w3, sin_w3;
    int16_t  amp;
    uint8_t  phase;       // 0=off 1=attack 2=decay 3=release
    uint32_t end_ms;
};

#define PIANO_KEYS         88
#define SCREEN_W           128
#define SCREEN_H           64
#define VIS_WINDOW_MS      1500u
#define VIS_LOOKAHEAD      256

// ================================================================
// 共有グローバル変数(Core0/Core1で共有)
// ================================================================

Adafruit_SSD1306 display(CFG_PHYS_W, CFG_PHYS_H, &Wire, -1);

// PWM動的パラメータ(setup_pwm_dma で初期化)
static uint32_t pwm_wrap;
static uint32_t pwm_mid;
static uint     pwm_slice_num;
static uint     dma_chan;

// ダブルバッファ: Core1書き、DMA読み
static uint32_t dma_buf[2][BUF_SAMPLES] __attribute__((aligned(4)));

// バッファ管理(DMA IRQ=Core0と Core1 で共有)
volatile uint8_t  buf_dma   = 0;
volatile uint8_t  buf_fill  = 1;
volatile bool     need_fill = true;
volatile uint32_t dma_complete_count = 0;

// ★iFFT空間: Core0=書き込み, Core1=読み出し
// volatileで両コアからのアクセスを保証
volatile IfftBin bins[IFFT_BINS];
volatile uint8_t active_bins[IFFT_ACTIVE_MAX];
volatile uint8_t active_count = 0;

// 共有状態
volatile uint32_t shared_playback_ms = 0;
volatile int      global_frame_idx   = 0;

// 診断
volatile uint8_t  dbg_active = 0;
volatile uint32_t dbg_fill_us = 0;
volatile uint32_t core1_loop_count = 0;   // Core1が動いているかの証拠

// モード
volatile bool diag_mode = true;
static bool started = false;

// 440Hz診断用回転子(Core1専有)
static int16_t diag_re, diag_im, diag_cw, diag_sw;

// ================================================================
// ユーティリティ
// ================================================================

static inline int16_t q15_mul(int16_t a, int16_t b) {
    return (int16_t)(((int32_t)a * b) >> 15);
}

// ================================================================
// iFFT初期化 (Core0で実行)
// ================================================================

static void set_tw(float f, int16_t *cw, int16_t *sw) {
    if (f >= (float)SAMPLE_RATE * 0.5f) {
        *cw = Q15(1.0f); *sw = 0; return;
    }
    float w = 2.0f * (float)M_PI * f / (float)SAMPLE_RATE;
    *cw = Q15(cosf(w));
    *sw = Q15(sinf(w));
}

static void init_ifft_space() {
    for (int n = 0; n < IFFT_BINS; n++) {
        bins[n].re  = Q15(1.0f); bins[n].im  = 0;
        bins[n].re2 = Q15(1.0f); bins[n].im2 = 0;
        bins[n].re3 = Q15(1.0f); bins[n].im3 = 0;
        bins[n].cos_w  = Q15(1.0f); bins[n].sin_w  = 0;
        bins[n].cos_w2 = Q15(1.0f); bins[n].sin_w2 = 0;
        bins[n].cos_w3 = Q15(1.0f); bins[n].sin_w3 = 0;
        bins[n].amp    = 0;
        bins[n].phase  = 0;
        bins[n].end_ms = 0;
        if (n < MIDI_NOTE_A0 || n > MIDI_NOTE_C8) continue;
        float freq = 440.0f * powf(2.0f, (n - 69) / 12.0f);
        int16_t cw, sw;
        set_tw(freq, &cw, &sw); bins[n].cos_w = cw; bins[n].sin_w = sw;
        set_tw(freq*2.0f, &cw, &sw); bins[n].cos_w2 = cw; bins[n].sin_w2 = sw;
        set_tw(freq*3.0f, &cw, &sw); bins[n].cos_w3 = cw; bins[n].sin_w3 = sw;
    }
    active_count = 0;
}

// ================================================================
// ビン管理 (Core0で実行)
// ================================================================

static void bin_activate(uint8_t n) {
    for (uint8_t i = 0; i < active_count; i++)
        if (active_bins[i] == n) return;
    if (active_count < IFFT_ACTIVE_MAX) {
        active_bins[active_count] = n;
        __dmb();
        active_count++;
    }
}

static void bin_deactivate_idx(uint8_t idx) {
    // Core1から呼ばれる。active_bins[idx]を末尾と入れ替えて減らす
    uint8_t last = active_count - 1;
    active_bins[idx] = active_bins[last];
    __dmb();
    active_count = last;
}

static void note_on_bin(uint8_t note, uint32_t end_ms) {
    if (note < MIDI_NOTE_A0 || note > MIDI_NOTE_C8) return;
    volatile IfftBin *b = &bins[note];
    uint32_t min_end = shared_playback_ms + 300u;
    b->end_ms = (end_ms > min_end) ? end_ms : min_end;
    b->amp = 0;
    b->re  = Q15(1.0f); b->im  = 0;
    b->re2 = Q15(1.0f); b->im2 = 0;
    b->re3 = Q15(1.0f); b->im3 = 0;
    __dmb();
    b->phase = 1;  // phaseを最後に書く(Core1がphase!=0で読み始める)
    bin_activate(note);
}

// ================================================================
// バッファ充填 (Core1で実行)
// ================================================================

static const int16_t DECAY_Q15   = Q15(SYNTH_DECAY_COEF);
static const int16_t REL_Q15     = Q15(SYNTH_REL_COEF);
static const int16_t ATTACK_STEP = (int16_t)(Q15_ONE / SYNTH_ATTACK_SAMP);
static const int16_t H0_Q15      = Q15(H0_WEIGHT);
static const int16_t H1_Q15      = Q15(H1_WEIGHT);
static const int16_t H2_Q15      = Q15(H2_WEIGHT);
static const int16_t LPF_Q15     = Q15(SYNTH_LPF_ALPHA);

static void fill_buffer_c1(uint32_t *buf, uint32_t base_ms) {
    static int32_t lpf = 0;

    for (uint32_t s = 0; s < BUF_SAMPLES; s++) {
        uint32_t now_ms = base_ms + (s * 1000u) / SAMPLE_RATE;
        int32_t sample;

        if (diag_mode) {
            int16_t nr = q15_mul(diag_re, diag_cw) - q15_mul(diag_im, diag_sw);
            int16_t ni = q15_mul(diag_re, diag_sw) + q15_mul(diag_im, diag_cw);
            diag_re = nr; diag_im = ni;
            sample = (int32_t)nr;
        } else {
            int32_t mix = 0;
            uint8_t alive = 0;
            uint8_t i = 0;
            uint8_t cur_count = active_count;

            while (i < cur_count) {
                uint8_t  note = active_bins[i];
                volatile IfftBin *b = &bins[note];

                // エンベロープ
                int16_t amp = b->amp;
                uint8_t ph  = b->phase;
                switch (ph) {
                    case 1: {
                        int32_t new_amp = (int32_t)amp + (int32_t)ATTACK_STEP;
                        if (new_amp >= (int32_t)Q15_ONE) {
                            amp = Q15_ONE; ph = 2;
                        } else {
                            amp = (int16_t)new_amp;
                        }
                        break;
                    }
                    case 2:
                        amp = q15_mul(amp, DECAY_Q15);
                        if (now_ms >= b->end_ms) ph = 3;
                        break;
                    case 3:
                        amp = q15_mul(amp, REL_Q15);
                        break;
                }
                b->amp = amp;
                b->phase = ph;

                if (amp < 32) {
                    b->amp = 0; b->phase = 0;
                    bin_deactivate_idx(i);
                    cur_count = active_count;
                    continue;
                }
                alive++;
                i++;

                // 回転子更新
                int16_t re  = b->re,  im  = b->im;
                int16_t re2 = b->re2, im2 = b->im2;
                int16_t re3 = b->re3, im3 = b->im3;
                int16_t cw  = b->cos_w,  sw  = b->sin_w;
                int16_t cw2 = b->cos_w2, sw2 = b->sin_w2;
                int16_t cw3 = b->cos_w3, sw3 = b->sin_w3;

                int16_t nr  = q15_mul(re,  cw)  - q15_mul(im,  sw);
                int16_t ni  = q15_mul(re,  sw)  + q15_mul(im,  cw);
                int16_t nr2 = q15_mul(re2, cw2) - q15_mul(im2, sw2);
                int16_t ni2 = q15_mul(re2, sw2) + q15_mul(im2, cw2);
                int16_t nr3 = q15_mul(re3, cw3) - q15_mul(im3, sw3);
                int16_t ni3 = q15_mul(re3, sw3) + q15_mul(im3, cw3);

                b->re  = nr;  b->im  = ni;
                b->re2 = nr2; b->im2 = ni2;
                b->re3 = nr3; b->im3 = ni3;

                int32_t bin_out =
                    (int32_t)q15_mul(nr,  H0_Q15) +
                    (int32_t)q15_mul(nr2, H1_Q15) +
                    (int32_t)q15_mul(nr3, H2_Q15);
                mix += (int32_t)q15_mul((int16_t)bin_out, amp);
            }

            dbg_active = alive;
            int32_t divisor = alive < 2 ? 2 : (alive < 4 ? alive : (alive < 8 ? alive : 8));
            int32_t scaled = mix / divisor;
            if (scaled >  Q15_ONE) scaled =  Q15_ONE;
            if (scaled < -Q15_ONE) scaled = -Q15_ONE;
            sample = scaled;
        }

        // LPF (Q15: lpf と sample 同スケール)
        // lpf += alpha * (sample - lpf) where alpha = LPF_Q15/Q15_ONE
        int32_t diff = sample - lpf;
        lpf += (diff * (int32_t)LPF_Q15) >> 15;
        int32_t out = lpf;

        // PWMデューティ
        int32_t duty_a = (int32_t)((float)out
                          * (float)pwm_mid / (float)Q15_ONE
                          * SYNTH_VOL_SCALE)
                         + (int32_t)pwm_mid;
        if (duty_a > (int32_t)pwm_wrap) duty_a = (int32_t)pwm_wrap;
        if (duty_a < 0) duty_a = 0;
        uint32_t duty_b = pwm_wrap - (uint32_t)duty_a;
        buf[s] = (duty_b << 16) | (uint32_t)duty_a;
    }
}

// ================================================================
// DMA完了割り込み (Core0で実行)
// ================================================================

static void __isr dma_irq_handler() {
    if (!dma_channel_get_irq0_status(dma_chan)) return;
    dma_channel_acknowledge_irq0(dma_chan);

    uint8_t next = buf_fill;
    buf_dma  = next;
    buf_fill = 1 - next;
    need_fill = true;

    dma_channel_set_read_addr(dma_chan, dma_buf[buf_dma], false);
    dma_channel_set_trans_count(dma_chan, BUF_SAMPLES, true);

    dma_complete_count++;
}

// ================================================================
// PWM + DMA 初期化 (Core0)
// ================================================================

static void setup_pwm_dma() {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    pwm_wrap = sys_hz / SAMPLE_RATE - 1u;
    pwm_mid  = pwm_wrap / 2u;
    Serial1.printf("sys=%lu wrap=%lu mid=%lu\n", sys_hz, pwm_wrap, pwm_mid);

    gpio_set_function(CFG_AUDIO_PIN_P, GPIO_FUNC_PWM);
    gpio_set_function(CFG_AUDIO_PIN_N, GPIO_FUNC_PWM);
    gpio_set_drive_strength(CFG_AUDIO_PIN_P, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(CFG_AUDIO_PIN_N, GPIO_DRIVE_STRENGTH_12MA);

    pwm_slice_num = pwm_gpio_to_slice_num(CFG_AUDIO_PIN_P);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, pwm_wrap);
    pwm_config_set_clkdiv_int(&cfg, 1);
    pwm_init(pwm_slice_num, &cfg, false);

    pwm_set_chan_level(pwm_slice_num, PWM_CHAN_A, pwm_mid);
    pwm_set_chan_level(pwm_slice_num, PWM_CHAN_B, pwm_wrap - pwm_mid);

    // 440Hz診断用回転子を初期化(setup1より前にfill_buffer_c1を呼ぶため)
    {
        float w = 2.0f * (float)M_PI * 440.0f / (float)SAMPLE_RATE;
        diag_re = Q15(1.0f); diag_im = 0;
        diag_cw = Q15(cosf(w));
        diag_sw = Q15(sinf(w));
    }
    
    // 両バッファに事前に実音声データ(diag mode 440Hz)を充填
    // これでDMA起動直後から音が出る
    Serial1.println("Pre-filling buffers with 440Hz...");
    fill_buffer_c1(dma_buf[0], 0);
    fill_buffer_c1(dma_buf[1], 0);
    Serial1.printf("dma_buf[0][0]=%08lX [256]=%08lX (silent should not be both same)\n",
        dma_buf[0][0], dma_buf[0][256]);

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pwm_get_dreq(pwm_slice_num));

    dma_channel_configure(
        dma_chan, &dc,
        &pwm_hw->slice[pwm_slice_num].cc,
        dma_buf[0], BUF_SAMPLES, false);

    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    pwm_set_enabled(pwm_slice_num, true);
    dma_channel_start(dma_chan);
}

// ================================================================
// Core1: iFFT処理専用ループ
// ================================================================

void setup1() {
    // 440Hz診断用回転子
    float w = 2.0f * (float)M_PI * 440.0f / (float)SAMPLE_RATE;
    diag_re = Q15(1.0f); diag_im = 0;
    diag_cw = Q15(cosf(w));
    diag_sw = Q15(sinf(w));
}

void loop1() {
    core1_loop_count++;
    __dmb();   // メモリバリア: need_fill, bins[] を確実に読む
    if (need_fill) {
        need_fill = false;
        uint32_t t0 = time_us_32();
        // buf_fill は volatile なので毎回読み直す
        uint8_t target = buf_fill;
        fill_buffer_c1(dma_buf[target], shared_playback_ms);
        dbg_fill_us = time_us_32() - t0;
    }
}

// ================================================================
// Core0: MIDI + OLED
// ================================================================

static int vis_cursor = 0;
static uint32_t last_oled_ms = 0;
static uint32_t last_diag_ms = 0;
static int frame_idx = 0;
static uint32_t start_offset = 0;

static void process_midi(uint32_t now) {
    if (diag_mode) return;
    if (!started) { start_offset = now; started = true; }
    shared_playback_ms = now - start_offset;

    int processed = 0;
    while (frame_idx < TOTAL_FRAMES && processed < 4) {
        uint32_t fstart = pgm_read_dword(&midi_sequence[frame_idx].start_ms);
        if (shared_playback_ms < fstart) break;
        for (int i = 0; i < 24; i++) {
            uint8_t  note = pgm_read_byte(&midi_sequence[frame_idx].poly_notes[i].note);
            uint16_t dur  = pgm_read_word(&midi_sequence[frame_idx].poly_notes[i].duration_ms);
            if (note >= MIDI_NOTE_A0 && note <= MIDI_NOTE_C8) {
                note_on_bin(note, fstart + dur);
            }
        }
        frame_idx++;
        global_frame_idx = frame_idx;
        processed++;
    }
    if (frame_idx >= TOTAL_FRAMES) {
        frame_idx = 0; global_frame_idx = 0; start_offset = now;
    }
}

static void draw_oled(uint32_t now) {
    bool title = (now < 3000u) ||
                 (global_frame_idx == 0 && shared_playback_ms < 200u);
    if (title) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        if (diag_mode) {
            display.setCursor(10, 20); display.println("DIAGNOSTIC");
            display.setCursor(10, 36); display.println("440Hz test");
        } else {
            display.setCursor(25, 20); display.println(PlayName);
            display.setCursor(31, 36); display.println(ArtistName);
        }
        display.display();
        vis_cursor = 0;
        return;
    }

    const int PLAY_Y = SCREEN_H - 5;
    const float ms_per_px = (float)VIS_WINDOW_MS / (float)PLAY_Y;
    display.clearDisplay();
    uint32_t cur = shared_playback_ms;

    while (vis_cursor < TOTAL_FRAMES) {
        uint32_t s = pgm_read_dword(&midi_sequence[vis_cursor].start_ms);
        if (s + VIS_WINDOW_MS >= cur) break;
        vis_cursor++;
    }
    if (vis_cursor >= TOTAL_FRAMES) vis_cursor = 0;

    int end = vis_cursor + VIS_LOOKAHEAD;
    if (end > TOTAL_FRAMES) end = TOTAL_FRAMES;

    for (int i = vis_cursor; i < end; i++) {
        uint32_t start_ms = pgm_read_dword(&midi_sequence[i].start_ms);
        int32_t  diff     = (int32_t)(start_ms - cur);
        if (diff > (int32_t)VIS_WINDOW_MS) break;
        if (diff <= 0) continue;
        int y_bottom = PLAY_Y - (int)((float)diff / ms_per_px);
        if (y_bottom <= 0 || y_bottom > PLAY_Y) continue;
        for (int n = 0; n < 24; n++) {
            uint8_t  note = pgm_read_byte(&midi_sequence[i].poly_notes[n].note);
            uint16_t dur  = pgm_read_word(&midi_sequence[i].poly_notes[n].duration_ms);
            if (note < MIDI_NOTE_A0 || note > MIDI_NOTE_C8) continue;
            int x = (SCREEN_W - PIANO_KEYS) / 2 + (note - MIDI_NOTE_A0);
            if (x < 0 || x >= SCREEN_W) continue;
            int bar_h = max(2, (int)((float)dur / ms_per_px));
            int y_top = max(0, y_bottom - bar_h);
            display.drawFastVLine(x, y_top, y_bottom - y_top, SSD1306_WHITE);
        }
    }
    display.drawFastHLine(0, PLAY_Y, SCREEN_W, SSD1306_WHITE);
    display.display();
}

void setup() {
    Wire.setSDA(CFG_I2C_SDA);
    Wire.setSCL(CFG_I2C_SCL);
    Wire.setClock(400000);
    Wire.begin();

    Serial1.setTX(0);
    Serial1.setRX(1);
    Serial1.begin(115200);
    delay(500);
    Serial1.println();
    Serial1.println("=== iFFT_Orgel v14 (Core0/Core1) ===");
    Serial1.println("NOTE: After picoprobe upload, POWER CYCLE target Pico!");

    display.begin(SSD1306_SWITCHCAPVCC, CFG_OLED_ADDR);
    display.clearDisplay();
    display.display();

    init_ifft_space();
    setup_pwm_dma();
    Serial1.println("Setup done. Core1 should start iFFT.");
}

void loop() {
    uint32_t now = millis();

    // 診断モード終了
    if (diag_mode && now > 5000u) {
        diag_mode = false;
        Serial1.println("DIAG -> iFFT");
    }

    process_midi(now);

    if (now - last_oled_ms >= 150u) {
        last_oled_ms = now;
        draw_oled(now);
    }

    if (now - last_diag_ms >= 2000u) {
        last_diag_ms = now;
        Serial1.printf("[%lus] %s active=%u(cnt=%u) fill=%luus dma=%lu c1=%lu frm=%d/%d\n",
            now/1000u,
            diag_mode ? "DIAG" : "IFFT",
            (unsigned)dbg_active,
            (unsigned)active_count,
            (unsigned long)dbg_fill_us,
            (unsigned long)dma_complete_count,
            (unsigned long)core1_loop_count,
            global_frame_idx, TOTAL_FRAMES);
        // Core1動作確認: c1がカウントアップしていればCore1動作中
    }
}
