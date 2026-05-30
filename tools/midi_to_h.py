# -*- coding: utf-8 -*-
import argparse
import os
from mido import MidiFile, merge_tracks, tick2second

def midi_to_c_header(midi_path, output_h_path):
    if not os.path.exists(midi_path):
        print("Error: File not found.")
        return

    mid = MidiFile(midi_path) 
    merged_track = merge_tracks(mid.tracks)
    
    current_time_ms = 0.0
    current_tempo = 500000 
    
    active_notes = {}
    raw_notes = []

    # 1. まず全MIDIメッセージを走査して単一のノートイベントを抽出
    for msg in merged_track:
        if msg.time > 0:
            seconds = tick2second(msg.time, mid.ticks_per_beat, current_tempo)
            current_time_ms += seconds * 1000.0
        
        if msg.is_meta and msg.type == 'set_tempo':
            current_tempo = msg.tempo
            
        if msg.type == 'note_on' and msg.velocity > 0:
            active_notes[msg.note] = current_time_ms
        elif (msg.type == 'note_off') or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note in active_notes:
                start_time = active_notes[msg.note]
                duration = current_time_ms - start_time
                if duration > 0:
                    raw_notes.append({
                        "start_ms": int(start_time),
                        "note": msg.note,
                        "duration_ms": int(duration)
                    })
                del active_notes[msg.note]

    if not raw_notes:
        print("Error: No valid notes found.")
        return

    # 2. 同一の「発音時刻」でグルーピング（最大24和音のチャンク化）
    grouped_chunks = {}
    for note in raw_notes:
        t = note["start_ms"]
        if t not in grouped_chunks:
            grouped_chunks[t] = []
        if len(grouped_chunks[t]) < 24: # 最大24和音まで格納
            grouped_chunks[t].append(note)

    # 時系列（発音時刻順）にソート
    sorted_times = sorted(grouped_chunks.keys())

    # .h ファイルの出力
    with open(output_h_path, 'w') as f:
        f.write("/* 24-Polyphonic Chunked MIDI Data */\n")
        f.write("#ifndef MIDI_DATA_H\n")
        f.write("#define MIDI_DATA_H\n\n")
        f.write('#include <Arduino.h>\n\n')
        
        f.write("struct NoteChunk {\n")
        f.write("  uint8_t  note;        // 鍵盤番号 (0の場合は無効スロット)\n")
        f.write("  uint16_t duration_ms; // 音符の長さ (uint16_t)\n")
        f.write("};\n\n")
        
        f.write("struct MidiSequenceFrame {\n")
        f.write("  uint32_t start_ms;     // シーケンス時間軸 (uint32_t)\n")
        f.write("  NoteChunk poly_notes[24]; // 24倍確保されたパラレルバッファ\n")
        f.write("};\n\n")
        
        f.write("const int TOTAL_FRAMES = %d;\n\n" % len(sorted_times))
        f.write("const MidiSequenceFrame midi_sequence[] PROGMEM = {\n")
        
        for idx, t in enumerate(sorted_times):
            notes_in_t = grouped_chunks[t]
            f.write("  {\n")
            f.write("    %d,\n" % t) # start_ms
            f.write("    {\n")
            
            # 24個の固定チャンクスロットを埋める
            for i in range(24):
                if i < len(notes_in_t):
                    n = notes_in_t[i]
                    f.write("      {%d, %d}" % (n["note"], n["duration_ms"]))
                else:
                    f.write("      {0, 0}") # 空きスロットは無効値0で埋める
                
                if i < 23:
                    f.write(",\n")
                else:
                    f.write("\n")
                    
            f.write("    }\n")
            if idx < len(sorted_times) - 1:
                f.write("  },\n")
            else:
                f.write("  }\n")
                
        f.write("};\n\n")
        f.write("#endif\n")
        
    print("[SUCCESS] Chunked into %d sequential frames (24-poly per frame)." % len(sorted_times))

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-i", "--input", required=True)
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args()
    midi_to_c_header(args.input, args.output)
