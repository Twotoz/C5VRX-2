# WBFM image-quality path

The physically proven baseline from PR2 is intentionally retained as a
fallback. It reduces the proven Q4/I4 byte to asymmetric Q3/I2 Cartesian
states, performs a direct 32 x 32 phase-difference lookup, and sustains the
required 20 MS/s output. On 2026-09-09 that path produced stable, recognizable
live NTSC video, but with visible static.

## Uniform phase5 core

The quality path uses the complete Q4/I4 input byte and a dual-purpose
1024 x 16-bit LUT:

```text
raw Q4/I4 byte
 -> atan2-derived uniform five-bit phase
 -> circular delta from the previous retained phase
 -> calibrated gain, pedestal and six-bit clamp
 -> PARLIO TX at 20 MS/s
```

The LUT stores raw-byte phase in bits 12..8 and the 32 x 32 discriminator
result in bits 5..0. This allows both lookups to coexist without another
buffer or another DMA stage. Persistent BitScrambler state carries the prior
phase across ordinary DMA and ring boundaries.

The host model exhaustively checks all 1024 LUT addresses and the circular
phase wrap. Across all useful Q4 amplitude states, uniform phase5 reduces the
absolute phase quantization error from 9.52 degrees RMS / 25.63 degrees max
for Q3/I2 to 3.27 degrees RMS / 5.62 degrees max.

## Reproducible builds

Live quality firmware:

```powershell
docker run --rm -v "${PWD}:/project" -w /project espressif/idf:v6.0.1 `
  idf.py -B build-quality-live `
  -D SDKCONFIG=/project/build-configs/sdkconfig.quality `
  -D SDKCONFIG_DEFAULTS=/project/sdkconfig.quality.defaults build
```

Bounded transport oracle (no RF):

```powershell
docker run --rm -v "${PWD}:/project" -w /project espressif/idf:v6.0.1 `
  idf.py -B build-quality-test `
  -D SDKCONFIG=/project/build-configs/sdkconfig.quality-test `
  -D SDKCONFIG_DEFAULTS=/project/sdkconfig.quality-test.defaults build
```

The oracle sends deterministic Q4/I4 input through the exact three-bundle TX
BitScrambler at the production 40-to-20 MS/s ratio and captures the low four
DAC bits through PARLIO RX. Its persisted result must match the C reference
byte-for-byte before the quality core is called physically proven.

## Proof status

Software proven:

- full-Q4/I4 to uniform phase5 mapping;
- all 1024 dual-purpose LUT entries;
- circular delta including the 31-to-0 wrap;
- both LIVE and bounded diagnostic ESP-IDF 6.0.1 builds.

Still requiring hardware proof:

- three-bundle TX BitScrambler sustains 20 MS/s without underrun;
- bounded output matches the C reference byte-for-byte;
- live image has less static than the Q3/I2 baseline;
- no hidden discontinuity at long-running RX/TX ring boundaries.

Set `CONFIG_C5VRX2_WBFM_PHASE5_QUALITY=n` to restore the known-working Q3/I2
baseline while preserving the rest of the direct RX-ring-to-TX architecture.
