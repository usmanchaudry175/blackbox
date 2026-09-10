#pragma once

#include <cstdint>
#include <cstddef>

// Wire format v1 — see docs/design.md
// Big-endian, COBS-framed, CRC-16-CCITT.

namespace blackbox {

// ---- Field sizes (D13) ----
inline constexpr size_t kFlagsSize     = 1;
inline constexpr size_t kSessionSize   = 1;
inline constexpr size_t kSequenceSize  = 4;
inline constexpr size_t kPayloadSize   = 14;
inline constexpr size_t kTimestampSize = 4;
inline constexpr size_t kCrcSize       = 2;

// ---- Derived sizes — arithmetic, never literals ----
inline constexpr size_t kHeaderSize   = kFlagsSize + kSessionSize + kSequenceSize;  // flags + session + sequence
inline constexpr size_t kMinRawFrame  = kHeaderSize + kPayloadSize + kCrcSize;  // header + payload + crc
inline constexpr size_t kMaxRawFrame  = kMinRawFrame + kTimestampSize;  // header + payload + timestamp + crc

inline constexpr size_t kCobsMax      = kMaxRawFrame + (kMaxRawFrame + 253) / 254;
inline constexpr size_t kMaxWireFrame = kCobsMax + 1;  // + delimiter

static_assert(kMinRawFrame == 20);
static_assert(kMaxWireFrame == 26);

// ---- Offsets (D12) ----
inline constexpr size_t kOffFlags     = 0;
inline constexpr size_t kOffSession   = kOffFlags + kFlagsSize;
inline constexpr size_t kOffSequence  = kOffSession + kSessionSize;
inline constexpr size_t kOffPayload   = kOffSequence + kSequenceSize;
inline constexpr size_t kOffTimestamp = kOffPayload + kPayloadSize;
// CRC is located from the decoded length, not from here (D14)

// ---- Flags byte (D10) ----
inline constexpr uint8_t kFlagTimestamp    = 1u << 0;  // bit 0
inline constexpr uint8_t kFlagFirstOfSession = 1u << 1;  // bit 1
inline constexpr uint8_t kVersionMask  = 0b111u << kVersionShift;  // bits 5-7
inline constexpr uint8_t kVersionShift = 5;
inline constexpr uint8_t kFormatVersion = 1;

static_assert((kVersionMask & (kFlagTimestamp | kFlagFirstOfSession)) == 0);

inline constexpr uint8_t kReservedMask = 0b111u << 2;  // bits 2-4

inline constexpr uint8_t kDelimiter = 0x00;

inline constexpr size_t kAxisCount = 6;   // ax ay az gx gy gz

// ---- Decoded representation ----
// NOT the wire format. Natural types; padding is irrelevant because
// nothing memcpys this anywhere.
struct Frame {
    uint8_t  version;
    uint8_t  session_id;
    uint32_t sequence;
    int16_t  samples[kAxisCount];
    uint32_t timestamp_ms;      // valid only if has_timestamp
    bool     has_timestamp;
    bool     first_of_session;
};

// ---- Result type ----
// A bool loses the failure reason; Phase 3 counts error types separately.
enum class DecodeError {
    Ok,
    TooShort,
    UnsupportedVersion,
    LengthMismatch,
    BadCrc,
};
enum Axis : size_t {
    kAccelX = 0, kAccelY, kAccelZ,
    kTemp,
    kGyroX, kGyroY, kGyroZ,
    kFieldCount
};

int16_t samples[kFieldCount];
static_assert(kPayloadSize == kFieldCount * sizeof(int16_t));

// ---- API ----

// Encodes `frame` into `out`. Returns bytes written, or 0 if `out` is too small.
// Caller supplies the buffer; this function allocates nothing.
size_t encode(const Frame& frame, uint8_t* out, size_t out_len);

// Decodes one COBS-decoded frame from `in` into `frame`.
DecodeError decode(const uint8_t* in, size_t in_len, Frame& frame);

}  // namespace blackbox