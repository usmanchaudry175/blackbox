#!/usr/bin/env python3
"""
Phase 7 — Python parser throughput benchmark, for cross-language
comparison against tools/bench_throughput.cpp's pure encode/decode
number.

Methodology mirrors bench_throughput.cpp exactly: same frame count,
same synthetic frame content (deterministic, sequence-derived, not
random — for repeatability), same round-trip (encode then decode),
per-frame timing collected for p50/p99.
"""

import time

from generate_frames import build_wire_frame, FIELD_COUNT
from blackbox_parser import decode_wire_frame

FRAME_COUNT = 500_000


def make_bench_samples(sequence: int) -> list[int]:
    # Matches make_bench_frame() in bench_throughput.cpp: samples[i] = sequence + i,
    # cast to int16 range via modulo to avoid overflow at large sequence values.
    return [((sequence + i) % 65536) - 32768 for i in range(FIELD_COUNT)]


def percentiles(latencies_ns: list[float]) -> tuple[float, float]:
    s = sorted(latencies_ns)
    n = len(s)
    return s[int(n * 0.50)], s[int(n * 0.99)]


def main():
    latencies_ns = []

    start = time.perf_counter()
    for seq in range(FRAME_COUNT):
        samples = make_bench_samples(seq)
        first = seq == 0

        t0 = time.perf_counter_ns()
        wire = build_wire_frame(session_id=1, sequence=seq, samples=samples,
                                 timestamp_ms=None, first_of_session=first)
        decoded = decode_wire_frame(wire)
        t1 = time.perf_counter_ns()

        latencies_ns.append(t1 - t0)
    end = time.perf_counter()

    total_seconds = end - start
    p50, p99 = percentiles(latencies_ns)
    frames_per_sec = FRAME_COUNT / total_seconds

    sample_wire = build_wire_frame(session_id=1, sequence=0, samples=make_bench_samples(0),
                                    timestamp_ms=None, first_of_session=True)

    print("--- Python pure encode/decode ---")
    print(f"  frames:          {FRAME_COUNT}")
    print(f"  wall time:       {total_seconds:.3f} s")
    print(f"  frames/sec:      {frames_per_sec:.0f}")
    print(f"  p50 latency:     {p50:.0f} ns ({p50/1000:.3f} us)")
    print(f"  p99 latency:     {p99:.0f} ns ({p99/1000:.3f} us)")
    print(f"  bytes/frame:     {len(sample_wire)}")


if __name__ == "__main__":
    main()