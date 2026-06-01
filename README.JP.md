# iFFT_Orgel — RP2040 Piano Synthesizer

## 特徴

- **真のiFFT空間アーキテクチャ**: MIDIノート番号をビン番号としたsparse iFFT合成。重ね合わせ原理により多和音時の歪みが発生しない
- **ピアノ音色**: 8倍音合成、音域別減衰時間(低音5s〜高音0.5s)、高倍音先消失による経時的音色変化
- **Q15固定小数点演算**: float除算を排除した整数演算ベースの高速処理
- **デュアルコア分担**: Core0でMIDI/OLED、Core1でiFFT専用
- **DMA + ハードウェアPWM**: 差動11bit/13bit PWM出力、CPU割り込みなしの連続音声生成
- **30FPS OLED**: ピアノロール風落下ノート表示

## ハードウェア構成

| 信号 | GPIO | 接続先 |
|------|------|--------|
| PWM正相 | GP2 | アンプ正相入力 (1kΩ + 100nF LPF経由) |
| PWM逆相 | GP3 | アンプ逆相入力 (1kΩ + 100nF LPF経由) |
| I2C SDA | GP4 | SSD1306 OLED |
| I2C SCL | GP5 | SSD1306 OLED |
| UART TX | GP0 | picoprobe RX (デバッグ出力) |
| UART RX | GP1 | picoprobe TX |

差動PWM出力は1kΩ抵抗+100nFローパスフィルタ+10μFカップリングコンデンサ経由でアンプかBTLスピーカーへ接続します。

## ファイル構成

```
iFFT_Orgel/
├── iFFT_Orgel.ino         メインスケッチ(Core0/Core1)
├── piano_voice.h          ピアノ音色定義(倍音プロファイル等)
├── piano_decay_table.cpp  88鍵分の減衰係数事前計算テーブル
├── config.h               ハードウェアピン定義
├── midi_data.h            La Campanella MIDIシーケンス
└── README.md              本ファイル
```

## アーキテクチャ

```
┌──────────────────────────────────┐    ┌────────────────────────────┐
│ Core0                            │    │ Core1                      │
│                                  │    │                            │
│ ┌─────────────────────────────┐  │    │ ┌────────────────────────┐ │
│ │ MIDIシーケンサ              │  │    │ │ iFFT合成専用ループ     │ │
│ │ - midi_data.h読み出し       │  │    │ │                        │ │
│ │ - 再生位置追跡              │  │    │ │ - active_bins[]走査    │ │
│ └──────────┬──────────────────┘  │    │ │ - 8倍音複素回転子更新  │ │
│            │                     │    │ │ - エンベロープ計算     │ │
│            ↓                     │    │ │ - Q15 sparse iFFT      │ │
│ ┌─────────────────────────────┐  │    │ │ - LPF → PWMデューティ  │ │
│ │ note_on_bin(note, end_ms)   │  │    │ └──────┬─────────────────┘ │
│ │ - bins[note]のtwiddleと     │──┼────┼──→     │                   │
│ │   減衰係数をセット          │  │    │        ↓                   │
│ │ - active_binsに登録         │  │    │  dma_buf[buf_fill][1024]   │
│ └─────────────────────────────┘  │    │        ↓ need_fill=true    │
│                                  │    └────────┼───────────────────┘
│ ┌─────────────────────────────┐  │             ↓
│ │ OLED描画 (30FPS)            │  │    ┌────────────────────────────┐
│ │ - I2C 1MHz Fast-Mode Plus   │  │    │ DMA ch0                    │
│ │ - Q16固定小数点座標計算     │  │    │ - dma_buf[buf_dma] → PWM CC│
│ │ - ピアノロール風落下表示    │  │    │ - 自動ループ、wrap毎にDREQ │
│ └─────────────────────────────┘  │    └─────────┬──────────────────┘
└────────────┬─────────────────────┘              ↓
             │                          ┌────────────────────────────┐
             │ DMA完了IRQ               │ PWM slice1                 │
             │ buf_fill反転、           │ - CC_A → GP2 (正相)        │
             │ need_fill=true           │ - CC_B → GP3 (逆相)        │
             ↓                          │ - wrap=sys_clk/22050=9070  │
                                        │ - 13bit分解能              │
                                        └────────────────────────────┘
```

### 共有メモリ

- **`bins[128]`**: Core0が書き込み、Core1が読み出し。`volatile`で保護
- **`active_bins[16]`**: 発音中ノートのインデックスリスト
- **`dma_buf[2][1024]`**: Core1書き込み、DMA読み出し
- **`shared_playback_ms`**: 現在再生位置(ms)

## ピアノ音色設計

### 倍音プロファイル (`piano_voice.h`)

実ピアノA4近傍のスペクトル測定値を参考にした8倍音プロファイル:

