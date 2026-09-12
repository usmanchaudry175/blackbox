#!/usr/bin/env python3
"""
Phase 7 — MATLAB export for offline flight analysis.

Reads a completed on-disk log (all segments, in order) and produces a
.mat file containing:
  - sequence, session_id, first_of_session, samples (Nx6)
  - timestamp_ms_raw: the amortised timestamp as actually recorded
    (D6) — NaN for frames that didn't carry one
  - timestamp_ms_interp: linearly interpolated between known timestamp
    markers, so every frame has a usable time value for plotting. Flat
    extrapolation before the first / after the last marker (a known
    limitation, not a corrected one — see note below)
  - gap_start / gap_end: sequence ranges missing from the log, so a
    MATLAB user can mask or flag those windows rather than plot
    through them as if they were continuous
  - decode_error_count: on-disk records that failed CRC/version
    checks (should be 0 for anything other than genuine corruption —
    truncation from a mid-write crash is NOT counted here, it's
    treated as a clean end of segment per D15, same as LogReader)

Requires: numpy, scipy (pip install numpy scipy --break-system-packages)
Usage: python3 export_matlab.py <base_path> <output.mat>
"""

import argparse
import os
import sys

import numpy as np
from scipy.io import savemat

from blackbox_parser import decode_raw_frame, expected_record_len, segment_path, DecodeError


def read_all_records(base_path: str):
    """Walks every segment in order, decoding records. A short read at
    the end of a segment is treated as a clean stop (D15/D18 — a
    truncated trailing record from a disk-full or kill-mid-write event
    is expected, not an error), matching LogReader::for_each_frame's
    behaviour exactly."""
    frames = []
    gaps = []
    decode_error_count = 0
    high_water_mark = None

    segment_index = 0
    while True:
        path = segment_path(base_path, segment_index)
        if not os.path.exists(path):
            break

        with open(path, "rb") as fh:
            while True:
                flags_byte = fh.read(1)
                if len(flags_byte) < 1:
                    break  # clean end of this segment

                flags = flags_byte[0]
                record_len = expected_record_len(flags)

                fh.seek(-1, os.SEEK_CUR)
                record = fh.read(record_len)
                if len(record) < record_len:
                    break  # truncated trailing record — clean stop, not an error

                try:
                    decoded = decode_raw_frame(record)
                except DecodeError:
                    decode_error_count += 1
                    continue

                seq = decoded["sequence"]
                if high_water_mark is not None and seq > high_water_mark + 1:
                    gaps.append((high_water_mark + 1, seq - 1))
                if high_water_mark is None or seq > high_water_mark:
                    high_water_mark = seq

                frames.append(decoded)

        segment_index += 1

    return frames, gaps, decode_error_count


def build_export_arrays(frames):
    n = len(frames)
    sequence = np.array([f["sequence"] for f in frames], dtype=np.int64)
    session_id = np.array([f["session_id"] for f in frames], dtype=np.int32)
    first_of_session = np.array([f["first_of_session"] for f in frames], dtype=bool)
    samples = np.array([f["samples"] for f in frames], dtype=np.int32)  # N x 6

    timestamp_ms_raw = np.array(
        [f["timestamp_ms"] if f["timestamp_ms"] is not None else np.nan for f in frames],
        dtype=np.float64,
    )

    known_mask = ~np.isnan(timestamp_ms_raw)
    if known_mask.sum() >= 2:
        known_seqs = sequence[known_mask].astype(np.float64)
        known_ts = timestamp_ms_raw[known_mask]
        # np.interp does flat extrapolation outside [known_seqs[0], known_seqs[-1]]
        # by default — frames before the first marker or after the last one
        # get that marker's value rather than a projected one. Acceptable for
        # a first pass; a proper fix would extrapolate using the nominal
        # sample rate instead.
        timestamp_ms_interp = np.interp(sequence.astype(np.float64), known_seqs, known_ts)
    else:
        timestamp_ms_interp = np.full(n, np.nan)

    return {
        "sequence": sequence,
        "session_id": session_id,
        "first_of_session": first_of_session,
        "samples": samples,
        "timestamp_ms_raw": timestamp_ms_raw,
        "timestamp_ms_interp": timestamp_ms_interp,
    }


def main():
    parser = argparse.ArgumentParser(description="Export a blackbox log to MATLAB .mat format")
    parser.add_argument("base_path", help="Log base path (without .NNNN.log suffix)")
    parser.add_argument("output_path", help="Output .mat file path")
    args = parser.parse_args()

    frames, gaps, decode_error_count = read_all_records(args.base_path)

    if not frames:
        print(f"error: no frames found for {args.base_path}", file=sys.stderr)
        sys.exit(1)

    data = build_export_arrays(frames)

    gap_start = np.array([g[0] for g in gaps], dtype=np.int64)
    gap_end = np.array([g[1] for g in gaps], dtype=np.int64)

    savemat(args.output_path, {
    "sequence": data["sequence"].reshape(-1, 1),
    "session_id": data["session_id"].reshape(-1, 1),
    "first_of_session": data["first_of_session"].reshape(-1, 1),
    "samples": data["samples"],  # already N x 6, correct as-is
    "timestamp_ms_raw": data["timestamp_ms_raw"].reshape(-1, 1),
    "timestamp_ms_interp": data["timestamp_ms_interp"].reshape(-1, 1),
    "gap_start": gap_start.reshape(-1, 1) if len(gap_start) else gap_start,
    "gap_end": gap_end.reshape(-1, 1) if len(gap_end) else gap_end,
    "decode_error_count": int(decode_error_count),
})

    print(f"Exported {len(frames)} frames to {args.output_path}")
    print(f"  gaps: {len(gaps)} ({int(gap_end.sum() - gap_start.sum() + len(gaps)) if len(gaps) else 0} missing frames total)")
    print(f"  decode errors: {decode_error_count}")


if __name__ == "__main__":
    main()