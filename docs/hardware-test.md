# Hardware tests still required

## Wiring

| XIAO | GPIO | Series resistor to VIDEO |
|---|---:|---:|
| D4 | 23 | 8.2 kOhm |
| D5 | 24 | 3.9 kOhm |
| D6 | 11 | 2.0 kOhm |
| D7 | 12 | 1.0 kOhm |
| D8 | 8 | 470 Ohm |
| D9 | 9 | 240 Ohm |

Join the six resistors at VIDEO, fit 200 Ohm VIDEO-to-GND, share ground and use
the normal 75 Ohm receiver termination. Expected loaded levels are roughly
code 0 = 0 V, code 18 = 0.30 V and code 62 = 1.0 V.

## 1. Static AV levels

Build `C5VRX2_MODE_AV_STATIC` six times with codes 0, 18, 31, 32, 62 and 63.
Measure VIDEO into 75 Ohm. This isolates GPIO mapping and analog levels from RF.

## 2. PARLIO continuity and PAL diagnostics

Run `C5VRX2_MODE_AV_PAL_MONO`, then `C5VRX2_MODE_AV_PAL_COLOR`. Scope the
buffer-switch boundaries and verify no idle, duplicated, missing or stretched
DAC sample. Confirm stable sync and burst at the actual 20 MHz diagnostic rate.

## 3. Vendor pre-trigger oracle

Run `C5VRX2_MODE_RF_ORACLE` with no Wi-Fi TX. It calls exactly:

```c
adctrig(16383, 5, 0, 1, 1, 0, 0, 0, 0);
```

Verify pointer changes, repeated wraps, RAM mutation, ENABLE remaining set and
DONE remaining clear. Repeat VTX OFF -> ON -> OFF without rearming.

## 4. Physical IQ-wrap continuity

Feed a coherent RF tone and run `C5VRX2_MODE_RF_WRAP`. It collects 256 windows
around 16383 -> 0 and reports boundary versus neighboring adjacent phase. Save
the raw/summary evidence and statistically compare it; the firmware deliberately
prints `CAPTURED_NOT_YET_PROVEN` rather than claiming gaplessness itself.

## 5. Live recovered CVBS

Flash the default live mode. At 115200 expect `LIVE START` followed by one-second
`LIVE` telemetry with changing pointer, `enabled=1`, `done=0`, and no reset or
USB disappearance. Repeat VTX OFF -> ON -> OFF, then verify composite sync,
blanking, burst and picture on a scope/decoder. Record measured RF rate and the
requested/actual PARLIO rate; check long-run reader/writer drift as well as the
physical SRAM and GDMA boundaries.
