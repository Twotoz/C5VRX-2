# First hardware gate

This gate is intentionally lower-level than recognizable video.

## Flash the exact PR build

1. Open the latest successful **C5VRX-2 realtime build** run for this PR.
2. Download and unzip artifact `c5vrx2-full`.
3. Verify `c5vrx2-full.bin` against `c5vrx2-full.bin.sha256`.
4. Serve the unzipped directory from localhost (for example with
   `python -m http.server 8000`) and open `http://localhost:8000/flasher.html`
   in Chrome or Edge.
5. If the XIAO is not detected automatically, hold **BOOT**, press and release
   **RESET**, then release **BOOT**.
6. Select `c5vrx2-full.bin`, connect the bootloader and flash it at `0x0`.

The bundle's `FIRMWARE-COMMIT.txt` identifies the exact PR commit under test.

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

1. Power the FatShark/XIAO with the VTX OFF and reset the XIAO.
2. Optionally open the 115200 terminal immediately after reset. Before the HP
   core parks, it must print the A1 / 5865 MHz tune and `REALTIME ARM` lines.
3. Confirm the FatShark AV input is active with a changing static-like waveform.
   The IQ producer starts automatically; no USB command is required.
4. Turn the A1 / 5865 MHz VTX ON without resetting or reconnecting USB.
5. Confirm the physical AV output changes while the producer remains running.
6. Turn the VTX OFF again and confirm the output continues instead of stopping.
7. Leave it running for at least 60 seconds and repeat OFF -> ON -> OFF once.
8. Reject the run if USB prints `REALTIME STOP`, the AV output freezes/stops, or
   the XIAO resets. A `REALTIME STOP` line includes the stored rearm/fault data.

During a healthy run the HP core is deliberately parked while the MAC owns RF
SRAM. Live counters and periodic USB logs are therefore **not** expected. The
first gate is continuous, RF-dependent physical AV output plus absence of a
stored terminal fault; it is not a live telemetry test.

Recognizable video is a follow-on gate after the producer/consumer stream is physically proven.
