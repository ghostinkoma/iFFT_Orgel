#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "midi_data.h"
#include <hardware/pwm.h> 

Adafruit_SSD1306 display(CFG_PHYS_W, CFG_PHYS_H, &Wire, -1);

#define PIANO_KEYS 88
#define MIDI_NOTE_A0 21  
#define MIDI_NOTE_C8 108 
#define SCREEN_H 64
#define SCREEN_W 128
const int X_OFFSET = (SCREEN_W - PIANO_KEYS) / 2; 

struct PlaybackNote {
  uint8_t  note;
  float    real;
  float    imag;
  uint32_t end_ms;
  uint32_t atk;
  float    env; 
};
volatile PlaybackNote active_slots[24] = {0};

float note_omega_deltas[128] = {0.0f};
volatile uint32_t shared_playback_time = 0;
volatile int global_frame_idx = 0; 
volatile bool is_title_screen = true;
volatile uint32_t title_timer_start = 0;

uint pwm_slice_p, pwm_chan_p;
uint pwm_slice_n, pwm_chan_n;

void init_frequency_table() {
  for (int i = 0; i < 128; i++) {
    if (i >= MIDI_NOTE_A0 && i <= MIDI_NOTE_C8) {
      float freq = 440.0f * pow(2.0f, (i - 69) / 12.0f);
      note_omega_deltas[i] = (2.0f * PI * freq) / (float)CFG_OPUS_SAMPLE_RATE;
    } else {
      note_omega_deltas[i] = 0.0f;
    }
  }
}

static float prev_output = 0.0f;

void inline generate_audio_from_ifft_space() {
  static float prev_output = 0.0f; 
  float total_real_wave = 0.0f;
  int active_count = 0;
  uint32_t current_time = shared_playback_time;

  for (int i = 0; i < 24; i++) {
    uint8_t note = active_slots[i].note;
    if (note == 0) continue; 

    float omega = note_omega_deltas[note];
    float re1 = active_slots[i].real;
    float im1 = active_slots[i].imag;
    float cos1 = cos(omega);
    float sin1 = sin(omega);
    active_slots[i].real = re1 * cos1 - im1 * sin1;
    active_slots[i].imag = re1 * sin1 + im1 * cos1;

    // 倍音合成
    float harmonic_wave = active_slots[i].real + 
                          (active_slots[i].real * 0.125f) + 
                          (active_slots[i].real * 0.06f);

    total_real_wave += harmonic_wave;
    active_count++;
  }

  if (active_count > 0) total_real_wave /= (float)active_count;

  // 1. ソフトウェア・ローパスフィルター (IIR型)
  float filtered_wave = (total_real_wave * CFG_AUDIO_LPF_ALPHA) + 
                        (prev_output * (1.0f - CFG_AUDIO_LPF_ALPHA));
  
  // 2. 簡易バスブースト処理
  float bass_wave = filtered_wave + (filtered_wave - prev_output) * (CFG_AUDIO_BASS_BOOST - 1.0f);
  
  // 状態の更新は最後に行う
  prev_output = filtered_wave; 

  // 3. 最終的な振幅計算
  int32_t amplitude = (int32_t)(bass_wave * 127.0f * (float)CFG_AUDIO_VOL / 256.0f);
  
  // 完全差動駆動用のPWM生成
  int32_t pwm_duty_p = 128 + amplitude;
  int32_t pwm_duty_n = 128 - amplitude;

  // クランプ処理
  if (pwm_duty_p > 255) pwm_duty_p = 255;
  if (pwm_duty_p < 0)   pwm_duty_p = 0;
  if (pwm_duty_n > 255) pwm_duty_n = 255;
  if (pwm_duty_n < 0)   pwm_duty_n = 0;

  pwm_set_chan_level(pwm_slice_p, pwm_chan_p, (uint16_t)pwm_duty_p);
  pwm_set_chan_level(pwm_slice_n, pwm_chan_n, (uint16_t)pwm_duty_n);
}

// ============================================================
// Core 1: オーディオ再生
// ============================================================
void setup1() {
  Serial.begin(115200);
  Serial.println("Setup 1");
  init_frequency_table();

  gpio_set_function(CFG_AUDIO_PIN_P, GPIO_FUNC_PWM);
  pwm_slice_p = pwm_gpio_to_slice_num(CFG_AUDIO_PIN_P);
  pwm_chan_p = pwm_gpio_to_channel(CFG_AUDIO_PIN_P);
  pwm_set_wrap(pwm_slice_p, 255); 
  pwm_set_enabled(pwm_slice_p, true);

  gpio_set_function(CFG_AUDIO_PIN_N, GPIO_FUNC_PWM);
  pwm_slice_n = pwm_gpio_to_slice_num(CFG_AUDIO_PIN_N);
  pwm_chan_n = pwm_gpio_to_channel(CFG_AUDIO_PIN_N);
  pwm_set_wrap(pwm_slice_n, 255); 
  pwm_set_enabled(pwm_slice_n, true);
}

