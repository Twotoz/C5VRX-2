/* Keep static application state below the C5 RF dump window. */
ASSERT(_bss_end <= 0x4082ffc0,
       "C5VRX-2: static DRAM overlaps RF dump window 0x4082ffc0..0x4084003f")