| 倍音 | 相対振幅 | 周波数(A4=440Hz基準) |
|------|----------|----------------------|
| 基音 | 1.00 | 440 Hz |
| 第2倍音 | 0.50 | 880 Hz |
| 第3倍音 | 0.30 | 1320 Hz |
| 第4倍音 | 0.20 | 1760 Hz |
| 第5倍音 | 0.15 | 2200 Hz |
| 第6倍音 | 0.10 | 2640 Hz |
| 第7倍音 | 0.07 | 3080 Hz |
| 第8倍音 | 0.05 | 3520 Hz |

### 音域別減衰時間 (`piano_decay_table.cpp`)

88鍵全てに事前計算された減衰係数を持ちます。実ピアノに近い減衰特性を再現:

| ノート | T60(秒) | Q15係数 |
|--------|---------|---------|
| A0 (note 21) | ~5s | 32765 |
| A4 (note 69) | ~1.7s | 32761 |
| C8 (note 108) | ~0.5s | 32746 |

低音は長く響き、高音ほど短く減衰するピアノ特有の特性。Core0のnote_on_bin()時に`bins[note].decay_q15`へコピーされ、Core1のサンプル合成で使用されます。

### 高倍音先消失

打鍵後、時間経過とともに高倍音(h>=3)が先に消失する処理(`piano_hf_extra[]`)を実装。これによりピアノが時間経過とともに「やさしく」「暗く」なる音色変化を再現します。

### アタック特性

打鍵の急峻な立ち上がりを再現するため、5サンプル(約227μs @ 22050Hz)で0→1.0までattack。これによりピアノのハンマー打鍵感を表現します。

## CPU使用率試算

200MHz, 1サンプル予算45μs:

| 同時発音数 | 演算量(/sample) | 200MHz時の処理時間 |
|-----------|----------------|---------------------|
| 1 | 8倍音×4演算 = 32 | 約2μs |
| 8 | 256演算 | 約13μs |
| 16 | 512演算 | 約25μs |

実機計測: active=4で `fill=約8ms/1024sample = 7.8μs/sample`、予算45μsに対し17%。

## ビルドと書き込み

### 必要環境

- Arduino IDE 2.x
- arduino-pico (Earle Philhower core) 5.6.0以降
- ライブラリ: Adafruit_GFX, Adafruit_SSD1306

### Arduino IDE設定

- Board: **Raspberry Pi Pico** (Earle Philhower)
- CPU Speed: **200 MHz**
- Flash Size: 2MB (default)

### 書き込み手順

picoprobe経由で書き込む場合:

1. picoprobeのSWDをtarget PicoのSWCLK/SWDIOに接続
2. picoprobeのGP4 → target GP1 (RX)、GP5 → target GP0 (TX) でUART接続
3. picoprobeをPCにUSB接続
4. Arduino IDEから「Tools → Programmer → Picoprobe (CMSIS-DAP)」を選択
5. 「アップロード」(Sketch → Upload Using Programmer)

**⚠ 重要**: picoprobe書き込み完了後、**target PicoのUSBケーブルを抜き差ししてpower cycleしてください**。Arduino-Pico既知バグでpicoprobe書き込み直後はCore1が起動しません。

### 動作確認

シリアル(GP0/GP1経由、picoprobeのCDC、115200baud)に以下が出力されます:

```
=== iFFT_Orgel v15 PIANO ===
Harmonic count=8 hweight[0]=14005 hweight[7]=700
sys=200000000 wrap=9069 mid=4534
Pre-filling buffers...
Setup done. Power-cycle target if Core1 not running.
[2s] DIAG active=0 fill=2000us oled=8500us(30FPS) c1=9000000 frm=0/2332
[4s] DIAG active=0 ...
DIAG -> PIANO
[6s] PIANO active=4 fill=8000us oled=8500us(30FPS) c1=27000000 frm=15/2332
```

- 起動5秒間: 440Hz診断モード
- 5秒後: La Campanella自動再生開始
- `c1=` が増加していればCore1動作中
- `oled=` < 33000μs、`(NN FPS)` が30FPS表示なら30FPS達成
- `active=` で現在発音中の音数

## 既知の問題と回避策

### Core1が起動しない

picoprobe経由書き込み直後の既知バグ。targetをpower cycleで解決。

### 音が出ない

1. **配線確認**: GP2/GP3からRC LPF + DCカット経由でアンプ
2. **PWMキャリア確認**: sys_clk/22050 ≈ 9070 で誤差0.014%以下
3. **シリアル出力**: `c1=`が増加していなければCore1停止
4. **同一スライス確認**: GP2/GP3が同じPWMスライス(slice=1)に属していること

### 動作異常時の診断

- `active=0` のまま: MIDIフレーム読み出し失敗(midi_data.hを確認)
- `fill=`が30000μs超: CPU負荷限界、`IFFT_ACTIVE_MAX`を減らす
- `oled=`が30000μs超: 30FPS未達、I2Cクロックを確認