void loop1() {
  static int frame_idx = 0;
  static uint32_t start_offset = 0;
  
  // オープニングセレモニー中はオーディオ待機
  if (millis() < 3000) return;

  if (start_offset == 0) start_offset = millis();
  shared_playback_time = millis() - start_offset;

  while (frame_idx < TOTAL_FRAMES) {
    uint32_t start_ms = pgm_read_dword(&midi_sequence[frame_idx].start_ms);
    
    if (shared_playback_time >= start_ms) {
      for (int i = 0; i < 24; i++) {
        uint8_t  note = pgm_read_byte(&midi_sequence[frame_idx].poly_notes[i].note);
        uint16_t dur  = pgm_read_word(&midi_sequence[frame_idx].poly_notes[i].duration_ms);
        
        if (note >= MIDI_NOTE_A0 && note <= MIDI_NOTE_C8) {
          active_slots[i].note = note;
          active_slots[i].real = 1.0f;
          active_slots[i].imag = 0.0f;
          active_slots[i].end_ms = start_ms + dur; 
          active_slots[i].atk = CFG_ENV_ATTACK_SAMP;
          active_slots[i].env = 0.0f;        
        } else {
          active_slots[i].note = 0; 
        }
      }
      frame_idx++;
      global_frame_idx = frame_idx;
    } else {
      break;
    }
  }

  if (frame_idx >= TOTAL_FRAMES) {
    frame_idx = 0;
    global_frame_idx = 0;
    start_offset = millis();
    for(int i = 0; i < 24; i++) active_slots[i].note = 0;
  }

  static unsigned long last_sample_time = 0;
  unsigned long now_us = micros();
  if (now_us - last_sample_time >= (1000000 / CFG_OPUS_SAMPLE_RATE)) {
    last_sample_time = now_us;
    generate_audio_from_ifft_space();
  }
}

// ============================================================
// Core 0: 画面描画
// ============================================================
// --- setup() の先頭に必ず追加してください ---
void setup() {
  // config.h の定義に従いピンを設定 (重要！)
  Wire.setSDA(CFG_I2C_SDA);
  Wire.setSCL(CFG_I2C_SCL);
  Wire.begin();
  Serial.begin(115200);
  Serial.println("Setup 0");
  display.begin(SSD1306_SWITCHCAPVCC, CFG_OLED_ADDR);
  title_timer_start = millis();
  is_title_screen = true;
}

// 画面描画用に追加する状態管理変数
uint32_t finish_time = 0; 
bool is_finished = false;

void loop() {
  bool is_title_mode = (millis() < 3000) || 
                       (global_frame_idx == 0 && shared_playback_time < 500 && millis() > 5000);

  if (is_title_mode) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 20);
    display.println(PlayName);
    display.setCursor(31, 36);
    display.println(ArtistName);
    display.display();
    delay(500);
    return; 
  }

  // --- 以下、ノーツ描画処理 ---
  display.clearDisplay();
  uint32_t current_time = shared_playback_time;
  float ms_per_pixel = 1000.0f / 64.0f;

  for (int i = 0; i < TOTAL_FRAMES; i++) {
    uint32_t start_ms = pgm_read_dword(&midi_sequence[i].start_ms);
    int32_t time_diff = (int32_t)(start_ms - current_time);

    // 画面の先1秒以内のノーツを描画
    if (time_diff >= 0 && time_diff < 1000) {
      int y_start = 60 - (int)((float)time_diff / ms_per_pixel);
      
      for (int n = 0; n < 24; n++) {
        uint8_t note = pgm_read_byte(&midi_sequence[i].poly_notes[n].note);
        uint16_t dur = pgm_read_word(&midi_sequence[i].poly_notes[n].duration_ms);
        
        if (note >= MIDI_NOTE_A0 && note <= MIDI_NOTE_C8) {
          int x = X_OFFSET + (note - MIDI_NOTE_A0);
          int h = (int)((float)dur / ms_per_pixel);
          if (h < 1) h = 1;
          
          int y_end = y_start - h;
          if (y_end < 0) y_end = 0;
          display.drawFastVLine(x, y_end, y_start - y_end + 1, SSD1306_WHITE);
        }
      }
    }
  }

  display.drawFastHLine(X_OFFSET, 63, PIANO_KEYS, SSD1306_WHITE);
  display.display();
}
