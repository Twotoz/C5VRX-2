# Proven donor policy

C5VRX-2 does not inherit the current C5VRX product state machine. Donor code is accepted only when it directly supports one of these primitives:

- ESP32-C5 5 GHz RF init / A1=5865 MHz tune;
- RF dump SRAM reservation / ownership;
- mode-0 packed Q10/I10 producer configuration;
- physically proven direct 16K restart sequence;
- PARLIO 6-bit output on XIAO D4..D9;
- bounded counters/diagnostics outside the hot path.

Proof24 (`d17b2c56f1b6bb2973af5f96d60a6fa0e7b58837`) is the golden donor for RF/IQ semantics and host-video proof. Issue #22/its implementation donors own restart evidence. PR #26 is not a realtime architecture donor; only isolated low-level primitives may be reused from it.
