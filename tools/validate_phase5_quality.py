#!/usr/bin/env python3
"""Validate the dual-purpose five-bit polar WBFM LUT and its assembly map."""

from __future__ import annotations

import argparse
import functools
import math
from pathlib import Path


TAU = 2.0 * math.pi
def signed_bucket_center(code: int, bits: int) -> float:
    width = 1 << (10 - bits)
    center = code * width + (width - 1) * 0.5
    return center - 1024.0 if center >= 512.0 else center


def wrapped(value: float) -> float:
    return (value + math.pi) % TAU - math.pi


def exact_phase(packed: int) -> float:
    q = signed_bucket_center(packed & 0x0F, 4)
    i = signed_bucket_center(packed >> 4, 4)
    return math.atan2(q, i)


def compact_iq5_phase(packed: int) -> float:
    compact = ((packed >> 1) & 0x07) | (((packed >> 6) & 0x03) << 3)
    q = signed_bucket_center(compact & 0x07, 3)
    i = signed_bucket_center(compact >> 3, 2)
    return math.atan2(q, i)


def phase5(packed: int) -> int:
    return round(exact_phase(packed) * 32.0 / TAU) & 0x1F


def phase5_state(packed: int) -> int:
    return phase5(packed)


@functools.lru_cache(maxsize=None)
def phase5_centroids() -> list[float]:
    result: list[float] = []
    for state in range(32):
        members = [exact_phase(packed) for packed in range(256)
                   if phase5_state(packed) == state]
        sine = sum(math.sin(value) for value in members)
        cosine = sum(math.cos(value) for value in members)
        result.append(math.atan2(sine, cosine))
    return result


@functools.lru_cache(maxsize=None)
def phase5_centroid_phase8() -> list[int]:
    return [round(value * 256.0 / TAU) for value in phase5_centroids()]


def centroid_delta_phase8(previous: int, current: int) -> int:
    centers = phase5_centroid_phase8()
    delta = (centers[current] - centers[previous] + 128) % 256 - 128
    return delta


def scale_real_sum(value: int, calibration_gain: int = 2) -> int:
    numerator = value * (calibration_gain + 1)
    return -((-numerator + 2) // 4) if numerator < 0 else (numerator + 2) // 4


def build_lut(calibration_gain: int = 2, pedestal: int = 26) -> list[int]:
    lut = [0] * 1024
    for previous in range(32):
        for current in range(32):
            delta = centroid_delta_phase8(previous, current)
            code = max(0, min(63, pedestal + scale_real_sum(delta, calibration_gain)))
            lut[(previous << 5) | current] = code
    for packed in range(256):
        lut[packed] |= phase5_state(packed) << 8
    return lut


def rms_degrees(errors: list[float]) -> float:
    return math.degrees(math.sqrt(sum(value * value for value in errors) /
                                  len(errors)))


def validate_sources(repo: Path) -> None:
    asm = (repo / "main" / "c5vrx2_wbfm_q4_phase5_2to1.bsasm").read_text()
    source = (repo / "main" / "wbfm_q4.c").read_text()
    realtime = (repo / "main" / "realtime.c").read_text()
    defaults = (repo / "sdkconfig.quality.defaults").read_text()
    # The first read primes the phase LUT once. Thereafter the emit bundle
    # reads the next pair while writing the preceding pair, leaving exactly
    # two steady-state bundles per output.
    assert asm.count("read 16") == 2
    assert asm.count("write 8") == 1
    assert asm.count("set 16 8") == 2
    assert "jmp address_delta" in asm
    assert "set 16 L8" in asm and "set 20 L12" in asm
    assert "set 21 O8" in asm and "set 25 O12" in asm
    assert "set 0..5 L0..L5" in asm
    assert "lut " + " ".join(map(str, build_lut())) in asm
    assert "q4_phase5" in source
    assert "q4_phase5_state" in source
    assert "c5vrx2_wbfm_q4_phase5_program" in realtime
    assert "CONFIG_C5VRX2_MODE_LIVE=y" in defaults
    assert "CONFIG_C5VRX2_WBFM_PHASE5_QUALITY=y" in defaults


def rewrite_embedded_lut(repo: Path) -> None:
    path = repo / "main" / "c5vrx2_wbfm_q4_phase5_2to1.bsasm"
    lines = path.read_text().splitlines()
    replacement = "lut " + " ".join(map(str, build_lut()))
    lut_lines = [index for index, line in enumerate(lines)
                 if line.startswith("lut ")]
    if len(lut_lines) != 1:
        raise RuntimeError("expected exactly one embedded LUT")
    lines[lut_lines[0]] = replacement
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rewrite-lut", action="store_true",
                        help="regenerate the embedded assembly LUT")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    if args.rewrite_lut:
        rewrite_embedded_lut(repo)
    validate_sources(repo)
    lut = build_lut()
    assert len(lut) == 1024

    # Prove that overlapping raw-phase and pair-delta addresses do not collide:
    # they occupy disjoint bits of the same 16-bit word.
    for packed in range(256):
        assert ((lut[packed] >> 8) & 0x1F) == phase5_state(packed)
    for previous in range(32):
        for current in range(32):
            index = (previous << 5) | current
            delta = centroid_delta_phase8(previous, current)
            expected = max(0, min(63, 26 + scale_real_sum(delta, 2)))
            assert (lut[index] & 0x3F) == expected

    cartesian_errors: list[float] = []
    polar_errors: list[float] = []
    for packed in range(256):
        q = signed_bucket_center(packed & 0x0F, 4)
        i = signed_bucket_center(packed >> 4, 4)
        if i * i + q * q <= 128.0 * 128.0:
            continue
        reference = exact_phase(packed)
        cartesian_errors.append(abs(wrapped(compact_iq5_phase(packed) -
                                            reference)))
        quantized = phase5_centroids()[phase5_state(packed)]
        polar_errors.append(abs(wrapped(quantized - reference)))

    cartesian_rms = rms_degrees(cartesian_errors)
    polar_rms = rms_degrees(polar_errors)
    cartesian_max = math.degrees(max(cartesian_errors))
    polar_max = math.degrees(max(polar_errors))
    assert polar_rms < cartesian_rms * 0.4
    # Keep the worst reliable-vector error comfortably below half of the old
    # Cartesian quantizer's maximum.
    assert polar_max < cartesian_max * 0.5

    # Explicit branch-cut check around +pi/-pi remains a small positive step.
    assert centroid_delta_phase8(15, 16) == 8

    output_codes = sorted({value & 0x3F for value in lut})
    assert len(output_codes) >= 32

    print("five-bit polar WBFM quality validation PASS")
    print(f"  Q3/I2 Cartesian phase error: {cartesian_rms:.2f} deg RMS, "
          f"{cartesian_max:.2f} deg max")
    print(f"  phase5 polar phase error:     {polar_rms:.2f} deg RMS, "
          f"{polar_max:.2f} deg max")
    print(f"  centroid delta DAC levels:    {len(output_codes)}")
    print("  circular phase states:        32/32 preserved")
    print("  all 1024 dual-purpose LUT entries and modulo wrap verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
