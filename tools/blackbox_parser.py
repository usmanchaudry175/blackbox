"""
Python decode-side implementation of the blackbox wire format, for
cross-language benchmarking against the C++ pipeline (Phase 7).

Encode-side logic (cobs_encode, crc16_ccitt_false, build_wire_frame)
already exists in generate_frames.py and is reused here rather than
duplicated. This module adds the decode side, which previously had no
Python implementation — verify_replay.py deliberately avoids
reimplementing decode by trusting replay_cli's output instead.

Wire format matches include/blackbox/frame.hpp exactly. See
docs/design.md D1-D14 for rationale.
"""

import struct

from generate_frames import (
    FLAG_TIMESTAMP,
    FLAG_FIRST_OF_SESSION,
    VERSION_SHIFT,
    FORMAT_VERSION,
    FIELD_COUNT,
    crc16_ccitt_false,
)


class DecodeError(Exception):
    pass


def cobs_decode(data: bytes) -> bytes:
    """Standard COBS decode. Must byte-for-byte match src/cobs.cpp's
    cobs_decode() for the same input."""
    out = bytearray()
    idx = 0
    n = len(data)
    while idx < n:
        code = data[idx]
        if code == 0:
            raise DecodeError("zero byte found where a length code was expected")
        idx += 1
        chunk_len = code - 1
        if idx + chunk_len > n:
            raise DecodeError("length code points past end of buffer")
        out.extend(data[idx:idx + chunk_len])
        idx += chunk_len
        if code != 0xFF and idx < n:
            out.append(0)
    return bytes(out)


def decode_raw_frame(raw: bytes) -> dict:
    """Decodes a raw (post-COBS, pre-delimiter-stripped) frame per the
    D12 field ordering, verifying CRC. Raises DecodeError on any
    corruption (bad length, bad CRC, unsupported version) — matching
    decode()'s defensive checks in src/frame.cpp."""

    # Two legal lengths: base frame, or base + 4-byte timestamp (D7).
    kMinRawFrame = 1 + 1 + 4 + (2 * FIELD_COUNT) + 2   # flags+session+seq+payload+crc
    kMaxRawFrame = kMinRawFrame + 4                     # + timestamp

    if len(raw) not in (kMinRawFrame, kMaxRawFrame):
        raise DecodeError(f"unexpected raw length {len(raw)}, "
                           f"expected {kMinRawFrame} or {kMaxRawFrame}")

    body = raw[:-2]
    crc_received = struct.unpack(">H", raw[-2:])[0]
    crc_computed = crc16_ccitt_false(body)
    if crc_received != crc_computed:
        raise DecodeError(f"CRC mismatch: got {crc_received:#06x}, expected {crc_computed:#06x}")

    flags, session_id, sequence = struct.unpack(">BBI", raw[0:6])

    version = (flags >> VERSION_SHIFT) & 0x07
    if version != FORMAT_VERSION:
        raise DecodeError(f"unsupported format version {version}")

    has_timestamp = bool(flags & FLAG_TIMESTAMP)
    first_of_session = bool(flags & FLAG_FIRST_OF_SESSION)

    expected_len = kMaxRawFrame if has_timestamp else kMinRawFrame
    if len(raw) != expected_len:
        raise DecodeError(f"flags claim {'timestamp' if has_timestamp else 'no timestamp'} "
                           f"but raw length {len(raw)} doesn't match ({expected_len} expected)")

    offset = 6
    samples = list(struct.unpack(">" + "h" * FIELD_COUNT, raw[offset:offset + 2 * FIELD_COUNT]))
    offset += 2 * FIELD_COUNT

    timestamp_ms = None
    if has_timestamp:
        timestamp_ms = struct.unpack(">I", raw[offset:offset + 4])[0]
        offset += 4

    return {
        "session_id": session_id,
        "sequence": sequence,
        "samples": samples,
        "timestamp_ms": timestamp_ms,
        "first_of_session": first_of_session,
    }


def decode_wire_frame(wire: bytes) -> dict:
    """Decodes a full wire-format frame: strips the trailing delimiter,
    COBS-decodes, then decodes the raw frame. wire must NOT include
    bytes from adjacent frames — caller is responsible for splitting
    on the 0x00 delimiter first (this mirrors StreamReader::feed()'s
    job, not duplicated here)."""
    if not wire or wire[-1] != 0x00:
        raise DecodeError("wire frame missing trailing delimiter")
    cobs_encoded = wire[:-1]
    raw = cobs_decode(cobs_encoded)
    return decode_raw_frame(raw)