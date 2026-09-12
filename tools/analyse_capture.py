#!/usr/bin/env python3
"""
Phase 7 preview — numpy/pandas frame analysis.

Captures wire-format frames (from a PTY slave, matching tools/generate_frames.py),
decodes them with a vectorized numpy structured dtype (not a per-frame Python
loop), and reports gap/duplicate detection plus clock drift via linear
regression on the amortised timestamp (design.md D6).

Framing/CRC validation (COBS decode + CRC-16 check) is inherently sequential —
you can't find delimiters or verify a checksum in bulk before you've walked
the bytes. The vectorization happens at the field-extraction stage: once N
frames are known-valid, fixed-width raw bytes, parsing all their fields is
one numpy call instead of N separate struct.unpack() calls. That's the
actual, honest performance/design point for the Phase 7 benchmark.
"""

import argparse
import os
import sys
import time

import numpy as np
import pandas as pd

# Reuse the tested encode/CRC logic from the generator rather than
# reimplementing it a third time.
sys.path.insert(0, os.path.dirname(__file__))
from generate_frames import (
    crc16_ccitt_false, build_wire_frame, FIELD_COUNT,
    FLAG_TIMESTAMP, VERSION_SHIFT,
)

DELIMITER = 0x00

# ---- numpy structured dtypes, matching frame.hpp's D12 field ordering ----
# '>' = big-endian, per D11 — this must match the C++ layout exactly or
# every field silently comes out wrong with no error raised.
DTYPE_NO_TS = np.dtype([
    ("flags", ">u1"),
    ("session_id", ">u1"),
    ("sequence", ">u4"),
    ("samples", ">i2", (FIELD_COUNT,)),
    ("crc", ">u2"),
])
DTYPE_WITH_TS = np.dtype([
    ("flags", ">u1"),
    ("session_id", ">u1"),
    ("sequence", ">u4"),
    ("samples", ">i2", (FIELD_COUNT,)),
    ("timestamp_ms", ">u4"),
    ("crc", ">u2"),
])


