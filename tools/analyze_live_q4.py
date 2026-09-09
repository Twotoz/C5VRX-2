#!/usr/bin/env python3
"""Analyze a frozen LIVE Q4/I4 ring exactly as the production WBFM sees it."""

from __future__ import annotations

import argparse
import math
import struct
from pathlib import Path

import numpy as np


MAGIC = 0x31564243
HEADER_BYTES = 64
RAW_RATE_HZ = 40_000_000.0
SELECTED_RATE_HZ = RAW_RATE_HZ / 2.0
NTSC_LINE_HZ = 15_734.264
RAW_BLOCK_BYTES = 4096
TAU = 2.0 * math.pi


def signed_nibble(value: np.ndarray) -> np.ndarray:
    value = value.astype(np.int16)
    return np.where(value & 8, value - 16, value).astype(np.float64)


def phase5(raw: np.ndarray) -> np.ndarray:
    q = signed_nibble(raw & 0x0f)
    i = signed_nibble(raw >> 4)
    return (np.rint(np.arctan2(q, i) * 32.0 / TAU).astype(np.int16) & 31)


def wrapped_delta(phase: np.ndarray) -> np.ndarray:
    delta = (phase[1:] - phase[:-1]) & 31
    return np.where(delta >= 16, delta - 32, delta).astype(np.int16)


def load(path: Path) -> tuple[dict[str, int], np.ndarray]:
    blob = path.read_bytes()
    if len(blob) < HEADER_BYTES:
        raise ValueError("capture is shorter than its header")
    fields = struct.unpack_from("<IHHIIIII2BHII6I", blob)
    names = (
        "magic", "version", "header_bytes", "payload_bytes", "iq_rate_hz",
        "cvbs_rate_hz", "writer_pointer", "dump_control", "minimum",
        "maximum", "reserved0", "sample_sum", "transitions",
        "reserved1", "reserved2", "reserved3", "reserved4", "reserved5",
        "reserved6",
    )
    header = dict(zip(names, fields))
    if header["magic"] != MAGIC or header["version"] != 3:
        raise ValueError("not a complete LIVE snapshot v3")
    if header["header_bytes"] != HEADER_BYTES:
        raise ValueError("unsupported LIVE header size")
    if header["cvbs_rate_hz"] != 0:
        raise ValueError("snapshot contains post-WBFM data, not raw Q4/I4")
    end = HEADER_BYTES + header["payload_bytes"]
    if len(blob) < end:
        raise ValueError("capture payload is truncated")
    raw = np.frombuffer(blob, dtype=np.uint8, count=header["payload_bytes"],
                        offset=HEADER_BYTES).copy()
    return header, raw


def correlation(signal: np.ndarray, nominal_lag: float) -> tuple[int, float]:
    centered = signal.astype(np.float64) - float(np.mean(signal))
    best = (0, -2.0)
    for lag in range(int(nominal_lag) - 24, int(nominal_lag) + 25):
        left, right = centered[:-lag], centered[lag:]
        denom = float(np.sqrt(np.dot(left, left) * np.dot(right, right)))
        score = float(np.dot(left, right) / denom) if denom else 0.0
        if score > best[1]:
            best = (lag, score)
    return best


