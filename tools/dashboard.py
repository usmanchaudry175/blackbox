#!/usr/bin/env python3
"""
Phase 7 — live dashboard tailing an active (or completed) blackbox log.

Polls the current segment file for new complete records, decodes them,
and displays a live-updating terminal view: throughput, sequence
gaps, and the most recent frames.

Design note: the on-disk log is written by LogWriter only from frames
that StreamReader::pop_frame() already accepted — duplicates and
out-of-order frames are rejected upstream (D17/D19) before ever
reaching disk. So this dashboard only needs to track a high-water
mark for gap detection, not a full seen-set — there is nothing to
deduplicate at this layer, by construction.

This tails ANY file being appended to, independent of whether
capture_cli is the process doing the writing — no live-process
integration required (Phase 7 scope decision).

Usage:
    python3 dashboard.py <base_path> [--poll-interval 0.1]
"""

import argparse
import os
import sys
import time
from collections import deque

from blackbox_parser import (
    decode_raw_frame,
    expected_record_len,
    segment_path,
    DecodeError,
)

CLEAR_SCREEN = "\033[2J\033[H"
RATE_WINDOW_SECONDS = 2.0
HISTORY_LEN = 15


class LiveTail:
    def __init__(self, base_path: str):
        self.base_path = base_path
        self.segment_index = 0
        self.fh = None
        self.position = 0

        self.total_frames = 0
        self.high_water_mark = None
        self.gaps_detected = 0
        self.missing_total = 0
        self.decode_errors = 0

        self.recent_frames = deque(maxlen=HISTORY_LEN)
        self.recent_timestamps = deque()

    def _open_current_segment(self) -> bool:
        path = segment_path(self.base_path, self.segment_index)
        if not os.path.exists(path):
            return False
        self.fh = open(path, "rb")
        self.position = 0
        return True

    def _maybe_advance_segment(self) -> bool:
        """If the current segment can't yield more complete records,
        check whether the NEXT segment already exists (rotation
        happened) and switch to it. Mirrors LogReader::for_each_frame's
        segment-advance logic."""
        next_path = segment_path(self.base_path, self.segment_index + 1)
        if os.path.exists(next_path):
            if self.fh:
                self.fh.close()
            self.segment_index += 1
            self.fh = open(next_path, "rb")
            self.position = 0
            return True
        return False

    def poll(self):
        """Reads as many complete records as are currently available.
        A short read at the end means either the writer hasn't
        finished this record yet (expected during live capture) or
        the segment has rotated — both handled by returning and
        letting the next poll (or a segment-advance check) resolve it."""
        if self.fh is None:
            if not self._open_current_segment():
                return

        while True:
            self.fh.seek(self.position)
            flags_byte = self.fh.read(1)
            if len(flags_byte) < 1:
                if not self._maybe_advance_segment():
                    return
                continue

            flags = flags_byte[0]
            record_len = expected_record_len(flags)

            self.fh.seek(self.position)
            record = self.fh.read(record_len)
            if len(record) < record_len:
                # Partial record — writer is mid-write. Do NOT advance
                # position; wait for the rest on the next poll.
                return

            try:
                decoded = decode_raw_frame(record)
            except DecodeError:
                self.decode_errors += 1
                self.position += record_len
                continue

            self._record_frame(decoded)
            self.position += record_len

    def _record_frame(self, decoded: dict):
        seq = decoded["sequence"]

        if self.high_water_mark is not None and seq > self.high_water_mark + 1:
            missing = seq - self.high_water_mark - 1
            self.gaps_detected += 1
            self.missing_total += missing

        if self.high_water_mark is None or seq > self.high_water_mark:
            self.high_water_mark = seq

        self.total_frames += 1
        self.recent_frames.append(decoded)

        now = time.monotonic()
        self.recent_timestamps.append(now)
        cutoff = now - RATE_WINDOW_SECONDS
        while self.recent_timestamps and self.recent_timestamps[0] < cutoff:
            self.recent_timestamps.popleft()

    def current_rate(self) -> float:
        if len(self.recent_timestamps) < 2:
            return 0.0
        span = self.recent_timestamps[-1] - self.recent_timestamps[0]
        if span <= 0:
            return 0.0
        return (len(self.recent_timestamps) - 1) / span


def render(tail: LiveTail, base_path: str):
    lines = []
    lines.append(f"blackbox live dashboard — {base_path}")
    lines.append(f"segment: {tail.segment_index}   position: {tail.position} bytes")
    lines.append("")
    lines.append(f"frames recorded:   {tail.total_frames}")
    lines.append(f"current rate:      {tail.current_rate():.1f} frames/sec  (last {RATE_WINDOW_SECONDS:.0f}s)")
    lines.append(f"high-water seq:    {tail.high_water_mark}")
    lines.append(f"gaps detected:     {tail.gaps_detected}  ({tail.missing_total} missing frames total)")
    lines.append(f"decode errors:     {tail.decode_errors}")
    lines.append("")
    lines.append(f"last {HISTORY_LEN} frames:")
    lines.append(f"  {'seq':>8}  {'session':>7}  samples")
    for f in tail.recent_frames:
        samples_str = ", ".join(str(s) for s in f["samples"])
        lines.append(f"  {f['sequence']:>8}  {f['session_id']:>7}  [{samples_str}]")
    lines.append("")
    lines.append("Ctrl+C to exit")

    sys.stdout.write(CLEAR_SCREEN)
    sys.stdout.write("\n".join(lines))
    sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description="Live dashboard for a blackbox log")
    parser.add_argument("base_path", help="Log base path (without .NNNN.log suffix)")
    parser.add_argument("--poll-interval", type=float, default=0.1,
                         help="Seconds between polls for new data")
    args = parser.parse_args()

    tail = LiveTail(args.base_path)

    try:
        while True:
            tail.poll()
            render(tail, args.base_path)
            time.sleep(args.poll_interval)
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()