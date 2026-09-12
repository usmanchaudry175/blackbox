#!/usr/bin/env python3
"""
Phase 2 — synthetic frame generator.

Emits valid blackbox wire-format frames over a pseudo-terminal (PTY),
matching what real serial hardware will look like in Phase 8. Supports
injecting loss, duplication, reordering, and bit corruption for testing
the C++ reader's recovery behaviour (Phase 3/6).

Wire format matches include/blackbox/frame.hpp exactly:
  flags(1) session(1) sequence(4) payload(12) [timestamp(4)] crc16(2)
  -> COBS-encoded -> 0x00 delimiter appended
See docs/design.md for the full rationale (D1-D14).
"""

import argparse
import os
import pty
import random
import struct
import sys
import time
import tty
import json

manifest = []
# ---- Must match include/blackbox/frame.hpp exactly ----
FLAG_TIMESTAMP = 1 << 0
FLAG_FIRST_OF_SESSION = 1 << 1
VERSION_SHIFT = 5
FORMAT_VERSION = 1
DELIMITER = 0x00

FIELD_COUNT = 6  # accelX, accelY, accelZ, gyroX, gyroY, gyroZ


def crc16_ccitt_false(data: bytes) -> int:
    """Matches src/crc16.cpp exactly: poly 0x1021, init 0xFFFF, no reflect."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    """Standard COBS encode. Must produce byte-identical output to
    src/cobs.cpp for the same input, since Phase 3's reader will be
    tested against both."""
    out = bytearray()
    idx = 0
    while True:
        next_zero = data.find(b'\x00', idx)
        if next_zero == -1:
            chunk = data[idx:]
            # Split any remaining chunk longer than 254 bytes.
            while len(chunk) >= 0xFF:
                out.append(0xFF)
                out.extend(chunk[:0xFE])
                chunk = chunk[0xFE:]
            out.append(len(chunk) + 1)
            out.extend(chunk)
            break
        else:
            chunk = data[idx:next_zero]
            while len(chunk) >= 0xFF:
                out.append(0xFF)
                out.extend(chunk[:0xFE])
                chunk = chunk[0xFE:]
            out.append(len(chunk) + 1)
            out.extend(chunk)
            idx = next_zero + 1
    return bytes(out)


def build_raw_frame(session_id: int, sequence: int, samples: list[int],
                     timestamp_ms: int | None, first_of_session: bool) -> bytes:
    """Builds the raw (pre-COBS) frame per the D12 field ordering."""
    assert len(samples) == FIELD_COUNT

    flags = (FORMAT_VERSION << VERSION_SHIFT) & 0xFF
    if timestamp_ms is not None:
        flags |= FLAG_TIMESTAMP
    if first_of_session:
        flags |= FLAG_FIRST_OF_SESSION

    body = struct.pack(">BBI", flags, session_id, sequence)
    body += struct.pack(">" + "h" * FIELD_COUNT, *samples)
    if timestamp_ms is not None:
        body += struct.pack(">I", timestamp_ms)

    crc = crc16_ccitt_false(body)
    body += struct.pack(">H", crc)
    return body


def build_wire_frame(*args, **kwargs) -> bytes:
    raw = build_raw_frame(*args, **kwargs)
    return cobs_encode(raw) + bytes([DELIMITER])


def corrupt_bit(wire: bytes) -> bytes:
    """Flips one random bit anywhere in the wire-format bytes, including
    possibly inside the delimiter itself — deliberately, since real
    corruption doesn't respect frame structure."""
    if not wire:
        return wire
    b = bytearray(wire)
    byte_idx = random.randrange(len(b))
    bit_idx = random.randrange(8)
    b[byte_idx] ^= (1 << bit_idx)
    return bytes(b)


