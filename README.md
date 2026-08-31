# C5VRX-2

Clean ESP32-C5 realtime analog-FPV receiver experiment.

The branch `realtime-iq-parlio` deliberately starts from the C5VRX proof24 RF/IQ facts and strips the receiver back to the shortest hardware datapath:

```text
A1 / 5865 MHz
      ↓
mode-0 packed Q10/I10 writer
      ↓
fixed 16K RF dump SRAM
      ↓
DONE + PTR=16383
      ↓
PAU / REGDMA immediate rearm
      ↓
looping RF-SRAM consumer
      ↓
BitScrambler: phase -> dphi, decimate 4:1
      ↓
20 MS/s PARLIO
      ↓
XIAO D4..D9 six-resistor DAC
```

## Non-negotiable realtime rules

- VTX presence is **never** required to produce IQ.
- VTX OFF is valid: off-air noise/spurs still flow through the datapath.
- `NO_RF`, PAL/NTSC detection, sync/burst analysis and USB are not producer gates.
- The full vendor `adctrig()` lifecycle is not repeated per 16K generation.
- At the 16K boundary, REGDMA restart is launched before counters, logging, DSP or consumer bookkeeping.
- No full 64 KiB per-block memcpy exists in the realtime path.
- RF SRAM feeds the looping BitScrambler/PARLIO transaction directly.
- HP is parked while MAC owns the fixed dump SRAM. USB is useful before a run or after a fault, never required during RF acquisition.

## First hardware gate

Flash `realtime-iq-parlio`, connect the existing D4..D9 resistor DAC to AV, and boot with the VTX **off**. The branch starts automatically; there is no capture command.

Expected behavior:

1. VTX OFF: the physical AV output is still active with RF-dependent/off-air waveform.
2. Turn VTX ON at A1/5865 without resetting anything: output must change immediately.
3. Turn VTX OFF again: producer must continue instead of entering a `NO_RF` state.
4. If the LP producer hits a real writer/rearm fault it restores SRAM ownership and the HP core prints the stored counters/fault registers.

Recognizable stable video is **not** the first acceptance criterion. First prove that RF acquisition and PARLIO consumption remain alive continuously across VTX OFF -> ON -> OFF.

See `docs/realtime-iq-plan.md` for the implementation contract and `docs/hardware-test.md` for the hardware gate.
