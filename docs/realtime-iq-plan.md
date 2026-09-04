# Realtime IQ to recovered-CVBS contract

## Source

The production source is an autonomous, pre-trigger RF dump ring:

```text
SRAM base   0x40830000
size        0x10000 bytes
format      Q10 bits 0..9, I10 bits 10..19
pointer     0x600a9008[15:0]
enable      0x600a9004[31]
length      0x600a9004[16:0]
```

The C5 vendor call `adctrig(16383, 5, 0, 1, 1, 0, 0, 0, 0)` is used only as
an oracle. Its recovered C5 state selects TX_START with `0x00060000` in the
new selector field and clears historical control bit 17 for `dump_trig=1`.
Production reproduces that state directly, selects a trigger that never occurs,
grants SRAM and sets ENABLE once. It deliberately omits the wrapper's trigger,
DONE wait, timeout, disable, restore and periodic rearm lifecycle.

`continuous_iq_acquire()` returns only physically contiguous spans. Logical
producer/consumer positions continue through address 16383 -> 0. A missed or
ambiguous wrap is exposed as a discontinuity; it is never hidden by holding or
inventing samples.

## DSP and output

For every adjacent pair, including across normal DMA/ring boundaries:

```text
d[n] = arg(x[n] * conj(x[n-1]))
```

The streaming C5 implementation uses a compact signed-I/Q LUT approximation in
the BitScrambler. It processes all adjacent samples, accumulates four real FM
results, then emits their boxcar average. The host validator keeps the exact
floating-point conjugate-product reference and reports adjacent, post-filter
and +/-pi-seam errors so the optimized LUT is never accepted blindly.

RF and AV rates are separate state. Startup measures the RF writer cadence;
PARLIO is configured for one output per four discriminator samples. This first
hardware implementation is a fixed-ratio real-domain boxcar/decimator. Any
future arbitrary ratio belongs after FM and must not discard complex IQ first.

PARLIO hardware loop mode mounts a cyclic GDMA chain once. Its BitScrambler
state and source address continue over the physical SRAM wrap. No PAL frame,
framebuffer, finite transaction restart or per-block memory copy exists in the
live path.

## Realtime and USB rules

- No `vTaskDelay`, allocation, logging or USB operation occurs in RF/DSP pacing.
- VTX OFF remains valid continuous noise IQ; signal presence never gates RF.
- Previous-IQ state resets only on a reported real discontinuity or retune.
- USB remains enabled and scheduled on the HP CPU; a low-priority task samples
  telemetry once per second.
- The old MSTATUS interrupt mask, HP parking, LP rearm and 30-second terminal
  postmortem loop are removed.

## Proof boundary

Software construction proves neither hidden RF sample continuity nor exact
PARLIO boundary timing. The required physical proofs are listed in
`docs/hardware-test.md`.
