# C5VRX-2 agent context

This repository is a clean realtime reset of `Twotoz/C5VRX`, not an unrelated receiver project.

## Historical source of truth

Use the old repository as a hardware/reverse-engineering donor when needed:

- repository: `https://github.com/Twotoz/C5VRX`
- proven finite-capture baseline: commit `d17b2c56f1b6bb2973af5f96d60a6fa0e7b58837`
- build label at that point: `video-proof-24-polarity-locked-hsync`
- that baseline proved A1 / 5865 MHz mode-0 16K packed Q10/I10 captures and host-side video proof.

Important: proof24 "live" IQ was host-chained finite `CAPTURE 16384` requests. It was not true chip-side continuous acquisition.

Later C5VRX work proved the fast one-shot rearm primitive and REGDMA/LP-core machinery. Reuse those low-level proven primitives, but do **not** blindly port the later receiver state machine, video classifier, `NO_RF` gating, large per-block copies, or USB capture pipeline.

The current architecture contract is:

```text
A1 / 5865 MHz
-> mode-0 packed Q10/I10 writer
-> fixed 16K RF SRAM
-> immediate REGDMA rearm at DONE + PTR=16383
-> direct consumer
-> minimal phase/dphi 4:1 transform
-> 20 MS/s PARLIO
-> XIAO D4..D9 resistor DAC
```

VTX presence is never a prerequisite for IQ. VTX OFF still produces valid changing IQ and must not stop the producer.

## Exact current XIAO ESP32-C5 AV hardware

Use the **current physical resistor network below**, not the older near-ideal resistor values from C5VRX documentation.

| XIAO pin | ESP32-C5 GPIO | Current series resistor to VIDEO node |
|---|---:|---:|
| D4 | GPIO23 | 8.2 kOhm |
| D5 | GPIO24 | 3.9 kOhm |
| D6 | GPIO11 | 2.0 kOhm |
| D7 | GPIO12 | 1.0 kOhm |
| D8 | GPIO8 | 470 Ohm |
| D9 | GPIO9 | 240 Ohm |

All six resistor outputs join at the same `VIDEO` node.

- `VIDEO -> GND`: 200 Ohm
- XIAO ground and FatShark/video ground must be common.
- The goggles/monitor provide the normal 75 Ohm video termination.

Older C5VRX calculations used approximately:

- 7.87 kOhm
- 3.92 kOhm
- 1.96 kOhm
- 976 Ohm
- 487 Ohm
- 243 Ohm
- 191 Ohm shunt to ground

Those are **not** the currently fitted values. Do not silently substitute them for the current 8.2k / 3.9k / 2k / 1k / 470R / 240R + 200R network.

## Realtime rules

- Do not gate IQ production on PAL/NTSC, sync, burst, RF power, VTX detection, or USB.
- Do not repeat the complete vendor `adctrig()` lifecycle per 16K generation.
- At a one-shot boundary, launch rearm before telemetry, counters, logging, DSP, or consumer bookkeeping.
- Do not insert a full 64 KiB per-block memcpy in the hot path.
- Keep USB/terminal out of the realtime datapath.
- Do not change D4..D9 GPIO mapping or DAC resistor assumptions unless the physical hardware is explicitly changed.
- Recognizable video is a later acceptance gate. First prove uninterrupted RF-dependent output across VTX OFF -> ON -> OFF.
