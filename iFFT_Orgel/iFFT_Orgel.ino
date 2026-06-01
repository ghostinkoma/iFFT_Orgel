// ============================================================
// iFFT_Orgel v15 (ピアノ音源版)
//
// 構成:
//   Core0: MIDI読み込み + bins[]書き込み + OLED描画
//   Core1: bins[]を読みiFFT合成 → dma_buf[]
//   DMA:   dma_buf[] → PWM CC → GP2/GP3 差動出力
//
// ピアノ音色:
//   - 8倍音 (基音 + h2~h8、相対振幅プロファイル)
//   - 音域別減衰時間 (低音 5s, 高音 0.5s) 事前テーブル化
//   - 高倍音先消失 (時間経過で暗い音に)
//   - 急峻な打鍵アタック (5サンプル)
//
// 重要: picoprobe書き込み後、target Picoをpower cycle してください
//       (USBケーブル抜き挿し)。Arduino-Pico既知バグでCore1が起動しません。
// ============================================================

#include "config.h"
#include "piano_voice.h"



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
// 定数
// ================================================================
#define SAMPLE_RATE        22050u
#define BUF_SAMPLES        1024u

#define SYNTH_LPF_ALPHA    0.85f
#define SYNTH_VOL_SCALE    1.0f

#define MIDI_NOTE_A0       21
#define MIDI_NOTE_C8       108
#define IFFT_BINS          128
#define IFFT_ACTIVE_MAX    16  // 同時発音数(8倍音×16=128 振動子, CPU余裕)

#define Q15_ONE            32767
#define Q15(x)             ((int16_t)((x) * 32767.0f))

#define NH                 PIANO_NUM_HARMONICS  // = 8

// 倍音ウェイトのQ15化(setup時に計算)
static int16_t harm_weight_q15[NH];
static int16_t hf_extra_q15[NH];

// ピアノ用ビン (8倍音持つ)
struct IfftBin {
    int16_t  re[NH];     // 各倍音の複素回転子
    int16_t  im[NH];
    int16_t  cos_w[NH];
    int16_t  sin_w[NH];
    int16_t  harm_amp[NH]; // 各倍音の現在振幅(Q15)
    int16_t  amp;          // 全体エンベロープ(Q15)
    int16_t  decay_q15;    // この音域の減衰係数(Q15)
    uint8_t  phase;        // 0=off 1=attack 2=decay 3=release
    uint32_t end_ms;
};

struct TriggerEvent {
    uint8_t  note;
    uint8_t  on;
    uint32_t end_ms;
};

#define PIANO_KEYS         88
#define SCREEN_W           128
#define SCREEN_H           64
#define VIS_WINDOW_MS      1500u
#define VIS_LOOKAHEAD      256

// ================================================================
// グローバル
// ================================================================
Adafruit_SSD1306 display(CFG_PHYS_W, CFG_PHYS_H, &Wire, -1);

static uint32_t pwm_wrap;
static uint32_t pwm_mid;
static uint     pwm_slice_num;
static uint     dma_chan;

static uint32_t dma_buf[2][BUF_SAMPLES] __attribute__((aligned(4)));

volatile uint8_t  buf_dma   = 0;
volatile uint8_t  buf_fill  = 1;
volatile bool     need_fill = true;
volatile uint32_t dma_complete_count = 0;

// 共有iFFT空間
volatile IfftBin bins[IFFT_BINS];
volatile uint8_t active_bins[IFFT_ACTIVE_MAX];
volatile uint8_t active_count = 0;

volatile uint32_t shared_playback_ms = 0;
volatile int      global_frame_idx   = 0;

volatile uint8_t  dbg_active = 0;
volatile uint32_t dbg_fill_us = 0;
volatile uint32_t dbg_oled_us = 0;
volatile uint8_t  dbg_oled_fps = 0;
volatile uint32_t core1_loop_count = 0;

volatile bool diag_mode = true;
static bool started = false;

static int16_t diag_re, diag_im, diag_cw, diag_sw;

// ================================================================
// ユーティリティ
// ================================================================
static inline int16_t q15_mul(int16_t a, int16_t b) {
    return (int16_t)(((int32_t)a * b) >> 15);
}

// ================================================================
// iFFT空間初期化 — 全88鍵の倍音twiddleを事前計算
// ================================================================
static void set_tw(float f, volatile int16_t *cw, volatile int16_t *sw) {
    if (f >= (float)SAMPLE_RATE * 0.5f) {
        *cw = Q15(1.0f); *sw = 0; return;
    }
    float w = 2.0f * (float)M_PI * f / (float)SAMPLE_RATE;
    *cw = Q15(cosf(w));
    *sw = Q15(sinf(w));
}

