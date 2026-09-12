#!/usr/bin/env python3
"""Compare issue-12 MODEM->PARLIO acquisition captures.

Use complete diagcap partition images made by MODEM_PARLIO builds. Each input
is correlated independently with the simultaneously completed native Q10/I10
ring. The report measures acquisition agreement and raw hard-error proxies; it
does not infer success from a recognizable picture.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from analyze_modem_capture import (RX_CLOCK_BY_VERSION, correlate,
                                   load_capture)


def phase(raw: np.ndarray) -> np.ndarray:
    q = ((raw & 15).astype(np.int16) + 8) % 16 - 8
    i = ((raw >> 4).astype(np.int16) + 8) % 16 - 8
    return np.arctan2(q.astype(float), i.astype(float))


def wrap(value: np.ndarray) -> np.ndarray:
    return (value + np.pi) % (2 * np.pi) - np.pi


def metrics(path: Path, samples: int) -> dict[str, object]:
    header, captured, ring = load_capture(path)
    if header["version"] not in RX_CLOCK_BY_VERSION:
        raise ValueError(f"{path}: requires an issue-12 raw capture (v12-v14)")
    count = samples or min(captured.size, max(64, (ring.size - 2) // 2))
    score, slope, offset, observed, reference = correlate(
        header, captured, ring, count, 0.01, 2001)
    xor = observed ^ reference
    bit_errors = np.fromiter((int(x).bit_count() for x in xor), np.uint8)
    d = wrap(np.diff(phase(captured)))
    block = []
    for boundary in range(4096, captured.size, 4096):
        if boundary - 1 < d.size:
            block.append(float(abs(d[boundary - 1])))
    return {
        "file": path.name,
        "version": header["version"],
        "clock": RX_CLOCK_BY_VERSION[header["version"]],
        "compared": int(observed.size),
        "rf_to_rx_ratio": slope,
        "ratio_error_ppm_from_2": (slope / 2.0 - 1.0) * 1e6,
        "correlation": score,
        "exact_byte_fraction": float(np.mean(xor == 0)),
        "bit_error_fraction": float(bit_errors.sum() / (8 * bit_errors.size)),
        "mixed_byte_fraction": float(np.mean((bit_errors > 0) & (bit_errors < 8))),
        "multi_lane_error_fraction": float(np.mean(bit_errors >= 2)),
        "adjacent_duplicate_fraction": float(np.mean(captured[1:] == captured[:-1])),
        "hard_phase_step_fraction": float(np.mean(np.abs(d) >= 0.9 * np.pi)),
        "phase_step_p999_rad": float(np.percentile(np.abs(d), 99.9)),
        "block_boundary_max_rad": max(block, default=float("nan")),
        "pipeline_offset_native_samples": int(
            (header["capture_end_pointer"] - offset) % ring.size),
        "producer_starts": header["producer_starts"],
        "physical_wraps": header["physical_wraps"],
        "trigger_count": header["trigger_count"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--samples", type=int, default=0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    rows = [metrics(path, args.samples) for path in args.captures]
    if args.json:
        print(json.dumps(rows, indent=2))
        return
    keys = ("clock", "exact_byte_fraction", "bit_error_fraction",
            "mixed_byte_fraction", "ratio_error_ppm_from_2",
            "hard_phase_step_fraction", "block_boundary_max_rad")
    print("\t".join(keys))
    for row in rows:
        print("\t".join(str(row[key]) for key in keys))


if __name__ == "__main__":
    main()
