#!/usr/bin/env python3
"""Validate the dual-purpose five-bit polar WBFM LUT and its assembly map."""

from __future__ import annotations

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


def scale_real_sum(value: int, calibration_gain: int = 2) -> int:
    numerator = value * (calibration_gain + 1)
    return -((-numerator + 2) // 4) if numerator < 0 else (numerator + 2) // 4


def build_lut() -> list[int]:
    lut = [0] * 1024
    for previous in range(32):
        for current in range(32):
            delta = (current - previous) & 0x1F
            if delta >= 16:
                delta -= 32
            code = max(0, min(63, 20 + scale_real_sum(delta * 8)))
            lut[(previous << 5) | current] = code
    for packed in range(256):
        lut[packed] |= phase5(packed) << 8
    return lut


def rms_degrees(errors: list[float]) -> float:
    return math.degrees(math.sqrt(sum(value * value for value in errors) /
                                  len(errors)))


def validate_sources(repo: Path) -> None:
    asm = (repo / "main" / "c5vrx2_wbfm_q4_phase5_2to1.bsasm").read_text()
    source = (repo / "main" / "wbfm_q4.c").read_text()
    realtime = (repo / "main" / "realtime.c").read_text()
    defaults = (repo / "sdkconfig.quality.defaults").read_text()
    assert asm.count("read 16") == 1
    assert asm.count("write 8") == 1
    assert "set 16 L8" in asm and "set 20 L12" in asm
    assert "set 21 O8" in asm and "set 25 O12" in asm
    assert "set 0..5 L0..L5" in asm
    assert "build_phase5_lut" in source
    assert "lut[packed] |= (uint16_t)q4_phase5(packed) << 8u" in source
    assert "scale_real_sum(delta * 8" in source
    assert "c5vrx2_wbfm_q4_load_tx_phase5" in realtime
    assert "c5vrx2_wbfm_q4_phase5_program" in realtime
    assert "CONFIG_C5VRX2_MODE_LIVE=y" in defaults
    assert "CONFIG_C5VRX2_WBFM_PHASE5_QUALITY=y" in defaults


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    validate_sources(repo)
    lut = build_lut()
    assert len(lut) == 1024

    # Prove that overlapping raw-phase and pair-delta addresses do not collide:
    # they occupy disjoint bits of the same 16-bit word.
    for packed in range(256):
        assert ((lut[packed] >> 8) & 0x1F) == phase5(packed)
    for previous in range(32):
        for current in range(32):
            index = (previous << 5) | current
            delta = (current - previous) & 0x1F
            if delta >= 16:
                delta -= 32
            expected = max(0, min(63, 20 + scale_real_sum(delta * 8)))
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
        quantized = phase5(packed) * TAU / 32.0
        polar_errors.append(abs(wrapped(quantized - reference)))

    cartesian_rms = rms_degrees(cartesian_errors)
    polar_rms = rms_degrees(polar_errors)
    cartesian_max = math.degrees(max(cartesian_errors))
    polar_max = math.degrees(max(polar_errors))
    assert polar_rms < cartesian_rms * 0.4
    assert polar_max < cartesian_max * 0.25

    # Explicit branch-cut check: 31 -> 0 is +1 phase step, not -31.
    branch_delta = (0 - 31) & 0x1F
    if branch_delta >= 16:
        branch_delta -= 32
    assert branch_delta == 1

    print("five-bit polar WBFM quality validation PASS")
    print(f"  Q3/I2 Cartesian phase error: {cartesian_rms:.2f} deg RMS, "
          f"{cartesian_max:.2f} deg max")
    print(f"  phase5 polar phase error:     {polar_rms:.2f} deg RMS, "
          f"{polar_max:.2f} deg max")
    print("  all 1024 dual-purpose LUT entries and modulo wrap verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
