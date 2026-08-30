# First hardware gate

This gate is intentionally lower-level than recognizable video.

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
