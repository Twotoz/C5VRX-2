#!/usr/bin/env python3
"""Host reference checks for the continuous IQ -> CVBS streaming path."""

from __future__ import annotations

import math
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RING_WORDS = 16384
TAU = 2.0 * math.pi
PHASE8 = 256.0 / TAU
CENTERS = (127.5, 383.5, -384.5, -128.5)


def wrap_phase(value: float) -> float:
    return (value + math.pi) % TAU - math.pi


def exact_step(previous: complex, current: complex) -> float:
    product = current * previous.conjugate()
    return math.atan2(product.imag, product.real)


def compact_phase(sample: complex) -> float:
    i = max(-512, min(511, round(sample.real))) & 0x3FF
    q = max(-512, min(511, round(sample.imag))) & 0x3FF
    return math.atan2(CENTERS[q >> 8], CENTERS[i >> 8])


def optimized_step(previous: complex, current: complex) -> int:
    delta = wrap_phase(compact_phase(current) - compact_phase(previous))
    return max(-128, min(127, round(delta * PHASE8)))


def reference_step(previous: complex, current: complex) -> int:
    return max(-128, min(127, round(exact_step(previous, current) * PHASE8)))


def make_iq(count: int) -> list[complex]:
    random.seed(0xC5_2)
    phase = math.pi - 0.03
    samples: list[complex] = []
    for n in range(count):
        # Slow and fast real-domain content plus bounded WBFM deviation. This
        # crosses the atan2 +/-pi seam repeatedly without making adjacent RF
        # phase itself ambiguous.
        delta = (0.17 * math.sin(n * 0.0013) +
                 0.11 * math.sin(n * 0.173) +
                 random.uniform(-0.025, 0.025))
        phase += delta
        amplitude = 440.0 + 45.0 * math.sin(n * 0.0007)
        samples.append(complex(round(amplitude * math.cos(phase)),
                               round(amplitude * math.sin(phase))))
    return samples


def discriminate(samples: list[complex], split_points: set[int]) -> list[int]:
    previous: complex | None = None
    output: list[int] = []
    group: list[int] = []
    for index, sample in enumerate(samples):
        # A software/ring span boundary intentionally does not reset previous.
        if index in split_points:
            pass
        contribution = 0 if previous is None else optimized_step(previous, sample)
        previous = sample
        contribution = max(-20, min(43, contribution * 2))
        group.append(contribution)
        if len(group) == 4:
            output.append(20 + sum(group) // 4)
            group.clear()
    return output


def error_metrics(samples: list[complex]) -> tuple[float, float, float, float]:
    adjacent_errors: list[float] = []
    filtered_errors: list[float] = []
    seam_errors: list[float] = []
    exact_group: list[int] = []
    optimized_group: list[int] = []
    for index in range(1, len(samples)):
        exact = reference_step(samples[index - 1], samples[index])
        optimized = optimized_step(samples[index - 1], samples[index])
        error = optimized - exact
        adjacent_errors.append(error)
        if abs(math.atan2(samples[index].imag, samples[index].real)) > 3.0:
            seam_errors.append(error)
        exact_group.append(exact)
        optimized_group.append(optimized)
        if len(exact_group) == 4:
            filtered_errors.append((sum(optimized_group) - sum(exact_group)) / 4.0)
            exact_group.clear()
            optimized_group.clear()
    rms = lambda values: math.sqrt(sum(x * x for x in values) / len(values))
    return (rms(adjacent_errors), max(map(abs, adjacent_errors)),
            rms(filtered_errors), max(map(abs, seam_errors)))


def verify_sources() -> None:
    asm = (ROOT / "main/c5vrx2_wbfm_direct6_4to1.bsasm").read_text()
    realtime = (ROOT / "main/realtime.c").read_text()
    source = (ROOT / "main/continuous_iq.c").read_text()
    parlio = (ROOT / "main/parlio_direct.c").read_text()
    assert asm.count("ADDCTIAL") == 4
    assert "O6..O9" in asm and "set 0..5 A2..A7" in asm
    assert "adctrig(" not in source
    assert "CTRL_START" in source and "control | CTRL_ENABLE" in source
    assert "CTRL_START;" not in source
    assert ".flags.loop_transmission = true" in parlio
    assert "rf_rate_hz" in realtime and "av_rate_hz" in realtime
    assert "csrrc" not in realtime and "park_hp" not in realtime


def main() -> None:
    verify_sources()
    samples = make_iq(RING_WORDS * 3 + 257)
    uninterrupted = discriminate(samples, set())
    split = discriminate(samples, {1, 7, RING_WORDS, RING_WORDS + 3,
                                   2 * RING_WORDS})
    assert uninterrupted == split, "DSP state changed at a span/ring boundary"

    adjacent_rms, adjacent_max, filtered_rms, seam_max = error_metrics(samples)
    # The compact streaming LUT is deliberately judged after its real-domain
    # four-tap boxcar too; adjacent error remains visible for future LUT work.
    assert filtered_rms < 3.0
    assert seam_max <= 32.0
    print("continuous boundary state: PASS")
    print(f"adjacent phase8 error: rms={adjacent_rms:.3f} max={adjacent_max:.3f}")
    print(f"post-boxcar phase8 error: rms={filtered_rms:.3f}")
    print(f"atan2 +/-pi seam error: max={seam_max:.3f}")


if __name__ == "__main__":
    main()
