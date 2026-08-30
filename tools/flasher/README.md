# C5VRX-2 minimal flasher

This intentionally does only three things:

1. connect to the ESP32-C5 ROM bootloader;
2. flash one merged image at `0x0`;
3. open a raw 115200 serial terminal.

## Firmware

The `C5VRX-2 realtime build` GitHub Action produces artifact `c5vrx2-full` containing:

```text
c5vrx2-full.bin
```

The image is generated with ESP-IDF 6.0.2 `idf.py merge-bin`, so the web flasher does not need separate bootloader, partition-table or application offsets.

## Run the flasher

Web Serial needs a secure context. From the repository root:

```bash
python -m http.server 8000
```

Then open:

```text
http://localhost:8000/tools/flasher/
```

Use Chrome or Edge.

## Test flow

```text
select c5vrx2-full.bin
-> Connect bootloader
-> Flash merged .bin
-> Open 115200 terminal
```

The page uses Espressif `esptool-js` 0.6.1 and keeps the flash mode/frequency/size encoded by the merged firmware.
