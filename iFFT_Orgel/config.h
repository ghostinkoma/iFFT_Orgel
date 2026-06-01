/**
 * @file   config.h
 * @brief RP2040メディア再生システム (BadCodec + Opus) 設定
 * @version v1.0.0
 *
 * ハードウェア: Raspberry Pi Pico (RP2040 2MB版)
 * ディスプレイ: SSD1306 128x64 OLED
 * 音声: 差動PDM出力 (PIO SM1 + DMA)
 *
 * Flash パーティション:
 * Sketch:   512 KB  (0x00000 - 0x7FFFF)
 * LittleFS: 1536 KB (0x80000 - 0x1FFFFF)
 * 動画データ(Heatshrink圧縮済BadCodec): 960 KB
 * 音声データ(Opus 21kbps):              576 KB
 */

#ifndef CONFIG_H
#define CONFIG_H


// ==========================================
// 音色・エンベロープ設定（ここを書き換える！）
// ==========================================
#define CFG_TIMBRE_H2        0.4f   // 2倍音
#define CFG_TIMBRE_H3        0.01f  // 3倍音

#define CFG_ENV_ATTACK_SAMP  30      // アタック
#define CFG_ENV_DECAY_COEF   0.9999f // 減衰

#define CFG_AUDIO_LPF_ALPHA  0.05f    // ローパスフィルタ
#define CFG_AUDIO_BASS_BOOST  5.5f
/* ============================================================
 * 音量: 0=無音, 256=フルスケール
 * ============================================================ */
#define CFG_AUDIO_VOL       160U


/* ============================================================
 * Dispay String
 * ============================================================ */
#define PlayName "La Campanella"
#define ArtistName "Franz Liszt"
 
/* ============================================================
 * I2C (OLED) — PIO SM0 + DMA で自動転送
 * ============================================================ */
#define CFG_I2C_SDA         4       /* GP4 */
#define CFG_I2C_SCL         5       /* GP5 */
#define CFG_I2C_FREQ_HZ     1000000 /* 1MHz Fast-Mode Plus */

/* ============================================================
 * SSD1306 128x64
 * ============================================================ */
#define CFG_OLED_ADDR       0x3C
#define CFG_PHYS_W          128
#define CFG_PHYS_H          64
#define CFG_PAGES           8       /* 64px / 8px per page */
#define CFG_DISP_W          128
#define CFG_DISP_H          64
#define CFG_X_OFFSET        0
#define CFG_Y_OFFSET        0

/* ============================================================
 * BadCodec 映像
 * ============================================================ */
#define CFG_VIDEO_W         128
#define CFG_VIDEO_H         64
#define CFG_TARGET_FPS      30      /* Target: 30fps */

/* ============================================================
 * Heatshrink 解凍
 * Window: 11bit (2KB) — メモリとデコード効率のバランス
 * Lookahead: 4bit
 * ============================================================ */
#define CFG_HS_WINDOW_BITS  11
#define CFG_HS_LOOKAHEAD    4

/* ============================================================
 * LittleFS ファイルパス
 * ============================================================ */
#define CFG_VIDEO_FILE      "/video.bad"   /* Heatshrink圧縮済BadCodec */
#define CFG_AUDIO_FILE      "/audio.opus"  /* Opus 21kbps */

/* ============================================================
 * Opus 音声
 * ============================================================ */
#define CFG_OPUS_SAMPLE_RATE    22050U  /* 22.05kHz */
#define CFG_OPUS_CHANNELS       1       /* モノラル */
#define CFG_OPUS_BITRATE        21000   /* 21kbps VBR */
#define CFG_OPUS_FRAME_SIZE     480     /* 10ms @ 48kHz → 出力は22050Hzにリサンプル */
#define CFG_OPUS_MAX_PACKET     4000    /* 最大パケットサイズ (bytes) */

/* 音声リングバッファ: 約1.5秒 @ 22050Hz */
#define CFG_AUDIO_RING_SIZE     32768U  /* 2^15 samples, must be power of 2 */

/* ============================================================
 * 差動PDM出力 (PIO SM1 + DMA)
 * 正相: GP2   反転相: GP3
 * RC LPF: R=10kΩ, C=100nF (fc≈160Hz実効)
 * キャリア周波数: 約1MHz (PDM over-sample)
 * ============================================================ */
#define CFG_AUDIO_PIN_P     2       /* 正相 GPIO */
#define CFG_AUDIO_PIN_N     3       /* 反転相 GPIO */

/* PDM オーバーサンプリング比: 22050Hz × OSR = PIO clk target
 * OSR=45 → ~993kHz PDM キャリア (133MHz SYS / (3 × 45) ≈ 984kHz) */
#define CFG_PDM_OSR         45

/* ============================================================
 * CPU クロック
 * ============================================================ */
#define CFG_SYS_CLK_KHZ     133000  /* 133MHz (安定動作基準値) */
/* オーバークロック時: 150000 / 176000 / 200000 */

/* ============================================================
 * FreeRTOS タスク優先度
 * Core 0: video_task (映像+LittleFS+Heatshrink+BadCodec)
 * Core 1: opus_task  (Opus デコード+オーディオバッファ管理)
 * ============================================================ */
#define CFG_VIDEO_TASK_PRIO     5
#define CFG_AUDIO_TASK_PRIO     4
#define CFG_VIDEO_TASK_STACK    8192
/* Opus alloca() が使用するため大幅拡大: 16KB → 48KB
 * 特定の SILK WB パケットで stack overflow → opus_decode 無限ループの可能性
 * RAM 余裕 156KB あるため安全に拡大可能 */
#define CFG_AUDIO_TASK_STACK    49152



/* ============================================================
 * デバッグ出力 (0=無効 1=有効)
 * ============================================================ */
#define CFG_DEBUG           1

#if CFG_DEBUG
  #include <Arduino.h>
  #define DBG_PRINT(...)    Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...)  Serial.println(__VA_ARGS__)
  #define DBG_PRINTF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DBG_PRINT(...)
  #define DBG_PRINTLN(...)
  #define DBG_PRINTF(...)
#endif

#endif /* CONFIG_H */
