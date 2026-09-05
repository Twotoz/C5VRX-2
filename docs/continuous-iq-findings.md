# ESP32-C5 continuous IQ hardware findings

Hardware under test: ESP32-C5 revision v1.0, A1/5865 MHz receive path.
The observations below were made on 2026-09-05 with ESP-IDF v6.0.1.

## Proven autonomous circular writer

The C5 RF dump engine was armed once with:

- `DUMP_PTR_MODE[24:17] = 0x00060000` (TX_START selector)
- `DUMP_CTRL[17] = 1` (dump-first/pre-trigger)
- `DUMP_CTRL[31] = 1` (enable)
- no software START pulse
- MAC TX disabled, so TX_START cannot occur

The measured writer rate was approximately 79.97 MS/s. A producer-only
hardware soak then reported:

```text
observed physical wraps = 10000
observer-estimated wraps = 54240
producer starts = 1
rearms = 0
triggers = 0
DUMP_CTRL = 0x80024000
```

This proves that the 16,384-word/64-KiB region is a long-lived circular RF
writer rather than a mandatory sequence of finite 16K captures. Normal wraps
require no write to `DUMP_CTRL`.

## C5 vendor-wrapper finding

Disassembly of ESP-IDF v6.0.1 `librftest.a:adctrig` showed that C5 trigmode 5
selects `0x00060000`, but the historical `dump_trig` function argument is not
consumed by this implementation. Calls with `dump_trig=0` and `dump_trig=1`
therefore produce the same trigger-first state. Production code preserves the
vendor-derived TX_START selector and sets the historical dump-first bit
explicitly.

## SRAM ownership finding and current blocker

The vendor MAC dump allocation changes `0x60095004` to set bits `0x00010200`.
Both 64-KiB banks from `0x40830000` through `0x4084ffff` must be excluded from
the normal heap; reserving only the first bank caused a CPU lockup when the
ownership handoff overwrote allocations in the second bank.

With the full region reserved, the writer completes the soak without memory
corruption. However, direct HP-CPU reads of the first ring while the MAC owns
the banks returned zero-valued IQ windows. The guard check also appeared
unreadable during MAC ownership and became valid again after ownership was
returned. Feeding the same live ring directly to PARLIO/GDMA produced a FIFO
underrun followed by heap corruption.

Therefore the autonomous writer is proven, but the current SRAM ownership
configuration is not yet a usable simultaneous MAC-write/HP-read mapping.
`continuous_iq_acquire()` must not be considered production-ready until a
shared/arbitrated ownership configuration or another live-readable path is
physically demonstrated.

An AHB-GDMA visibility probe then copied the same 2-KiB physical RF-ring
window twice, 300 us apart, while the autonomous writer remained enabled. At
the measured 79.97 MS/s the writer traversed the 16K ring several times during
the probe. Both DMA operations completed successfully, but the snapshots were
byte-for-byte identical:

```text
VTX off: nonzero A=512, nonzero B=512, changed=0
VTX on:  nonzero A=256, nonzero B=256, changed=0
```

The different static occupancy with and without the VTX shows that the mapped
contents are not simply an untouched destination pattern. The absence of any
within-run changes nevertheless proves that ordinary AHB-GDMA sees the same
stale/non-live HP-domain view as direct CPU reads. Directly DMA-reading the
MAC-owned ring is therefore not the missing simultaneous-access mechanism.

A follow-up two-bank probe filled both candidate windows with distinct known
patterns before arming the same `SRAM_USAGE=2`, `MAC_DUMP_ALLOC=1` producer.
It copied both windows twice through AHB-GDMA while the writer ran, then
stopped the writer, restored CPU ownership and inspected the physical SRAM:

```text
RF rate                      = 79.959 MS/s
producer starts/rearms/trigs = 1 / 0 / 0
live GDMA changes, 0x40830000 = 0 / 256 bytes
live GDMA changes, 0x40840000 = 0 / 256 bytes
post-stop overwritten words, 0x40830000 = 16384 / 16384
post-stop overwritten words, 0x40840000 = 0 / 16384
guards intact                = yes
```

This physically locates the IQ dump at `0x40830000`; applying an additional
64-KiB address offset in the consumer would be wrong. `0x40840000` is not a
live alternate view. Both normal AHB views remain static while MAC ownership
is active, even though bank A contains the completed IQ ring immediately after
ownership is restored.

A second build retained documented HP-CPU ownership (`SRAM_USAGE=0`) while
leaving `MAC_DUMP_ALLOC=1`. The autonomous writer still advanced at about
79.96 MS/s, but neither CPU nor AHB-GDMA obtained a changing live view. Merely
leaving the ownership selector at zero is therefore not a shared-access mode.

Finally, the official ESP32-C5 HP_APM M1 exception registers were cleared and
sampled before and after four completed AHB-GDMA reads from both candidate
banks. The result was identical at every checkpoint:

```text
HP_APM_M1_STATUS          = 0x00000000
HP_APM_M1_EXCEPTION_INFO0 = 0x00000000
HP_APM_M1_EXCEPTION_INFO1 = 0x00000000
```

During that same run the RF writer measured 79.9705 MS/s and completed 28
physical wraps from one start with zero triggers. All DMA copies returned
success, their live snapshots did not change, and after stopping RF all 16,384
words of bank A were found overwritten while bank B remained untouched. Thus
HP_APM permission or bounds checking is not blocking the reads. The active
MAC dump bank exposes a stale/non-live normal AHB view by hardware design; an
APM permission change cannot make that view live.

## Claims deliberately not made yet

- No claim of sample-gapless RF time across `16383 -> 0` is made yet.
- The coherent-tone adjacent-phase test has not passed because HP could not
  observe the IQ words while the current MAC ownership was active.
- Live PARLIO output is not proven with the RF writer active.

The next bounded experiment is the C5 modem diagnostic path. Vendor
`coex_hw_debug_matrix_config()` writes `0x600a9408` and routes modem diagnostic
signals 106 through 109, while `bt_bb_ble_diag_all()` programs
`0x600a9404` and related selector fields. A diagnostic must first use only
vendor-observed selector states and determine whether a continuously changing,
RF-dependent IQ representation exists before attempting a production consumer.
Only after simultaneous live IQ access works should coherent-tone phase
continuity and the live WBFM-to-CVBS pipeline be tested.
