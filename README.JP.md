# iFFT Orgel & Visualizer

Raspberry Pi Picoのデュアルコアを活用した、リアルタイムオーディオ波形合成およびOLEDビジュアライザーです。
このプロジェクトは逆高速フーリエ変換（iFFT）を用いた加算合成を活用しており、現在はフランツ・リストの「ラ・カンパネラ（La Campanella）」を美しく機械的なオルゴール風の音色で演奏するように設定されています。

## 特徴 (Features)

* デュアルコア処理
  * Core 0: OLEDディスプレイへのタイトル画面および、音楽と同期して流れ落ちるノーツ（ビジュアライザー）の描画を担当。
  * Core 1: PWMを利用したリアルタイムオーディオ波形合成（iFFT、倍音合成、エンベロープ処理）とMIDIシーケンスの進行管理を専任で担当。
* iFFT加算合成と物理モデリング
  * 基音に2倍音・3倍音を組み合わせることで、オルゴールの櫛歯（くしば）特有の金属的な豊かな響きを生成します。
  * 非常に鋭いアタック（金属の歯を弾く瞬間）と、長く指数関数的な減衰を実装し、鐘のように美しく伸びる余韻を再現します。
  * 耳障りな高音を抑えるためのローパスフィルター（LPF）を搭載しています。
* シームレスなループ再生
  * 曲が終了すると自動的にタイトル画面へ戻り、3秒待機後に再び演奏を開始します。

## ハードウェア要件 (Hardware Requirements)

* Raspberry Pi Pico (RP2040搭載ボード)
* SSD1306 OLED ディスプレイ (128x64解像度, I2C接続)
* オーディオ出力: 圧電スピーカー、またはアンプ付きスピーカー（PWMピン経由で接続）

## ソフトウェア依存関係 (Software Dependencies)

このプロジェクトをコンパイルするには、Arduino IDEに以下のライブラリとボードパッケージをインストールする必要があります。

* ボードマネージャ: Raspberry Pi Pico/RP2040 by Earle F. Philhower, III
* ライブラリ:
  * Adafruit GFX Library
  * Adafruit SSD1306

## プロジェクト構成 (Project Structure)

* sketch_name.ino : デュアルコアのセットアップ、OLED描画（loop）、オーディオ生成（loop1）を処理するメインプログラム。
* config.h : ピン配置、I2Cアドレス、オーディオパラメータ、シンセサイザーの音色設定をまとめたファイル。
* midi_data.h : PROGMEMに格納されたMIDIシーケンスデータ（音符、発音タイミング、長さ）を含むファイル。

## セットアップと実行 (Setup & Usage)

1. リポジトリをクローンまたはダウンロードします。
2. Arduino IDEで `.ino` ファイルを開きます。
3. config.h 内のピン設定 (CFG_I2C_SDA, CFG_I2C_SCL, CFG_AUDIO_PIN_P, CFG_AUDIO_PIN_N など) が、ご自身の配線と一致しているか確認します。
4. Arduino IDEのツールメニューから「Raspberry Pi Pico」を選択します。
5. コンパイルしてPicoに書き込みます。

## カスタマイズ（オルゴール風チューニング）

config.h の数値を変更することで、音色を自由に調整できます。「オルゴールらしさ」を強調するには、高次の倍音（金属的な響き）を立たせ、アタックを鋭く保つ設定をおすすめします。

```cpp
// オルゴール風のおすすめ設定例
#define CFG_TIMBRE_H2        0.4f    // 2倍音（音の温かみ）
#define CFG_TIMBRE_H3        0.6f    // 3倍音（強めにすることで金属的・ベルのような響きに）
#define CFG_ENV_ATTACK_SAMP  10      // 非常に鋭いアタック（金属の歯を弾く感覚）
#define CFG_ENV_DECAY_COEF   0.9995f // 美しく長い余韻
#define CFG_AUDIO_LPF_ALPHA  0.6f    // 高音の響きを活かすためLPFを少し開く
