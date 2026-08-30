# C5VRX-2

Clean ESP32-C5 realtime analog-FPV receiver branch.

This repository intentionally starts from the proven C5VRX video-proof-24 hardware assumptions, but removes product/video gating from the realtime IQ producer path.

Immediate objective:

```text
boot -> tune A1/5865 -> IQ producer always running -> minimal FM discriminator -> PARLIO -> D4..D9 DAC
```

Rules for the hot path:

- VTX presence is never required to produce IQ.
- `NO_RF`, PAL/NTSC detection, sync/burst analysis and USB are not producer gates.
- Do not use the full vendor `adctrig()` lifecycle per block once the producer is running.
- Rearm must happen immediately at the 16K boundary, independently of DSP/PARLIO.
- Avoid full 64 KiB IQ copies; consume the RF dump window directly or through the smallest proven handoff.
- First output goal is raw RF-dependent analog waveform, not polished video.

See `docs/realtime-iq-plan.md` for the implementation contract.
