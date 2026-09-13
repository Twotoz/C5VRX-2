# Six-pad RF-off repeating replay

After the existing oracle/sweep, TX repeats the deterministic 16384-byte IQ
pattern without DSP reset at the DMA wrap. After 3 ms, RX captures 4096
samples from the existing six DAC pads {23,24,11,12,8,9}. No extra clock,
VALID or signal wiring. No RF. RX input enable preserves TX pad outputs.

Two trials request TX40/TX80, both with internally clocked RX40. The latter
is explicitly undersampled: it cannot validate every DAC update. Neither
trial measures resistor-network analog settling or simultaneous RF reception.
Requested clocks alone are not measured rates; disagreement can be capture
timing as well as TX corruption. A single capture need not cross a DMA wrap.

Each L8DP record has 16 little-endian words then 4096 captured bytes:
magic 0x5044384c, version=1, header=64, samples=4096, requested TX/RX Hz,
TX error, RX error, overall error, IRQ before, RX elapsed us, IRQ after,
capture FNV, input FNV, two reserved words. Flash writes happen after cleanup.
Existing L80O data remains intact. Records occupy reserved diagcap offsets
0x12000 and 0x14000; trace stages 0x860/861 report persistence status.

Read 4160 bytes at absolute flash addresses 0x124000 and 0x126000, then:

```
python tools/analyze_linear80_pads.py <capture.bin>
```

The analyzer checks payload/input hashes and compares all captured six-bit
codes to the source-driven steady cyclic reference. It searches cyclic phase,
not arbitrary stretches of skipped samples. Nonmatching signatures are not
discarded or marked passing. LED still reflects the original stock-DMA-EOF
oracle gate, not these separate pad captures. Allow 30 seconds, VTX off.