static void init_ifft_space() {
    for (int n = 0; n < IFFT_BINS; n++) {
        volatile IfftBin *b = &bins[n];
        b->amp = 0;
        b->phase = 0;
        b->end_ms = 0;
        b->decay_q15 = Q15(0.9999f); // デフォルト

        for (int h = 0; h < NH; h++) {
            b->re[h] = Q15(1.0f); b->im[h] = 0;
            b->cos_w[h] = Q15(1.0f); b->sin_w[h] = 0;
            b->harm_amp[h] = harm_weight_q15[h];
        }

        if (n < MIDI_NOTE_A0 || n > MIDI_NOTE_C8) continue;

        float freq = 440.0f * powf(2.0f, (n - 69) / 12.0f);
        for (int h = 0; h < NH; h++) {
            float hf = freq * (float)(h + 1);
            set_tw(hf, &b->cos_w[h], &b->sin_w[h]);
        }
        // 音域別減衰係数(テーブルから)
        b->decay_q15 = piano_decay_coef_q15[n - MIDI_NOTE_A0];
    }
    active_count = 0;
}

// ================================================================
// ビン管理 (Core0)
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
    uint8_t last = active_count - 1;
    active_bins[idx] = active_bins[last];
    __dmb();
    active_count = last;
}

static void note_on_bin(uint8_t note, uint32_t end_ms) {
    if (note < MIDI_NOTE_A0 || note > MIDI_NOTE_C8) return;
    volatile IfftBin *b = &bins[note];
    // MIDI duration を尊重。短いノートはリリースが3倍速で自然に消える
    b->end_ms = end_ms;

    // 全倍音の回転子と振幅をリセット
    b->amp = 0;
    for (int h = 0; h < NH; h++) {
        b->re[h] = Q15(1.0f);
        b->im[h] = 0;
        b->harm_amp[h] = harm_weight_q15[h];  // 倍音振幅をフレッシュに
    }
    __dmb();
    b->phase = 1;  // attack
    bin_activate(note);
}

// ================================================================
// Core1: iFFT合成
// ================================================================
static const int16_t ATTACK_STEP = (int16_t)(Q15_ONE / PIANO_ATTACK_SAMP);
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

                // 全体エンベロープ
                int16_t amp = b->amp;
                uint8_t ph  = b->phase;
                int16_t decay = b->decay_q15;
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
                        amp = q15_mul(amp, decay);
                        if (now_ms >= b->end_ms) ph = 3;
                        break;
                    case 3: {
                        // リリース: 通常減衰の5倍速 (ピアノのダンパー)
                        amp = q15_mul(amp, decay);
                        amp = q15_mul(amp, decay);
                        amp = q15_mul(amp, decay);
                        amp = q15_mul(amp, decay);
                        amp = q15_mul(amp, decay);
                        break;
                    }
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

                // 8倍音の回転子更新 + 倍音振幅減衰 + 合成
                int32_t bin_mix = 0;
                for (int h = 0; h < NH; h++) {
                    int16_t re = b->re[h], im = b->im[h];
                    int16_t cw = b->cos_w[h], sw = b->sin_w[h];
                    int16_t nr = q15_mul(re, cw) - q15_mul(im, sw);
                    int16_t ni = q15_mul(re, sw) + q15_mul(im, cw);
                    b->re[h] = nr; b->im[h] = ni;

                    // 倍音振幅を hf_extra で追加減衰(高倍音先消失)
                    int16_t ha = b->harm_amp[h];
                    if (h >= 2) {
                        ha = q15_mul(ha, hf_extra_q15[h]);
                        b->harm_amp[h] = ha;
                    }
                    bin_mix += (int32_t)q15_mul(nr, ha);
                }

                // bin全体にエンベロープ
                mix += (bin_mix * (int32_t)amp) >> 15;
            }

            dbg_active = alive;

            // 動的スケーリング
            int32_t divisor = alive < 2 ? 2 : (alive < 4 ? alive : (alive < 8 ? alive : 8));
            int32_t scaled = mix / divisor;
            if (scaled >  Q15_ONE) scaled =  Q15_ONE;
            if (scaled < -Q15_ONE) scaled = -Q15_ONE;
            sample = scaled;
        }

        // LPF
        int32_t diff = sample - lpf;
        lpf += (diff * (int32_t)LPF_Q15) >> 15;
        int32_t out = lpf;

        // PWMデューティ変換
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
// DMA完了割り込み (Core0)
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

    // 440Hz診断用回転子
    float w = 2.0f * (float)M_PI * 440.0f / (float)SAMPLE_RATE;
    diag_re = Q15(1.0f); diag_im = 0;
    diag_cw = Q15(cosf(w));
    diag_sw = Q15(sinf(w));

    Serial1.println("Pre-filling buffers...");
    fill_buffer_c1(dma_buf[0], 0);
    fill_buffer_c1(dma_buf[1], 0);

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
// Core1
// ================================================================
void setup1() {
    float w = 2.0f * (float)M_PI * 440.0f / (float)SAMPLE_RATE;
    diag_re = Q15(1.0f); diag_im = 0;
    diag_cw = Q15(cosf(w));
    diag_sw = Q15(sinf(w));
}

