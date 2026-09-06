# C5VRX-2

ESP32-C5 experiment for receiving 5.8 GHz analog FPV directly as continuous
complex IQ and forwarding the recovered composite waveform to a six-bit
resistor DAC.

Target pipeline:

```text
A1 / 5865 MHz RF
  -> autonomous RF frontend / live MODEM_DIAG IQ tap
  -> source-synchronous PARLIO RX
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
  the bit-17 dump-first state proven by a one-start hardware soak. It grants
  the fixed `0x40830000` SRAM bank once and sets ENABLE once; it never emits
  START, waits for DONE, rearms, times out or tears RF down while running.
- The reader API models contiguous ring spans and preserves logical positions,
  but the active MAC-owned SRAM exposes only a stale view to CPU and AHB-GDMA.
  It is not a usable simultaneous live source.
- A simultaneous MODEM_DIAG/ring capture physically proved
  `DIAG[6:9] = Q[6:9]` and `DIAG[16:19] = I[6:9]`. With the VTX on, all eight
  individual bits matched the post-stop Q10/I10 ring with 94.75% aggregate
  bit accuracy despite asynchronous CPU sampling. This is the live 4+4-bit
  IQ source for the XIAO; its source-synchronous PARLIO RX clock is next.
- The old direct-SRAM LIVE route is deliberately blocked. It falls back to the
  proven synthetic PAL bars instead of outputting stale IQ or risking a GDMA
  underrun. It will be replaced only after the modem bus mapping is proven.
- USB Serial/JTAG remains scheduled. It is telemetry only and never controls or
  paces RF, DSP or PARLIO.
- Release builds use ESP-IDF 6.0.1 and 40 MHz DIO flash to avoid the observed
  ESP32-C5 rev1 startup/MSPI lockup with the tested 6.0.2 build.

The autonomous circular writer and MODEM_DIAG Q4/I4 mapping are proven. The
code does **not** yet claim sample-gapless RF, source-clocked PARLIO RX, or
decoded live CVBS.

See [the realtime contract](docs/realtime-iq-plan.md) and
[hardware tests](docs/hardware-test.md).
