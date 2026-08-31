# C5VRX-2 minimal flasher

This intentionally does only three things:

1. connect to the ESP32-C5 ROM bootloader;
2. flash one merged image at `0x0`;
3. open a raw 115200 serial terminal.

## Firmware

The `C5VRX-2 realtime build` GitHub Action produces artifact `c5vrx2-full` containing:

```text
c5vrx2-full.bin
c5vrx2-full.bin.sha256
FIRMWARE-COMMIT.txt
TESTING.md
flasher.html
```

The image is generated with ESP-IDF 6.0.2 `idf.py merge-bin`, so the web flasher does not need separate bootloader, partition-table or application offsets.
The checksum and commit file make the physical result traceable to one PR build.

## Run the flasher

Web Serial needs a secure context. From the repository root, or from the
unzipped artifact directory:

```bash
python -m http.server 8000
```

Then open the matching page:

```text
http://localhost:8000/tools/flasher/
http://localhost:8000/flasher.html
```

Use Chrome or Edge.

## Test flow

```text
select c5vrx2-full.bin
-> Connect bootloader
-> Flash merged .bin
-> Open 115200 terminal
```

If the XIAO is not detected automatically, hold **BOOT**, press and release
**RESET**, then release **BOOT** before connecting again.

The page uses Espressif `esptool-js` 0.6.1 and keeps the flash mode/frequency/size encoded by the merged firmware.