def analyze_parity(raw: np.ndarray, parity: int) -> tuple[np.ndarray, np.ndarray]:
    selected = raw[parity::2]
    phases = phase5(selected)
    return phases, wrapped_delta(phases)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    header, raw = load(args.capture)

    print(f"payload:             {raw.size} raw Q4/I4 bytes")
    print(f"stored range:        {header['minimum']}..{header['maximum']}")
    print(f"unique raw bytes:    {np.unique(raw).size}/256")
    print(f"writer/dump ctrl:    {header['writer_pointer']}/"
          f"0x{header['dump_control']:08x}")
    print(f"adjacent duplicates: {np.count_nonzero(raw[1:] == raw[:-1])}/"
          f"{raw.size - 1}")

    all_phases = phase5(raw)
    adjacent = wrapped_delta(all_phases)
    q = signed_nibble(raw & 0x0f).astype(np.int16)
    i = signed_nibble(raw >> 4).astype(np.int16)
    amplitude2 = i * i + q * q
    adjacent_near_wrap = (adjacent <= -14) | (adjacent >= 14)
    print(f"40M adjacent near pi: {np.count_nonzero(adjacent_near_wrap)}/"
          f"{adjacent.size} ({np.mean(adjacent_near_wrap):.4%})")

    for parity in (0, 1):
        phases, delta = analyze_parity(raw, parity)
        starts = np.arange(parity, raw.size - 2, 2)
        unfolded_pair = adjacent[starts] + adjacent[starts + 1]
        ambiguous_pair = (unfolded_pair < -16) | (unfolded_pair > 15)
        pair_min_amplitude2 = np.minimum.reduce((
            amplitude2[starts], amplitude2[starts + 1],
            amplitude2[starts + 2]))
        strong = pair_min_amplitude2 >= 64
        radians = delta.astype(np.float64) * TAU / 32.0
        mean_angle = float(np.angle(np.mean(np.exp(1j * radians))))
        offset_hz = mean_angle / TAU * SELECTED_RATE_HZ
        centered = np.angle(np.exp(1j * (radians - mean_angle)))
        centered_steps = centered * 32.0 / TAU
        near_wrap = int(np.count_nonzero((delta <= -14) | (delta >= 14)))
        at_wrap = int(np.count_nonzero((delta == -16) | (delta == 15)))
        p99 = float(np.percentile(np.abs(centered_steps), 99.0))
        p999 = float(np.percentile(np.abs(centered_steps), 99.9))
        codes = np.clip(20 + delta.astype(np.int16) * 6, 0, 63)
        lag, corr = correlation(delta, SELECTED_RATE_HZ / NTSC_LINE_HZ)
        print(f"\nproduction parity {parity} (raw[{parity}::2]):")
        print(f"  carrier/DC offset: {offset_hz / 1e6:+.4f} MHz "
              f"({mean_angle * 32.0 / TAU:+.3f} phase5 steps)")
        print(f"  delta range:       {int(delta.min())}..{int(delta.max())}")
        print(f"  abs centered p99:  {p99:.3f} steps")
        print(f"  abs centered p999: {p999:.3f} steps")
        print(f"  near +/-pi:        {near_wrap}/{delta.size} "
              f"({near_wrap / delta.size:.4%})")
        print(f"  -16/+15 bins:      {at_wrap}/{delta.size} "
              f"({at_wrap / delta.size:.4%})")
        print(f"  n->n+2 ambiguous:  {np.count_nonzero(ambiguous_pair)}/"
              f"{ambiguous_pair.size} ({np.mean(ambiguous_pair):.4%})")
        if np.any(strong):
            strong_ambiguous = ambiguous_pair[strong]
            print(f"  ambiguous amp2>=64:{np.count_nonzero(strong_ambiguous)}/"
                  f"{strong_ambiguous.size} "
                  f"({np.mean(strong_ambiguous):.4%})")
        print(f"  effective DAC:     {len(np.unique(codes))} levels "
              f"{np.unique(codes).tolist()}")
        print(f"  NTSC correlation:  {corr:.6f} at {lag} samples "
              f"({SELECTED_RATE_HZ / lag:.3f} Hz)")

        # selected[k] maps to raw[2*k+parity]. Report the discriminator sample
        # immediately after every 4096-byte RX block boundary and the cyclic
        # end-to-start transition separately.
        boundary = []
        for raw_boundary in range(RAW_BLOCK_BYTES, raw.size, RAW_BLOCK_BYTES):
            k = (raw_boundary - parity + 1) // 2
            if 1 <= k < phases.size:
                boundary.append((raw_boundary, int(delta[k - 1])))
        cyclic = int(((int(phases[0]) - int(phases[-1]) + 16) & 31) - 16)
        print(f"  block deltas:      {boundary}")
        print(f"  cyclic end->start: {cyclic:+d}")


if __name__ == "__main__":
    main()
