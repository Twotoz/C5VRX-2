# C5VRX-2

ESP32-C5 experiment for receiving 5.8 GHz analog FPV directly as continuous
complex IQ and forwarding the recovered composite waveform to a six-bit
resistor DAC.

```text
A1 / 5865 MHz RF
  -> autonomous pre-trigger Q10/I10 SRAM ring
  -> adjacent-sample WBFM discriminator
  -> four-tap real boxcar / rate conversion
  -> continuous PARLIO/GDMA
  -> D4..D9 resistor DAC
  -> 75-ohm CVBS input
```

The normal live path does not decode or regenerate PAL. Sync, blanking, luma,
burst and chroma are already present in the VTX's FM modulation and remain one
real waveform after FM demodulation.

## Current implementation

- `continuous_iq_start()` reproduces the C5 vendor-oracle TX_START/dump-first
  register state, grants the fixed `0x40830000` SRAM bank once and sets ENABLE
  once. It never generates START, waits for DONE, rearms, times out or tears RF
  down while running.
- The reader API exposes contiguous ring spans with absolute logical positions,
  guard distance, explicit overrun/discontinuity reporting and no allocation.
- The production BitScrambler keeps previous IQ across every physical wrap,
  evaluates all four adjacent phase steps, then performs a real four-sample
  boxcar before emitting one DAC sample.
- PARLIO requests `measured_rf_rate / 4` and loops one cyclic GDMA descriptor
  chain over the RF ring. There is no output stop/restart at SRAM wrap.
- USB Serial/JTAG remains scheduled. It is telemetry only and never controls or
  paces RF, DSP or PARLIO.
- Release builds use ESP-IDF 6.0.1 and 40 MHz DIO flash to avoid the observed
  ESP32-C5 rev1 startup/MSPI lockup with the tested 6.0.2 build.

The code is ready to test but does **not** claim physically gapless RF or AV
until the coherent-tone SRAM-wrap and PARLIO-boundary measurements pass.

See [the realtime contract](docs/realtime-iq-plan.md) and
[hardware tests](docs/hardware-test.md).
