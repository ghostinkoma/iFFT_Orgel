# MIDI to C-Header Converter

A utility tool designed to convert standard MIDI files (`.mid`) into a C/C++ header file (`.h`). This generated header contains the note data, timestamps, and durations formatted as PROGMEM arrays, which can be directly read by the Pi Pico Dual-Core Synthesizer.

## Prerequisites

To run this conversion tool, you will need the following installed on your system:

* Python 3.6 or higher
* The `mido` library (for parsing MIDI files)

You can install the required library using pip:

```bash
pip install mido


python3 convert_midi.py -i input_file.mid  -o output_file.h