def main():
    parser = argparse.ArgumentParser(description="Synthetic blackbox frame generator")
    parser.add_argument("--rate", type=float, default=100.0, help="Frames per second")
    parser.add_argument("--session-id", type=int, default=1)
    parser.add_argument("--timestamp-every", type=int, default=100,
                         help="Send a timestamp every Nth frame (D6 amortised timestamp). 0 disables.")
    parser.add_argument("--loss", type=float, default=0.0, help="Probability [0-1] a frame is dropped entirely")
    parser.add_argument("--dup", type=float, default=0.0, help="Probability [0-1] a frame is duplicated")
    parser.add_argument("--reorder", type=float, default=0.0, help="Probability [0-1] a frame is held back one slot")
    parser.add_argument("--corrupt", type=float, default=0.0, help="Probability [0-1] a frame gets one bit flipped")
    parser.add_argument("--seed", type=int, default=None, help="RNG seed, for reproducible fault injection")
    parser.add_argument("--count", type=int, default=0, help="Stop after N frames (0 = run forever)")
    parser.add_argument("--manifest-out", default="manifest.json", help="Path to write the ground-truth manifest JSON")
    args = parser.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    slave_name = os.ttyname(slave_fd)
    print(f"PTY ready. Point your reader at: {slave_name}", file=sys.stderr)
    print(f"Quick check: cat {slave_name} | xxd", file=sys.stderr)

    period = 1.0 / args.rate
    sequence = 0
    held_back = None  # a frame delayed by --reorder, sent next iteration
    last_checkpoint_time = time.monotonic()
    last_checkpoint_seq = 0

    stats = {"sent": 0, "dropped": 0, "duplicated": 0, "reordered": 0, "corrupted": 0}

    try:
        while args.count == 0 or sequence < args.count:
            samples = [random.randint(-32768, 32767) for _ in range(FIELD_COUNT)]
            want_ts = args.timestamp_every > 0 and sequence % args.timestamp_every == 0
            timestamp_ms = int(time.monotonic() * 1000) if want_ts else None
            first = sequence == 0

            wire = build_wire_frame(args.session_id, sequence, samples, timestamp_ms, first)
            manifest_entry = {
                "sequence": sequence,
                "samples": samples,
                "timestamp_ms": timestamp_ms,
                "first_of_session": first,
                "corrupted": False,
                "duplicated": False,
                "dropped": False,
                }

            if random.random() < args.corrupt:
                wire = corrupt_bit(wire)
                stats["corrupted"] += 1
                manifest_entry["corrupted"] = True
                
            to_send = [wire]

            if random.random() < args.dup:
                to_send.append(wire)
                stats["duplicated"] += 1
                manifest_entry["duplicated"] = True

            if random.random() < args.reorder and held_back is None:
                held_back = to_send
                to_send = []
                stats["reordered"] += 1
            elif held_back is not None:
                to_send = held_back + to_send
                held_back = None

            frame_actually_sent = False
            for frame_bytes in to_send:
                if random.random() < args.loss:
                    stats["dropped"] += 1
                    continue
                os.write(master_fd, frame_bytes)
                stats["sent"] += 1
                frame_actually_sent = True
            
            manifest_entry["dropped"] = not frame_actually_sent
            manifest.append(manifest_entry)

            sequence += 1
            if sequence % 500 == 0:
                now = time.monotonic()
                window_elapsed = now - last_checkpoint_time
                window_frames = sequence - last_checkpoint_seq
                actual_rate = window_frames / window_elapsed if window_elapsed > 0 else 0
                print(f"seq={sequence} window_rate={actual_rate:.1f}Hz stats={stats}", file=sys.stderr)
                last_checkpoint_time = now
                last_checkpoint_seq = sequence
            time.sleep(period)

    except KeyboardInterrupt:
        pass
    finally:
        with open(args.manifest_out, "w") as f:
            json.dump(manifest, f)
        print(f"Manifest written: {len(manifest)} entries", file=sys.stderr)
        print(f"\nFinal stats: {stats}", file=sys.stderr)
        os.close(master_fd)
        os.close(slave_fd)


if __name__ == "__main__":
    main()