# Realtime IQ to recovered-CVBS contract

## Source

The proven RF producer is an autonomous, pre-trigger RF dump ring:

```text
SRAM base   0x40830000
size        0x10000 bytes
format      Q10 bits 0..9, I10 bits 10..19
pointer     0x600a9008[15:0]
enable      0x600a9004[31]
length      0x600a9004[16:0]
```

The C5 vendor call `adctrig(16383, 5, 0, 1, 1, 0, 0, 0, 0)` was used as an
oracle for the C5 TX_START selector (`0x00060000`). Disassembly showed that
this C5 vendor wrapper does not consume its historical `dump_trig` argument,
so dump-first could not be learned from that argument alone. The historical
bit-17 hypothesis was then tested directly on C5 hardware: bit 17 plus ENABLE,
with no START pulse, ran for thousands of wraps from one producer start with
zero triggers and rearms. The continuous-RF driver reproduces that physically tested state
and deliberately omits the wrapper's trigger, DONE wait, timeout, disable,
restore and periodic rearm lifecycle.

`continuous_iq_acquire()` models only physically contiguous spans. Logical
producer/consumer positions continue through address 16383 -> 0 and a missed
or ambiguous wrap is exposed as a discontinuity. The active MAC-owned SRAM was
subsequently proven unreadable through ordinary CPU/AHB-GDMA, so this remains a
diagnostic abstraction rather than the current production source.

The simultaneous GPIO/ring diagnostic subsequently proved the replacement
XIAO source exactly: `MODEM_DIAG[6:9] = Q[6:9]` and
`MODEM_DIAG[16:19] = I[6:9]`. The VTX-ON capture matched 94.75% of individual
bits against the Q10/I10 ring under asynchronous CPU polling. Production will
route those eight signals internally to PARLIO RX; only the matching modem
sample clock remains to be identified and proven.

## DSP and output

For every adjacent pair, including across normal DMA/ring boundaries:

```text
d[n] = arg(x[n] * conj(x[n-1]))
```

The retired direct-SRAM experiment contains a compact signed-I/Q BitScrambler
approximation. It processes all adjacent samples, accumulates four real FM
results, then emits their boxcar average, but it is not a live source because
the input SRAM view is stale. The host validator retains the exact
floating-point conjugate-product reference and reports adjacent, post-filter
and +/-pi-seam errors for reuse by the MODEM_DIAG implementation.

RF and AV rates remain separate state. The RF dump writer measures about
79.97 MS/s; the proven synthetic AV transport runs at 20 MHz. The replacement
live path must measure the MODEM_DIAG/PARLIO-RX cadence and bridge it to the
actual PARLIO-TX rate only after adjacent FM, using a real-domain filter and
resampler. It must not infer RF cadence from an old argument name or decimate
complex IQ before the discriminator.

PARLIO hardware loop mode itself is available and the synthetic PAL diagnostic
proves safe loop-buffer switching. Directly mounting the MAC-owned IQ SRAM is
disabled because its live AHB view is stale. The candidate replacement is the
RF-dependent MODEM_DIAG bus feeding PARLIO RX, followed by adjacent FM and a
separate continuous PARLIO TX stream.

## Realtime and USB rules

- No `vTaskDelay`, allocation, logging or USB operation occurs in RF/DSP pacing.
- VTX OFF remains valid continuous noise IQ; signal presence never gates RF.
- Previous-IQ state resets only on a reported real discontinuity or retune.
- USB remains enabled and scheduled on the HP CPU; a low-priority task samples
  telemetry once per second.
- Normal realtime operation must not globally mask interrupts. The bounded
  SRAM/ring comparison diagnostic masks them for under 5 ms because the C5
  cannot safely enter a flash-backed ISR while the modem owns the dump bank;
  ownership and MSTATUS are restored before USB or flash is touched.

## Proof boundary

Software construction proves neither hidden RF sample continuity nor exact
PARLIO boundary timing. The required physical proofs are listed in
`docs/hardware-test.md`.
