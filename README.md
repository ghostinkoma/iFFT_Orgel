# iFFT_Orgel
# Pi Pico iFFT Orgel & Visualizer

A dual-core real-time audio synthesizer and OLED visualizer for the Raspberry Pi Pico. 
This project utilizes an Inverse Fast Fourier Transform (iFFT) approach for additive synthesis, currently configured to play Franz Liszt's "La Campanella" with a beautiful, mechanical music box (orgel) sound profile.

## Features

* Dual-Core Processing
  * Core 0: Manages the OLED display, rendering the title screen and a cascading note visualizer synchronized with the music.
  * Core 1: Dedicated to real-time audio synthesis via PWM (iFFT, harmonic mixing, envelope processing) and MIDI sequence timing.
* iFFT Additive Synthesis & Physical Modeling
  * Generates sound by combining a fundamental frequency with 2nd and 3rd harmonics to create a rich, metallic resonance characteristic of a music box comb.
  * Implements a very sharp attack (simulating the plucking of a metal tooth) and a long exponential decay for a lingering, bell-like sustain.
  * Includes a low-pass filter (LPF) to round out harsh high frequencies.
* Seamless Looping
  * Automatically transitions back to the title screen upon completion of the sequence, pauses for 3 seconds, and restarts the performance.

## Hardware Requirements

* Raspberry Pi Pico (RP2040)
* SSD1306 OLED Display (128x64 resolution, I2C interface)
* Audio Output: Piezo buzzer or an amplified speaker (connected via PWM pins)

## Software Dependencies

To compile this project, install the following board package and libraries in the Arduino IDE:

* Board Manager: Raspberry Pi Pico/RP2040 by Earle F. Philhower, III
* Libraries:
  * Adafruit GFX Library
  * Adafruit SSD1306

## Project Structure

* sketch_name.ino : The main program handling dual-core setup, OLED rendering (loop), and audio generation (loop1).
* config.h : Configuration file for pin assignments, I2C addresses, audio parameters, and synthesizer timbre.
* midi_data.h : Contains the MIDI sequence data (notes, timestamps, durations) stored in PROGMEM.

## Setup & Usage

1. Clone or download this repository.
2. Open the `.ino` file in the Arduino IDE.
3. Verify that the pin definitions in `config.h` (CFG_I2C_SDA, CFG_I2C_SCL, CFG_AUDIO_PIN_P, CFG_AUDIO_PIN_N, etc.) match your physical wiring.
4. Select "Raspberry Pi Pico" from the Arduino IDE Tools > Board menu.
5. Compile and upload the sketch to your Pico.

## Customization (Orgel Tuning)

You can freely adjust the sound profile in `config.h`. To emphasize the "music box" feel, we recommend highlighting the higher harmonics (for a metallic chime) and keeping a sharp attack:

```cpp
// Recommended settings for a Music Box (Orgel) tone
#define CFG_TIMBRE_H2        0.4f    // 2nd harmonic (warmth)
#define CFG_TIMBRE_H3        0.6f    // 3rd harmonic (stronger for a metallic/bell-like ring)
#define CFG_ENV_ATTACK_SAMP  10      // Very sharp attack (plucking the metal comb)
#define CFG_ENV_DECAY_COEF   0.9995f // Long, beautiful sustain
#define CFG_AUDIO_LPF_ALPHA  0.6f    // Slightly open LPF to let the high frequencies shine
