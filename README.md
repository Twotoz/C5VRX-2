# C5VRX-2

ESP32-C5 experiment for receiving 5.8 GHz analog FPV directly as continuous
complex IQ and forwarding the recovered composite waveform to a six-bit
resistor DAC.

Target pipeline:

```text
A1 / 5865 MHz RF
  -> autonomous RF frontend / live MODEM_DIAG IQ tap
  -> coherent 2:1 capture by 40-MS/s PARLIO RX
  -> adjacent-sample WBFM discriminator
  -> real-domain filtering / rate conversion
  -> continuous PARLIO/GDMA
  -> D4..D9 resistor DAC
  -> 75-ohm CVBS input
```

The normal live path does not decode or regenerate PAL. Sync, blanking, luma,
burst and chroma are already present in the VTX's FM modulation and remain one
real waveform after FM demodulation.

## Current status

- `continuous_iq_start()` uses the vendor-derived C5 TX_START selector plus
  the bit-17 dump-first state proven by a one-start hardware soak. It sets
  ENABLE once; it never emits START, waits for DONE, rearms, times out or tears
  RF down while running. LIVE keeps HP SRAM CPU-owned because its samples come
  from MODEM_DIAG; bounded ring diagnostics grant the fixed dump bank only for
  their capture window.
- The reader API models contiguous ring spans and preserves logical positions,
  but the active MAC-owned SRAM exposes only a stale view to CPU and AHB-GDMA.
  It is not a usable simultaneous live source.
- A simultaneous MODEM_DIAG/ring capture physically proved
  `DIAG[6:9] = Q[6:9]` and `DIAG[16:19] = I[6:9]`. With the VTX on, all eight
  individual bits matched the post-stop Q10/I10 ring with 94.75% aggregate
  bit accuracy despite asynchronous CPU sampling. This is the live 4+4-bit
  IQ source for the XIAO.
- Bounded PARLIO tests at the C5's 40-MHz receive limit captured a bit-perfect
  sequence of every second native MODEM sample. Requesting F80 or F160 did not
  raise the observed receive cadence beyond about 40 MS/s; the native MODEM
  and dump cadence remained about 80 MS/s.
- LIVE now routes Q4/I4 through internal GPIO-matrix loopback into an infinite
  PARLIO-RX/GDMA ring. A two-bundle TX BitScrambler consumes two raw bytes per
  20-MS/s DAC sample and evaluates phase change between consecutive retained
  Q3/I2 states. PARLIO-TX continuously loops the same elastic ring into the
  existing D4..D9 DAC. No PAL decoder, framebuffer or PAL regenerator is in
  this normal path.
- On 2026-09-09 the merged `69f52c3` LIVE image produced a stably locked,
  clearly recognizable NTSC camera picture through the six-bit DAC on physical
  XIAO ESP32-C5 hardware. Visible static remains, so this proves functional
  end-to-end RF-to-CVBS recovery but not production picture quality.

The next image-quality path keeps that transport unchanged and replaces the
asymmetric Q3/I2 phase approximation with a full-input, uniform five-bit
polar phase LUT. Its design, reproducible builds and physical proof gates are
documented in [docs/image-quality.md](docs/image-quality.md).
- USB Serial/JTAG remains scheduled. It is telemetry only and never controls or
  paces RF, DSP or PARLIO.
- Release builds use ESP-IDF 6.0.1 and 40 MHz DIO flash to avoid the observed
  ESP32-C5 rev1 startup/MSPI lockup with the tested 6.0.2 build.

The autonomous circular writer, Q4/I4 mapping, bounded 80-to-40 sample
relationship and recognizable locked live NTSC output are proven. The code
does **not** yet claim long-duration slip-free 80-to-40 capture, sample-gapless
RF time, glitch-free RX/TX DMA boundaries, or production-quality recovered
CVBS.

See [the realtime contract](docs/realtime-iq-plan.md) and
[hardware tests](docs/hardware-test.md).
