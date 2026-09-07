#!/usr/bin/env python3
"""Correlate a MODEM_CAPTURE GPIO/PARLIO trace with its Q10/I10 ring.

Version 1 stores raw GPIO_IN words sampled by the CPU. Later versions store
packed Q4/I4 bytes acquired by PARLIO RX. Those tests requested several clock
sources, but physical correlation showed the C5 receive path accepting about
40 MS/s in each successful case. Both forms are followed by the 16384-word
post-stop RF dump ring. The tool searches timing-ratio uncertainty and every
circular ring offset rather than assuming the two engines started together.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import numpy as np

MAGIC = 0x5043444D
HEADER_WORDS = 21
HEADER_BYTES = 128
GPIO_PINS = (1, 0, 25, 7, 10, 5, 3, 4)
EXPECTED_BITS = (6, 7, 8, 9, 16, 17, 18, 19)
PARLIO_RATES_HZ = {version: 40_000_000.0 for version in (4, 5, 6, 7)}


def load_capture(path: Path) -> tuple[dict[str, int], np.ndarray, np.ndarray]:
    blob = path.read_bytes()
    names = (
        "magic", "version", "header_bytes", "raw_words", "ring_words",
        "diag_fix", "diag_exchange", "gpio_mask", "sample_us",
        "rf_rate_hz", "writer_pointer", "dump_control", "dump_ptr_mode",
        "producer_starts", "physical_wraps", "trigger_count", "raw_hash",
        "ring_hash", "capture_end_pointer", "capture_end_ptr_mode", "stop_us",
    )
    header = dict(zip(names, struct.unpack_from("<21I", blob)))
    if header["magic"] != MAGIC or header["version"] not in (1, 4, 5, 6, 7):
        raise ValueError(
            "capture is not a complete MODEM_CAPTURE (v1/v4/v5/v6/v7)")
    if header["header_bytes"] != HEADER_BYTES:
        raise ValueError("unsupported capture header size")
    if header["version"] != 1:
        receive_error = struct.unpack_from("<i", blob, 120)[0]
        if receive_error:
            raise ValueError(
                f"PARLIO capture did not complete (esp_err=0x{receive_error:x})")
    raw_offset = header["header_bytes"]
    raw_item_bytes = 4 if header["version"] == 1 else 1
    ring_offset = raw_offset + header["raw_words"] * raw_item_bytes
    needed = ring_offset + header["ring_words"] * 4
    if len(blob) < needed:
        raise ValueError("capture file is truncated")
    raw_dtype = "<u4" if header["version"] == 1 else "u1"
    raw = np.frombuffer(blob, dtype=raw_dtype, count=header["raw_words"],
                        offset=raw_offset).copy()
    ring = np.frombuffer(blob, dtype="<u4", count=header["ring_words"],
                         offset=ring_offset).copy()
    return header, raw, ring


def pack_gpio(raw: np.ndarray) -> np.ndarray:
    packed = np.zeros(raw.size, dtype=np.uint8)
    for lane, pin in enumerate(GPIO_PINS):
        packed |= (((raw >> pin) & 1).astype(np.uint8) << lane)
    return packed


def correlate(header: dict[str, int], captured: np.ndarray,
              ring: np.ndarray, samples: int, width: float,
              steps: int) -> tuple[float, float, int, np.ndarray, np.ndarray]:
    if header["version"] in PARLIO_RATES_HZ:
        gpio_rate = PARLIO_RATES_HZ[header["version"]]
    else:
        gpio_rate = captured.size / header["sample_us"] * 1_000_000.0
    nominal = header["rf_rate_hz"] / gpio_rate
    samples = min(samples, captured.size,
                  int((ring.size - 1) / max(nominal, 1.0)))
    if samples < 64:
        raise ValueError("not enough overlapping samples")

    expected = (((ring >> 6) & 0x0F) |
                (((ring >> 16) & 0x0F) << 4)).astype(np.uint8)
    captured_bits = [1 - 2 * ((captured >> bit) & 1).astype(np.int8)
                     for bit in range(8)]
    ring_bits = [1 - 2 * ((ring >> bit) & 1).astype(np.int8)
                 for bit in EXPECTED_BITS]
    ring_fft = [np.fft.fft(bits.astype(float)) for bits in ring_bits]

    best = (-2.0, nominal, 0)
    for slope in np.linspace(nominal - width, nominal + width, steps):
        positions = (-np.rint(slope * np.arange(samples)).astype(int)) % ring.size
        total = np.zeros(ring.size)
        for lane in range(8):
            sparse = np.zeros(ring.size)
            sparse[positions] = captured_bits[lane][-1:-samples - 1:-1]
            total += np.fft.ifft(np.conj(np.fft.fft(sparse)) *
                                 ring_fft[lane]).real
        offset = int(np.argmax(total))
        score = float(total[offset]) / (8 * samples)
        if score > best[0]:
            best = (score, float(slope), offset)

    score, slope, offset = best
    indices = (offset - np.rint(slope * np.arange(samples)).astype(int)) % ring.size
    observed = captured[-1:-samples - 1:-1]
    reference = expected[indices]
    return score, slope, offset, observed, reference


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--samples", type=int, default=0)
    parser.add_argument("--search-width", type=float)
    parser.add_argument("--steps", type=int, default=1201)
    args = parser.parse_args()

    header, raw, ring = load_capture(args.capture)
    captured = pack_gpio(raw) if header["version"] == 1 else raw
    samples = args.samples or (900 if header["version"] == 1 else 1800)
    search_width = args.search_width
    if search_width is None:
        search_width = 0.15 if header["version"] == 1 else 0.003
    score, slope, offset, observed, reference = correlate(
        header, captured, ring, samples, search_width, args.steps)
    xor = observed ^ reference
    bit_accuracy = (1.0 -
                    sum(int(value).bit_count() for value in xor) /
                    (8.0 * observed.size))
    exact = float(np.mean(observed == reference))
    pipeline_offset = ((header["capture_end_pointer"] - offset) % ring.size)

    print(f"RF rate:             {header['rf_rate_hz']} samples/s")
    print(f"capture transport:   {'CPU GPIO' if header['version'] == 1 else 'PARLIO RX'}")
    print(f"producer/wraps/trig: {header['producer_starts']}/"
          f"{header['physical_wraps']}/{header['trigger_count']}")
    print("mapping:             DIAG[6:9]=Q[6:9], DIAG[16:19]=I[6:9]")
    print(f"RF/GPIO ratio:       {slope:.6f}")
    print(f"pipeline offset:     {pipeline_offset} RF samples")
    print(f"correlation:         {score:.6f}")
    print(f"bit accuracy:        {bit_accuracy:.4%}")
    print(f"exact bytes:         {np.sum(observed == reference)}/"
          f"{observed.size} ({exact:.4%})")


if __name__ == "__main__":
    main()
