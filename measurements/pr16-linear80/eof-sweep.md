# RF-off EOF investigation

Physical tail=9 retest of e6cc418: still 32,764 received bytes, all equal
to the reference, four missing. TX40 timed out after 1,000,927 us and TX80
after 1,000,047 us. Both midstream IRQ snapshots zero. Capture SHA256:
`d171daec974dcf18b0f9e767cecbfe6980b99a762ca89e28cdfcc00e5783d40e`.
Changing 8 to 9 did NOT fix the issue.

The next diagnostic preserves the regular L80O capture first, then tests
upstream tails 8, 9, 10, 12, 16, 24, 32 on a RAM copy of the IDF v1 program.
Instructions/LUT remain unchanged. It also tests finite TX completion with
the existing Phase5 program at 40 MHz as a control. No live configuration
is selected automatically and no success gate is relaxed.

Read the 4096-byte trace partition at 0x111000 after running the test.
Records are 64 bytes: magic 0x52543243, version/size at 4, sequence at 8,
stage at 12, signed error at 16, and three detail words at 52/56/60.
For trial index i=0..6:

- 0x810+i: loopback error; details tail, written bytes, differing received bytes.
- 0x820+i: TX40 error; details tail, first transfer elapsed us, midstream IRQ.
- 0x830+i: TX80 error; same details.
- 0x840: Phase5 TX40 control error, details 40, elapsed us, IRQ.
- 0x84f: sweep reached its end (NOT a passing hardware gate).
- 0x800: unsupported binary header; sweep was not run.

Missing bytes remain failures even if differing received bytes is zero.
No timing rate can be inferred from a failed transaction. Each trace write
occurs after peripheral cleanup, outside measured transmission. The LED
continues to describe the original strict L80O test result, not the sweep.
Allow 30 seconds after boot; VTX stays off throughout.
