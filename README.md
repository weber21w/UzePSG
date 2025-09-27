# UzePSG — SN76489 / AY-3-8910 (YM2149) VGM Player for Uzebox

A lightweight **PSG VGM player** for **Uzebox** that emulates **up to 2× SN76489 and 2× AY-3-8910/YM2149 simultaneously**, streaming **.VGM** from **SPI RAM** while mixing audio in real time. The SD card is used only to **load/fill SPI RAM**, not for direct playback.

> **Status:** Working VGM player (dual-chip capable). **VGZ (gzip) not yet implemented**.

---

## Features

- **Chips supported (dual):** up to **2× SN76489** (3 tone + noise each) and **2× AY-3-8910/YM2149** (3 tone + shared noise + envelope each).
- **Auto-detect dual setups:** via VGM header bit-30 **and** observed writes (“seen” flags).
- **Real-time VGM parser:** SN (`0x50`), AY#0/AY#1 (`0xA0/0xA1`), waits (`0x61/0x62/0x63/0x70..0x7F`), end (`0x66`), fast skip for data blocks.
- **SPI-RAM streaming path** with small read-ahead cache; **all positions are file-relative** for reliable reloads.
- **Per-frame mixer** at Uzebox line rate with optional dual-mix autogain and light soft-knee to reduce clipping.
- File browser UI, pause/play/FF, on-screen timer, master volume.

## Systems this can play VGM for (PSG-only titles)

> Listed systems use **SN76489 family** or **AY-3-8910/YM2149 family** as their audio hardware.  
> We **exclude** systems where the music requires another non-PSG chip (e.g., FM synths).

**SN76489 / 96 family**
- **Sega SG-1000 / SC-3000**
- **Sega Master System / Mark III** (PSG; **FM module off / not required**)
- **Sega Game Gear** (PSG core with stereo panning)
- **ColecoVision**
- **TI-99/4A**
- **BBC Micro (Model B / Master)**
- **IBM PCjr / Tandy 1000** (“Tandy 3-voice”, SN-compatible)

**AY-3-8910 / YM2149 family (incl. AY-3-8912/14)**
- **ZX Spectrum 128/+2/+3** (AY-3-8912)
- **Amstrad CPC 464/664/6128** (AY-3-8912)
- **MSX (base PSG)** — AY/YM PSG **without FM/SCC expansions**
- **Atari ST (ST/STF/STM)** (YM2149; base PSG-only titles)
- **Oric-1 / Atmos** (AY-3-8912)
- **Vectrex** (AY-3-8912)
- **Intellivision** (AY-3-8914)
- **Tatung Einstein** (AY-3-8910)
- **Amstrad PCW** (AY-3-8912)

*If a VGM uses only these PSGs (and optional dual instances), UzePSG can play it. VGMs that include other chips (e.g., Yamaha FM/OPN, custom DACs) are out of scope.*

## Requirements

- **Uzebox** with **≥64 KiB SPI RAM** (≥128 KiB recommended for larger VGMs)
- SD card (FAT) with **Petit FatFs**
- **AVR-GCC** toolchain compatible with Uzebox

## Building

1. Install the Uzebox SDK/toolchain.  
2. Place sources in a project directory.  
3. `make` to build the ROM.

## Usage

1. Copy **`.VGM`** files to SD (VGZ not yet supported).  
2. Boot Uzebox → file selector → pick a track.  
3. Playback streams from **SPI RAM**; loader fills from SD. Controls: pause/play/FF, master volume. Timer shows **MM:SS:ff**.

## Technical Notes

- **Mixer rate:** `262 × 60 = 15 720 Hz`.  
- **Clocks:** Read from VGM header (bit-30 = dual). Zero clocks fall back to defaults.  
- **Timing:** 44.1 kHz waits → 15.72 kHz samples with fixed-point carry; tempo scaling via `tempo_q8_8`.  
- **Dual handling:** Header advertises; “seen” writes confirm; small autogain when multiple chips are active.  
- **Amplitude law:** AY vs YM2149 tables; optional dual half-gain tables; soft-knee safety limiter.

## Roadmap

- **VGZ** (gzip) decompression
- Per-song gain suggestion from header
- Optional DC-block / alternate clip curves
- Metadata/title extraction
- Playlists & UI polish

## License

**GPL-3.0**.
