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

## Initial physical result (2387411)

Both captures passed payload/input hashes, with TX/RX/setup ESP_OK. RX elapsed
160 us at requested TX40 and 133 us at TX80. IRQ before=0, after=2.
Neither captured six-bit sequence matched any exact 16-sample signature of
the nominal reference (stride 1 and 2 respectively). TX40 contained only 14
unique codes and a visibly repeating 16-sample pattern; TX80 only 8 codes
and a visibly repeating 8-sample pattern. This is a FAILED comparison,
not proof of its cause. Do not infer RF image quality from this test.

Next image adds same-pad direct-byte and Phase5 controls, each TX40/TX80.
Header word 14 now identifies mode: 0 linear80, 1 direct bytes, 2 Phase5.
Six records use offsets 0x12000+trial*0x2000 within diagcap, trials 0..5;
absolute flash addresses 0x124000, 0x126000, 0x128000, 0x12a000, 0x12c000,
0x12e000. Each record is 4160 bytes. Trace stages 0x860..865. Direct mode
does not allocate or enable BitScrambler. Reference analysis uses matching
source programs for each mode and preserves six-bit masking only.

## Control-run interruption (ab5ba15)

User observed a continuously lit LED. Recovered trace reaches 0x860/861
but has no completed direct-byte control (0x862) or later pad trial.
The two new linear80 records validate their hashes and again fail reference
alignment (12/8 distinct six-bit codes, RX elapsed 130/127 us). This is not
a completed successful suite. Exact stopping operation was not instrumented.

Next image runs pad trials in order 2,3,4,5,0,1 before any loopback or EOF
sweep, so direct bytes run before BitScrambler has ever been allocated that
boot. This tests a peripheral-state carryover hypothesis, not a proven fix.
Additional trace markers 0x870+trial mean entry and 0x880+trial mean setup
finished immediately before calling TX transmit. Both occur before TX starts.
The expected source pattern, capture format, pin order and comparison stay
unchanged. Only records with completion markers from this boot are current;
unreached flash slots can still contain earlier captures.
