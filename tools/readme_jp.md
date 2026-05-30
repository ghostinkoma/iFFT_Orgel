# MIDI to C-Header Converter

標準のMIDIファイル（`.mid`）をC/C++のヘッダーファイル（`.h`）に変換するためのユーティリティツールです。生成されたヘッダーファイルには、Pi Picoデュアルコアシンセサイザーで直接読み込むことができるよう、ノートデータ、タイムスタンプ、音の長さ（デュレーション）がPROGMEM配列としてフォーマットされて格納されます。

## 前提条件 (Prerequisites)

この変換ツールを実行するには、システムに以下がインストールされている必要があります。

* Python 3.6 以上
* `mido` ライブラリ (MIDIファイルの解析用)

必要なライブラリは pip を使用してインストールできます。

```bash
pip install mido

python3 convert_midi.py -i input_file.mid -o output_file.h

