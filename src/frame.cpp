#include "blackbox/frame.hpp"
#include "blackbox/cobs.hpp"
#include "blackbox/crc16.hpp"

namespace blackbox {

size_t encode(const Frame& frame, uint8_t* out, size_t out_len) {
    uint8_t raw[kMaxRawFrame];

    uint8_t flags = static_cast<uint8_t>((frame.version << kVersionShift) & kVersionMask);
    if (frame.has_timestamp)    flags |= kFlagTimestamp;
    if (frame.first_of_session) flags |= kFlagFirstOfSession;
    // Reserved bits (2-4) are left at 0 by construction — never set here (D10).

    raw[kOffFlags]   = flags;
    raw[kOffSession] = frame.session_id;

    raw[kOffSequence + 0] = static_cast<uint8_t>(frame.sequence >> 24);
    raw[kOffSequence + 1] = static_cast<uint8_t>(frame.sequence >> 16);
    raw[kOffSequence + 2] = static_cast<uint8_t>(frame.sequence >> 8);
    raw[kOffSequence + 3] = static_cast<uint8_t>(frame.sequence);

    for (size_t i = 0; i < kFieldCount; i++) {
        uint16_t v = static_cast<uint16_t>(frame.samples[i]);
        raw[kOffPayload + i * 2 + 0] = static_cast<uint8_t>(v >> 8);
        raw[kOffPayload + i * 2 + 1] = static_cast<uint8_t>(v);
    }

    size_t crc_off = kOffPayload + kPayloadSize;   // == kOffTimestamp
    if (frame.has_timestamp) {
        uint32_t ts = frame.timestamp_ms;
        raw[kOffTimestamp + 0] = static_cast<uint8_t>(ts >> 24);
        raw[kOffTimestamp + 1] = static_cast<uint8_t>(ts >> 16);
        raw[kOffTimestamp + 2] = static_cast<uint8_t>(ts >> 8);
        raw[kOffTimestamp + 3] = static_cast<uint8_t>(ts);
        crc_off = kOffTimestamp + kTimestampSize;
    }

    uint16_t crc = crc16_ccitt_false(raw, crc_off);
    raw[crc_off + 0] = static_cast<uint8_t>(crc >> 8);
    raw[crc_off + 1] = static_cast<uint8_t>(crc);

    size_t raw_len = crc_off + kCrcSize;

    // Reserve room for the trailing delimiter on top of the COBS bound.
    if (out_len < cobs_max_encoded(raw_len) + 1) {
        return 0;
    }

    size_t enc_len = cobs_encode(raw, raw_len, out, out_len);
    if (enc_len == 0) {
        return 0;
    }

    out[enc_len] = kDelimiter;
    return enc_len + 1;
}

DecodeError decode(const uint8_t* in, size_t in_len, Frame& frame) {
    // `in` is already COBS-decoded, delimiter stripped — this function
    // only parses and validates the raw frame layout (D12/D14).

    if (in_len < kMinRawFrame) {
        return DecodeError::TooShort;
    }

    uint8_t flags = in[kOffFlags];
    bool has_ts = (flags & kFlagTimestamp) != 0;
    size_t expected_len = has_ts ? kMaxRawFrame : kMinRawFrame;

    if (in_len != expected_len) {
        return DecodeError::LengthMismatch;
    }

    // CRC position comes from the actual decoded length, never from the
    // flags-implied length (D14) — here they coincide only because the
    // mismatch check above already passed, not because we trust flags.
    size_t crc_off = in_len - kCrcSize;

    uint16_t computed_crc = crc16_ccitt_false(in, crc_off);
    uint16_t received_crc = (static_cast<uint16_t>(in[crc_off]) << 8) | in[crc_off + 1];
    if (computed_crc != received_crc) {
        return DecodeError::BadCrc;
    }

    // Version is only trusted once the CRC has validated the whole frame.
    uint8_t version = static_cast<uint8_t>((flags & kVersionMask) >> kVersionShift);
    if (version != kFormatVersion) {
        return DecodeError::UnsupportedVersion;
    }

    frame.version          = version;
    frame.first_of_session = (flags & kFlagFirstOfSession) != 0;
    frame.has_timestamp    = has_ts;
    frame.session_id       = in[kOffSession];

    frame.sequence = (static_cast<uint32_t>(in[kOffSequence + 0]) << 24) |
                     (static_cast<uint32_t>(in[kOffSequence + 1]) << 16) |
                     (static_cast<uint32_t>(in[kOffSequence + 2]) << 8)  |
                      static_cast<uint32_t>(in[kOffSequence + 3]);

    for (size_t i = 0; i < kFieldCount; i++) {
        uint16_t v = (static_cast<uint16_t>(in[kOffPayload + i * 2]) << 8) |
                      static_cast<uint16_t>(in[kOffPayload + i * 2 + 1]);
        frame.samples[i] = static_cast<int16_t>(v);
    }

    frame.timestamp_ms = 0;
    if (has_ts) {
        frame.timestamp_ms = (static_cast<uint32_t>(in[kOffTimestamp + 0]) << 24) |
                              (static_cast<uint32_t>(in[kOffTimestamp + 1]) << 16) |
                              (static_cast<uint32_t>(in[kOffTimestamp + 2]) << 8)  |
                               static_cast<uint32_t>(in[kOffTimestamp + 3]);
    }

    return DecodeError::Ok;
}

}  // namespace blackbox