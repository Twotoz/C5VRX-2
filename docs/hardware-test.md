# First hardware gate

This gate is intentionally lower-level than recognizable video.

## Physical XIAO AV wiring

Use the resistor network that is actually fitted to the current XIAO ESP32-C5 test hardware:

| XIAO | GPIO | Series resistor to VIDEO |
|---|---:|---:|
| D4 | 23 | 8.2 kOhm |
| D5 | 24 | 3.9 kOhm |
| D6 | 11 | 2.0 kOhm |
| D7 | 12 | 1.0 kOhm |
| D8 | 8 | 470 Ohm |
| D9 | 9 | 240 Ohm |

All six meet at `VIDEO`. Fit 200 Ohm from `VIDEO` to GND. XIAO and FatShark/video ground must be common; the goggles/monitor provide the normal 75 Ohm termination.

These are intentionally the current real component values. Older C5VRX calculations used near-ideal 7.87k / 3.92k / 1.96k / 976R / 487R / 243R with a 191R shunt; do not substitute those values for this hardware test.

## Test

1. Boot XIAO ESP32-C5 with VTX OFF.
2. RF initializes and tunes A1 / 5865 MHz automatically.
3. IQ producer starts automatically; no USB command is required.
4. Observe asynchronous counters only: blocks, rearms, rearm_failures, boundary cycles, PARLIO underruns.
5. Confirm counters continue with VTX OFF.
6. Turn VTX ON while the producer remains armed.
7. Confirm output waveform/statistics change without any capture/start/rearm command from USB.
8. Turn VTX OFF again; producer must continue.
9. Accept only if rearm_failures remain zero and RF lifecycle is independent of USB and video classification.

Recognizable video is a follow-on gate after the producer/consumer stream is physically proven.
