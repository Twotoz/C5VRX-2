# Issue 12: source-related MODEM_DIAG acquisition A/B

This branch keeps the PR10 demodulator, 16 KiB production ring, and exact
40-to-20 MS/s BitScrambler schedule unchanged. Only the PARLIO RX clock source
and selectable physical sample edge differ.

## What source inspection proves

ESP-IDF v6.0.1's ESP32-C5 register header defines `FPGA_DEBUG_CLK40` at bit 5
and `FPGA_DEBUG_CLKSWITCH` at bit 3 of `MODEM_SYSCON_TEST_CONF`. The PARLIO RX
driver supports `PARLIO_CLK_SRC_EXTERNAL`, GPIO clock input, a declared external
frequency, free-running operation, and either physical sample edge.

The public C5 sources do **not** document which MODEM_DIAG lane carries
DEBUG_CLK40. DIAG21 is therefore an explicit test hypothesis, not a fact. A
scope/logic-analyzer measurement of GPIO2 must show a continuous 40 MHz clock
before the MODEM candidate is allowed into LIVE. The documented PLL_F40M clock
output is included as a safer phase-related fallback and control.

## Reproducible matrix

Build and test all three clocks on both edges:

| Clock | Config overlay | Purpose |
|---|---|---|
| internal 40 MHz | none | current baseline |
| PLL_F40M -> GPIO2 | `sdkconfig.source-sync-pll.defaults` | documented shared-clock candidate |
| MODEM DEBUG_CLK40 -> GPIO2 | `sdkconfig.source-sync-modem.defaults` | preferred source-sync candidate, lane unproven |

Append `sdkconfig.rx-neg.defaults` for the opposite physical edge. Use
`sdkconfig.modem-parlio.defaults` for byte/reference captures and append
`sdkconfig.trajectory.defaults` to the normal LIVE defaults for the PR10 path.

For every clock/edge combination, collect at least ten cold boots with the same
coherent RF stimulus, then several minutes of loaded CVBS. Keep VTX, channel,
attenuation, scene, supply, antenna/cable and capture equipment unchanged.

Run raw comparisons with:

```sh
python tools/compare_rx_clocks.py internal-pos.bin pll-pos.bin modem-pos.bin
```

The source-synchronous candidate passes acquisition only if repeated captures
show no unexplained parity/ratio slips, materially fewer exact-byte and mixed-
lane errors against the native ring, no new 4096/16384-boundary events, one RF
producer start, and zero rearms. Promote the better physical edge only from
these measurements.

For the live A/B, record hard discriminator impulses, black/white specks,
colour bombs, H-sync leading-edge jitter, shifted lines/layers and lock time.
An external continuous CVBS capture is required: a 16 KiB IQ snapshot alone is
too short to prove frame-scale layer/lock behavior. Keep PR10, gain, pedestal,
polarity, DAC network and output load identical.

## Current conclusion

Host/source tests can prove configuration separation and unchanged transport/
demodulator structure. They cannot prove that DIAG21 is DEBUG_CLK40 or that an
image improves without attached C5 hardware. Until the physical matrix is
attached, internal sampling remains the default and this PR remains a draft.
