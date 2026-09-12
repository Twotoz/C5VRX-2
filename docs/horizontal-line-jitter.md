# Horizontal line-jitter candidate

The post-PR7 live picture is sharp, responsive and substantially cleaner, but
individual NTSC lines can still begin roughly 5--25 output samples left or
right of their neighbours. The grey cast is tracked separately; this change
does not alter WBFM mapping, gain, pedestal, polarity or DAC levels.

## Concrete transport mismatch

The live code described its raw ring as four 4096-byte blocks. ESP-IDF 6.0.1
does not mount those as four C5 PARLIO-RX descriptors: its maximum aligned RX
descriptor payload is 4092 bytes. A 16384-byte ring is consequently divided
into unequal RX nodes, while PARLIO-TX builds a different descriptor layout.
The old 4096-byte boundary analysis therefore did not inspect the physical
GDMA boundaries.

The candidate uses:

```text
4 x 4092 bytes = 16368-byte raw cyclic ring
```

RX therefore receives four equal physical nodes. TX is started after a
rounded-up 103 microsecond lead, so it cannot begin inside the first RX node.
The 40 MHz input-byte rate and 20 MHz output rate remain derived from the same
PLL and the BitScrambler remains one continuous transaction.

## Proof boundary

This is a hardware A/B candidate, not yet proof that descriptor transitions
were dropping samples. Acceptance requires visibly reduced line-to-line
horizontal displacement without worse static, colour, lock or latency. If
jitter remains unchanged, the next suspect is sync-edge noise in the recovered
CVBS waveform rather than cyclic memory geometry; generic gap filling must not
be added without a measured missing/duplicate sample count.
