# Realtime IQ -> PARLIO contract

## Hardware facts we are preserving

1. C5 mode-0 produces packed 32-bit IQ words in the RF dump SRAM at `0x40830000`.
2. Packed format: Q = signed 10-bit bits 0..9, I = signed 10-bit bits 10..19.
3. A finite 16,384-word capture succeeds both with VTX OFF and VTX ON. VTX presence changes the signal statistics, not whether IQ words exist.
4. The finite `adctrig()` wrapper is not a realtime producer. Its whole call includes ownership/poll/teardown overhead around a short RF acquisition.
5. The proven direct restart sequence can rearm the 16K writer repeatedly; measured hardware boundary evidence in C5VRX issue #22 is far smaller than the whole vendor call.

## Realtime architecture

```text
ESP32-C5 RF frontend @ A1 / 5865 MHz
              |
              v
      RF dump SRAM writer
              |
       16K generation boundary
              |
      immediate HW rearm
              |
              +------------------------------+
              |                              |
              v                              v
      producer continues             consumer reads completed
        immediately                     generation / safe slice
                                             |
                                             v
                                    Q10/I10 -> phase8
                                             |
                                             v
                                  dphi[n] = phase[n]-phase[n-4]
                                             |
                                             v
                                     fixed gain + bias
                                             |
                                             v
                                         DAC6 byte
                                             |
                                             v
                                          PARLIO
                                             |
                                             v
                                         D4..D9 DAC
```

Producer and consumer are independent. DSP or PARLIO must never delay RF restart.

## Producer rules

The producer starts automatically after RF initialization and tune to A1/5865. It does not wait for USB, VTX, PAL/NTSC, sync, burst, video classification, power thresholds or a GUI command.

At every 16K boundary, restart must be the first action. No logging, heap allocation, memcpy, hashes, DSP or PARLIO calls are allowed between writer completion and the restart primitive.

First implementation candidate is the already-proven direct sequence:

```text
ENABLE 0
ENABLE 1
START 1
START 0
```

Then benchmark the smaller evidence-based candidate:

```text
START 1
START 0
```

with ENABLE continuously high. Adopt it only after physical hardware confirms equal reliability.

A truly native continuously overwriting ring remains interesting, but must not block the first direct-output experiment. The rearm producer is the current known-good fallback.

## Consumer rules

The consumer must not allocate or copy a full 64 KiB IQ block for every generation.

Initial implementation should use the smallest safe handoff that can be physically proven. Prefer:

1. direct read from a completed/safe RF SRAM generation;
2. small fixed chunks into PARLIO DMA buffers;
3. only if required, a bounded small scratch buffer.

Never add a second giant software IQ ring unless hardware ownership forces it.

## Minimal transform

No PAL/NTSC reconstruction is required for the first hardware proof.

For every fourth IQ sample:

```c
q = sign10(word);
i = sign10(word >> 10);
phase = phase8(i, q);
delta = (int8_t)(phase - previous_phase);
dac = clamp6(BIAS + delta * GAIN);
```

This produces 20 MS/s output from the 80 MS/s represented IQ timebase. The first sample after a missing/unknown generation boundary is neutral/held because discriminator phase across missing RF time is not valid.

Initial `GAIN` and `BIAS` may be fixed constants chosen only to fit the 6-bit DAC safely. Do not run video-analysis or classification to choose them in the realtime path.

## VTX OFF behavior

VTX OFF is a valid operating condition:

```text
VTX OFF -> noise/spurs/ambient RF -> IQ words -> discriminator noise -> analog static
```

The producer must continue indefinitely. A signal detector can later observe this stream, but it cannot stop or gate acquisition.

## USB

USB is diagnostics only. Connecting, disconnecting or stopping USB must not start, stop, rearm, reset or retune the RF producer.

Useful bounded counters:

```text
blocks
rearms
rearm_failures
boundary_cycles_min/avg/max
parlio_underruns
consumer_overruns
```

Do not print in the boundary hot path. Snapshot counters asynchronously.

## First physical acceptance test

1. Boot with VTX OFF.
2. Confirm producer block/rearm counters increase continuously.
3. Confirm PARLIO continues outputting a changing RF-dependent waveform/static.
4. Turn VTX ON without restarting anything.
5. Confirm the same producer continues and output changes immediately.
6. Turn VTX OFF again without restarting anything.
7. Confirm producer continues and output returns to the off-air/noise character.
8. Confirm `rearm_failures=0` and no USB dependency.

Recognizable PAL/NTSC video is explicitly *not* required for this first gate. The gate proves a continuous RF-dependent hardware datapath.

## Explicitly excluded from this branch

- `NO_RF` producer state
- PAL/NTSC analyzer in the hot path
- sync/burst gating
- full-frame raster reconstruction
- GUI-driven capture pipeline
- repeated full vendor `adctrig()` lifecycle per block
- per-block heap allocations
- per-block 64 KiB copy before restart
- USB-controlled RF lifecycle