void loop1() {
    core1_loop_count++;
    __dmb();
    if (need_fill) {
        need_fill = false;
        uint32_t t0 = time_us_32();
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
    while (frame_idx < TOTAL_FRAMES && processed < 8) {
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
    const int32_t px_per_ms_q16 = ((int32_t)PLAY_Y << 16) / (int32_t)VIS_WINDOW_MS;
    display.clearDisplay();
    uint32_t cur = shared_playback_ms;

    while (vis_cursor < TOTAL_FRAMES) {
        uint32_t s = pgm_read_dword(&midi_sequence[vis_cursor].start_ms);
        if (s + VIS_WINDOW_MS >= cur) break;
        vis_cursor++;
    }
    if (vis_cursor >= TOTAL_FRAMES) vis_cursor = 0;

    int end_idx = vis_cursor + VIS_LOOKAHEAD;
    if (end_idx > TOTAL_FRAMES) end_idx = TOTAL_FRAMES;

    const int X_OFFSET = (SCREEN_W - PIANO_KEYS) / 2;

    for (int i = vis_cursor; i < end_idx; i++) {
        uint32_t start_ms = pgm_read_dword(&midi_sequence[i].start_ms);
        int32_t  diff     = (int32_t)(start_ms - cur);
        if (diff > (int32_t)VIS_WINDOW_MS) break;
        if (diff <= 0) continue;
        int y_bottom = PLAY_Y - (int)(((int64_t)diff * px_per_ms_q16) >> 16);
        if (y_bottom <= 0 || y_bottom > PLAY_Y) continue;
        for (int n = 0; n < 24; n++) {
            uint8_t  note = pgm_read_byte(&midi_sequence[i].poly_notes[n].note);
            uint16_t dur  = pgm_read_word(&midi_sequence[i].poly_notes[n].duration_ms);
            if (note < MIDI_NOTE_A0 || note > MIDI_NOTE_C8) continue;
            int x = X_OFFSET + (note - MIDI_NOTE_A0);
            if (x < 0 || x >= SCREEN_W) continue;
            int bar_h = (int)(((int32_t)dur * px_per_ms_q16) >> 16);
            if (bar_h < 2) bar_h = 2;
            int y_top = y_bottom - bar_h;
            if (y_top < 0) y_top = 0;
            display.drawFastVLine(x, y_top, y_bottom - y_top, SSD1306_WHITE);
        }
    }
    display.drawFastHLine(0, PLAY_Y, SCREEN_W, SSD1306_WHITE);
    display.display();
}

void setup() {
    Wire.setSDA(CFG_I2C_SDA);
    Wire.setSCL(CFG_I2C_SCL);
    Wire.setClock(1000000);  // 1MHz Fast-Mode Plus
    Wire.begin();

    Serial1.setTX(0);
    Serial1.setRX(1);
    Serial1.begin(115200);
    delay(500);
    Serial1.println();
    Serial1.println("=== iFFT_Orgel v15 PIANO ===");

    // 倍音ウェイトのQ15化
    for (int h = 0; h < NH; h++) {
        // 正規化: 合計が1.0になるように
        float sum = 0.0f;
        for (int k = 0; k < NH; k++) sum += piano_harmonic_weight[k];
        harm_weight_q15[h] = Q15(piano_harmonic_weight[h] / sum);
        hf_extra_q15[h]    = Q15(piano_hf_extra[h]);
    }
    Serial1.printf("Harmonic count=%d  hweight[0]=%d hweight[7]=%d\n",
        NH, harm_weight_q15[0], harm_weight_q15[7]);

    display.begin(SSD1306_SWITCHCAPVCC, CFG_OLED_ADDR);
    display.clearDisplay();
    display.display();

    init_ifft_space();
    setup_pwm_dma();
    Serial1.println("Setup done. Power-cycle target if Core1 not running.");
}

void loop() {
    uint32_t now = millis();

    if (diag_mode && now > 5000u) {
        diag_mode = false;
        Serial1.println("DIAG -> PIANO");
    }

    process_midi(now);

    // OLED 30FPS
    static uint32_t oled_frame_count = 0;
    static uint32_t oled_fps_t0 = 0;
    if (now - last_oled_ms >= 33u) {
        last_oled_ms = now;
        uint32_t t0 = time_us_32();
        draw_oled(now);
        dbg_oled_us = time_us_32() - t0;
        oled_frame_count++;
        if (now - oled_fps_t0 >= 1000u) {
            dbg_oled_fps = oled_frame_count;
            oled_frame_count = 0;
            oled_fps_t0 = now;
        }
    }

    if (now - last_diag_ms >= 3000u) {
        last_diag_ms = now;
        Serial1.printf("[%lus] %s active=%u fill=%luus oled=%luus(%uFPS) c1=%lu frm=%d/%d\n",
            now/1000u, diag_mode ? "DIAG" : "PIANO",
            (unsigned)dbg_active,
            (unsigned long)dbg_fill_us,
            (unsigned long)dbg_oled_us,
            (unsigned)dbg_oled_fps,
            (unsigned long)core1_loop_count,
            global_frame_idx, TOTAL_FRAMES);
    }
}
