# iFFT_Orgel — RP2040 Piano Synthesizer

## Features

- **True iFFT Space Architecture**: Sparse iFFT synthesis using MIDI note numbers as bin indices. The superposition principle eliminates distortion during polyphonic playback
- **Piano Voicing**: 8-harmonic additive synthesis, register-dependent decay times (bass ~5s, treble ~0.5s), progressive high-harmonic rolloff for timbral evolution over time
- **Q15 Fixed-Point Arithmetic**: Integer-only DSP pipeline with no floating-point division in the audio path
- **Dual-Core Partitioning**: Core0 handles MIDI sequencing and OLED display; Core1 is dedicated to iFFT synthesis
- **DMA + Hardware PWM**: Differential 13-bit PWM output with zero-CPU audio streaming via DMA
- **30 FPS OLED**: Piano-roll style falling-note visualization

## Hardware Configuration

| Signal | GPIO | Connection |
|--------|------|------------|
| PWM positive | GP2 | Amplifier positive input (via 1kΩ + 100nF LPF) |
| PWM negative | GP3 | Amplifier negative input (via 1kΩ + 100nF LPF) |
| I2C SDA | GP4 | SSD1306 OLED |
| I2C SCL | GP5 | SSD1306 OLED |
| UART TX | GP0 | Picoprobe RX (debug output) |
| UART RX | GP1 | Picoprobe TX |

The differential PWM outputs should be connected through 1kΩ series resistors + 100nF low-pass filter capacitors + 10µF DC-blocking coupling capacitors to a BTL amplifier or speaker.

## File Structure

```
iFFT_Orgel/
├── iFFT_Orgel.ino         Main sketch (Core0/Core1, piano voice data inlined)
├── config.h               Hardware pin definitions
├── midi_data.h            La Campanella MIDI sequence
└── README.md              This file
```

## Architecture

```
┌──────────────────────────────────┐    ┌────────────────────────────┐
│ Core0                            │    │ Core1                      │
│                                  │    │                            │
│ ┌─────────────────────────────┐  │    │ ┌────────────────────────┐ │
│ │ MIDI Sequencer              │  │    │ │ iFFT Synthesis Loop    │ │
│ │ - Read midi_data.h          │  │    │ │                        │ │
│ │ - Track playback position   │  │    │ │ - Scan active_bins[]   │ │
│ └──────────┬──────────────────┘  │    │ │ - Update 8-harmonic    │ │
│            │                     │    │ │   complex rotators     │ │
│            ↓                     │    │ │ - Envelope calculation │ │
│ ┌─────────────────────────────┐  │    │ │ - Q15 sparse iFFT     │ │
│ │ note_on_bin(note, end_ms)   │  │    │ │ - LPF → PWM duty      │ │
│ │ - Set twiddle factors and   │──┼────┼──→     │                │ │
│ │   decay coefficient in      │  │    │ └──────┬─────────────────┘ │
│ │   bins[note]                │  │    │        ↓                   │
│ │ - Register in active_bins   │  │    │  dma_buf[buf_fill][1024]   │
│ └─────────────────────────────┘  │    │        ↓ need_fill=true    │
│                                  │    └────────┼───────────────────┘
│ ┌─────────────────────────────┐  │             ↓
│ │ OLED Display (30 FPS)       │  │    ┌────────────────────────────┐
│ │ - I2C 1MHz Fast-Mode Plus   │  │    │ DMA ch0                    │
│ │ - Q16 fixed-point coords    │  │    │ - dma_buf[buf_dma] → PWM CC│
│ │ - Falling-note piano roll   │  │    │ - Auto-loop, DREQ per wrap │
│ └─────────────────────────────┘  │    └─────────┬──────────────────┘
└────────────┬─────────────────────┘              ↓
             │                          ┌────────────────────────────┐
             │ DMA completion IRQ       │ PWM slice1                 │
             │ Swap buf_fill,           │ - CC_A → GP2 (positive)    │
             │ set need_fill=true       │ - CC_B → GP3 (negative)    │
             ↓                          │ - wrap = sys_clk/22050     │
                                        │         = 9070 (13-bit)    │
                                        └────────────────────────────┘
```

### Shared Memory

- **`bins[128]`**: Written by Core0, read by Core1. Protected with `volatile`
- **`active_bins[16]`**: Index list of currently sounding notes
- **`dma_buf[2][1024]`**: Written by Core1, read by DMA
- **`shared_playback_ms`**: Current playback position (ms)

## Piano Voice Design

### Harmonic Profile