def cobs_decode(data: bytes) -> bytes | None:
    """Standard COBS decode. Returns None on malformed input rather than
    raising, matching decode()'s "reject, don't crash" contract."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        if code == 0:
            return None
        i += 1
        end = i + code - 1
        if end > n:
            return None
        out.extend(data[i:end])
        i = end
        if code < 0xFF and i < n:
            out.append(0)
    return bytes(out)


def split_frames(stream: bytes) -> tuple[list[bytes], bytes]:
    """Splits a byte stream on the 0x00 delimiter. Returns (complete
    COBS-encoded frames, leftover partial bytes still awaiting a delimiter)."""
    parts = stream.split(bytes([DELIMITER]))
    if stream.endswith(bytes([DELIMITER])):
        return parts[:-1], b""
    return parts[:-1], parts[-1]


def validate_and_bucket(raw_frames: list[bytes]) -> tuple[list[bytes], list[bytes], int]:
    """COBS-decodes and CRC-validates each frame, sorting into the two
    fixed-width buckets numpy needs. Returns (no_ts_frames, with_ts_frames,
    rejected_count)."""
    no_ts, with_ts, rejected = [], [], 0

    for wire in raw_frames:
        raw = cobs_decode(wire)
        if raw is None or len(raw) not in (DTYPE_NO_TS.itemsize, DTYPE_WITH_TS.itemsize):
            rejected += 1
            continue

        has_ts = bool(raw[0] & FLAG_TIMESTAMP)
        expected_len = DTYPE_WITH_TS.itemsize if has_ts else DTYPE_NO_TS.itemsize
        if len(raw) != expected_len:
            rejected += 1
            continue

        crc_off = len(raw) - 2
        computed = crc16_ccitt_false(raw[:crc_off])
        received = (raw[crc_off] << 8) | raw[crc_off + 1]
        if computed != received:
            rejected += 1
            continue

        (with_ts if has_ts else no_ts).append(raw)

    return no_ts, with_ts, rejected


def frames_to_dataframe(no_ts_frames: list[bytes], with_ts_frames: list[bytes]) -> pd.DataFrame:
    """The actual vectorized step: stack same-length validated frames into
    one buffer each, view through the structured dtype once, done."""
    dfs = []

    if no_ts_frames:
        buf = b"".join(no_ts_frames)
        arr = np.frombuffer(buf, dtype=DTYPE_NO_TS)
        df = pd.DataFrame({
            "sequence": arr["sequence"],
            "session_id": arr["session_id"],
            "timestamp_ms": np.nan,
            **{f"s{i}": arr["samples"][:, i] for i in range(FIELD_COUNT)},
        })
        dfs.append(df)

    if with_ts_frames:
        buf = b"".join(with_ts_frames)
        arr = np.frombuffer(buf, dtype=DTYPE_WITH_TS)
        df = pd.DataFrame({
            "sequence": arr["sequence"],
            "session_id": arr["session_id"],
            "timestamp_ms": arr["timestamp_ms"].astype(float),
            **{f"s{i}": arr["samples"][:, i] for i in range(FIELD_COUNT)},
        })
        dfs.append(df)

    if not dfs:
        return pd.DataFrame()

    return pd.concat(dfs, ignore_index=True)


def analyze(df: pd.DataFrame, nominal_rate_hz: float, rejected: int):
    print(f"\n--- Analysis ---")
    print(f"Valid frames decoded: {len(df)}")
    print(f"Rejected (bad CRC / malformed / wrong length): {rejected}")

    if df.empty:
        return

    seq = df["sequence"].to_numpy()

    # Duplicate detection
    dup_count = df.duplicated(subset="sequence").sum()
    print(f"Duplicate sequence numbers: {dup_count}")

    # Gap detection — assumes sequence numbers were issued contiguously
    # by the generator (0..max), independent of arrival order.
    seen = set(seq.tolist())
    if seen:
        full_range = set(range(min(seen), max(seen) + 1))
        missing = full_range - seen
        print(f"Gaps detected (missing sequence numbers): {len(missing)} "
              f"of {len(full_range)} expected ({100 * len(missing) / len(full_range):.2f}%)")

    # Reordering evidence — did sequence numbers arrive out of order?
    arrival_order_seq = df["sequence"].to_numpy()
    is_monotonic = np.all(np.diff(arrival_order_seq) >= 0)
    print(f"Arrived strictly in sequence order: {is_monotonic}")

    # Clock drift — linear regression of timestamp vs sequence, per D6.
    ts_rows = df.dropna(subset=["timestamp_ms"])
    if len(ts_rows) >= 2:
        slope, intercept = np.polyfit(ts_rows["sequence"], ts_rows["timestamp_ms"], 1)
        nominal_period_ms = 1000.0 / nominal_rate_hz
        drift_ppm = (slope - nominal_period_ms) / nominal_period_ms * 1e6
        print(f"Measured inter-frame period: {slope:.4f} ms (nominal: {nominal_period_ms:.4f} ms)")
        print(f"Estimated clock drift: {drift_ppm:+.1f} ppm")
    else:
        print("Not enough timestamped frames for a drift estimate.")


def capture_from_pty(path: str, duration_s: float) -> bytes:
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    data = bytearray()
    deadline = time.monotonic() + duration_s
    try:
        while time.monotonic() < deadline:
            try:
                chunk = os.read(fd, 4096)
                data.extend(chunk)
            except BlockingIOError:
                time.sleep(0.01)
    finally:
        os.close(fd)
    return bytes(data)


def self_test():
    """Builds known frames in-process (bypassing the PTY entirely) and
    verifies the numpy decode path reconstructs them exactly. Run this
    before ever trusting output from a live capture."""
    print("Running self-test...")
    expected = []
    wire_frames = []
    for seq in range(50):
        samples = [seq, -seq, seq * 2, -seq * 2, seq * 3, -seq * 3]
        has_ts = seq % 10 == 0
        ts = seq * 1000 if has_ts else None
        expected.append((seq, samples, ts))
        wire_frames.append(build_wire_frame(1, seq, samples, ts, seq == 0))

    stream = b"".join(wire_frames)
    raw_frames, leftover = split_frames(stream)
    assert leftover == b"", f"unexpected leftover bytes: {leftover!r}"

    no_ts, with_ts, rejected = validate_and_bucket(raw_frames)
    assert rejected == 0, f"self-test frames were rejected: {rejected}"

    df = frames_to_dataframe(no_ts, with_ts)
    df = df.sort_values("sequence").reset_index(drop=True)

    for i, (seq, samples, ts) in enumerate(expected):
        row = df.iloc[i]
        assert row["sequence"] == seq, f"sequence mismatch at {i}"
        for j in range(FIELD_COUNT):
            assert row[f"s{j}"] == samples[j], f"sample {j} mismatch at seq {seq}"
        if ts is not None:
            assert row["timestamp_ms"] == ts, f"timestamp mismatch at seq {seq}"

    print(f"Self-test passed: {len(expected)} frames encoded, decoded, and verified byte-exact.")


def main():
    parser = argparse.ArgumentParser(description="Analyze captured blackbox frames with numpy/pandas")
    parser.add_argument("--source", help="PTY slave path to read from, e.g. /dev/pts/5")
    parser.add_argument("--duration", type=float, default=5.0, help="Seconds to capture")
    parser.add_argument("--rate", type=float, default=100.0, help="Nominal frame rate, for drift calc")
    parser.add_argument("--self-test", action="store_true", help="Run the self-test instead of capturing")
    args = parser.parse_args()

    if args.self_test or not args.source:
        self_test()
        return

    print(f"Capturing from {args.source} for {args.duration}s...")
    stream = capture_from_pty(args.source, args.duration)
    raw_frames, leftover = split_frames(stream)
    print(f"Captured {len(raw_frames)} candidate frames ({len(leftover)} leftover bytes, expected at capture cutoff)")

    no_ts, with_ts, rejected = validate_and_bucket(raw_frames)
    df = frames_to_dataframe(no_ts, with_ts)
    analyze(df, args.rate, rejected)


if __name__ == "__main__":
    main()