8-harmonic profile modeled after measured spectra of a real piano near A4:

| Harmonic | Relative Amplitude | Frequency (A4 = 440 Hz) |
|----------|--------------------|-------------------------|
| Fundamental | 1.00 | 440 Hz |
| 2nd harmonic | 0.80 | 880 Hz |
| 3rd harmonic | 0.40 | 1320 Hz |
| 4th harmonic | 0.25 | 1760 Hz |
| 5th harmonic | 0.18 | 2200 Hz |
| 6th harmonic | 0.10 | 2640 Hz |
| 7th harmonic | 0.06 | 3080 Hz |
| 8th harmonic | 0.04 | 3520 Hz |

### Register-Dependent Decay

Pre-computed decay coefficients for all 88 keys, reproducing the characteristic decay behavior of a real piano:

| Note | T60 (seconds) | Q15 Coefficient |
|------|---------------|-----------------|
| A0 (note 21) | ~5 s | 32765 |
| A4 (note 69) | ~1.7 s | 32761 |
| C8 (note 108) | ~0.5 s | 32746 |

Bass notes ring longer while treble notes decay quickly — a defining characteristic of piano acoustics. The coefficient is copied into `bins[note].decay_q15` during `note_on_bin()` on Core0 and used by Core1 during sample synthesis.

### Progressive High-Harmonic Rolloff

After note onset, higher harmonics (h ≥ 3) are progressively attenuated over time via per-harmonic extra decay factors (`piano_hf_extra[]`). This reproduces the way a piano tone becomes "softer" and "darker" as it sustains.

### Attack Characteristics

A rapid linear attack of 22 samples (~1 ms at 22050 Hz) from silence to full amplitude reproduces the percussive feel of a piano hammer striking the string.

## CPU Utilization

At 200 MHz, per-sample budget is 45 µs:

| Polyphony | Operations/sample | Processing Time at 200 MHz |
|-----------|-------------------|----------------------------|
| 1 voice | 8 harmonics × 4 ops = 32 | ~2 µs |
| 8 voices | 256 ops | ~13 µs |
| 16 voices | 512 ops | ~25 µs |

Measured on hardware: active=4 yields `fill ≈ 8 ms / 1024 samples = 7.8 µs/sample`, approximately 17% of the 45 µs budget.

## Build and Upload

### Requirements

- Arduino IDE 2.x
- arduino-pico (Earle Philhower core) 5.6.0 or later
- Libraries: Adafruit_GFX, Adafruit_SSD1306

### Arduino IDE Settings

- Board: **Raspberry Pi Pico** (Earle Philhower)
- CPU Speed: **200 MHz**
- Flash Size: 2MB (default)

### Upload Procedure

When uploading via picoprobe:

1. Connect picoprobe SWD to target Pico's SWCLK/SWDIO
2. Connect picoprobe GP4 → target GP1 (RX), GP5 → target GP0 (TX) for UART
3. Connect picoprobe to PC via USB
4. In Arduino IDE: Tools → Programmer → Picoprobe (CMSIS-DAP)
5. Upload (Sketch → Upload Using Programmer)

**⚠ Important**: After uploading via picoprobe, **power-cycle the target Pico by unplugging and re-plugging its USB cable**. This is a known arduino-pico bug where Core1 does not start after a picoprobe upload.

### Verifying Operation

Serial output (via GP0/GP1 through picoprobe CDC at 115200 baud):

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

- First 5 seconds: 440 Hz diagnostic tone
- After 5 seconds: La Campanella auto-playback begins
- `c1=` incrementing confirms Core1 is running
- `oled=` < 33000 µs with `(30FPS)` confirms 30 FPS display
- `active=` shows the number of currently sounding voices

## Known Issues and Workarounds

### Core1 Does Not Start

Known arduino-pico bug when uploading via picoprobe. Resolved by power-cycling the target.

### No Audio Output

1. **Check wiring**: GP2/GP3 must go through RC LPF + DC-blocking caps to an amplifier
2. **Verify PWM carrier**: sys_clk / 22050 ≈ 9070 with error < 0.014%
3. **Check serial output**: If `c1=` is not incrementing, Core1 is not running
4. **Verify same PWM slice**: GP2 and GP3 must map to the same PWM slice (slice 1)

### Diagnostic Indicators

- `active=0` persists: MIDI frame readout failure — check midi_data.h
- `fill=` exceeds 30000 µs: CPU overload — reduce `IFFT_ACTIVE_MAX`
- `oled=` exceeds 30000 µs: 30 FPS not achieved — verify I2C clock setting
