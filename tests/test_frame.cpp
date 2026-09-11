#include <catch2/catch_test_macros.hpp>
#include "blackbox/frame.hpp"
#include "blackbox/cobs.hpp"
#include "blackbox/crc16.hpp"
#include <vector>
#include <cstring>

using namespace blackbox;

// Runs a Frame through the real encode() path, then strips the trailing
// delimiter and COBS-decodes it back to raw bytes — giving direct access
// to the wire-format bytes for endianness/corruption tests, without
// duplicating frame.cpp's serialisation logic by hand.
std::vector<uint8_t> encode_to_raw(const Frame& frame) {
    uint8_t encoded[kMaxWireFrame];
    size_t enc_len = encode(frame, encoded, sizeof(encoded));
    REQUIRE(enc_len > 0);
    REQUIRE(encoded[enc_len - 1] == kDelimiter);

    std::vector<uint8_t> raw(kMaxRawFrame);
    size_t raw_len = cobs_decode(encoded, enc_len - 1, raw.data(), raw.size());
    REQUIRE(raw_len > 0);
    raw.resize(raw_len);
    return raw;
}

TEST_CASE("Frame round-trip: no timestamp") {
    Frame in{};
    in.version = kFormatVersion;
    in.session_id = 7;
    in.sequence = 123456;
    in.samples[kAccelX] = 100;
    in.samples[kAccelY] = -200;
    in.samples[kAccelZ] = 16384;
    in.samples[kGyroX]  = -32768;
    in.samples[kGyroY]  = 32767;
    in.samples[kGyroZ]  = 0;
    in.has_timestamp = false;
    in.first_of_session = false;

    uint8_t wire[kMaxWireFrame];
    size_t wire_len = encode(in, wire, sizeof(wire));
    REQUIRE(wire_len > 0);

    std::vector<uint8_t> raw(kMinRawFrame);
    size_t raw_len = cobs_decode(wire, wire_len - 1, raw.data(), raw.size());
    REQUIRE(raw_len == kMinRawFrame);

    Frame out{};
    DecodeError err = decode(raw.data(), raw_len, out);

    REQUIRE(err == DecodeError::Ok);
    REQUIRE(out.version == in.version);
    REQUIRE(out.session_id == in.session_id);
    REQUIRE(out.sequence == in.sequence);
    REQUIRE(out.has_timestamp == false);
    REQUIRE(out.first_of_session == false);
    for (size_t i = 0; i < kFieldCount; i++) {
        REQUIRE(out.samples[i] == in.samples[i]);
    }
}

TEST_CASE("Frame round-trip: with timestamp and first_of_session") {
    Frame in{};
    in.version = kFormatVersion;
    in.session_id = 200;
    in.sequence = 0xDEADBEEF;
    in.samples[kAccelX] = 1;
    in.samples[kAccelY] = 2;
    in.samples[kAccelZ] = 3;
    in.samples[kGyroX]  = -1;
    in.samples[kGyroY]  = -2;
    in.samples[kGyroZ]  = -3;
    in.has_timestamp = true;
    in.timestamp_ms = 0x12345678;
    in.first_of_session = true;

    uint8_t wire[kMaxWireFrame];
    size_t wire_len = encode(in, wire, sizeof(wire));
    REQUIRE(wire_len > 0);

    std::vector<uint8_t> raw(kMaxRawFrame);
    size_t raw_len = cobs_decode(wire, wire_len - 1, raw.data(), raw.size());
    REQUIRE(raw_len == kMaxRawFrame);

    Frame out{};
    DecodeError err = decode(raw.data(), raw_len, out);

    REQUIRE(err == DecodeError::Ok);
    REQUIRE(out.session_id == in.session_id);
    REQUIRE(out.sequence == in.sequence);
    REQUIRE(out.has_timestamp == true);
    REQUIRE(out.timestamp_ms == in.timestamp_ms);
    REQUIRE(out.first_of_session == true);
    for (size_t i = 0; i < kFieldCount; i++) {
        REQUIRE(out.samples[i] == in.samples[i]);
    }
}

TEST_CASE("Frame wire layout is big-endian (D11)") {
    Frame in{};
    in.version = kFormatVersion;
    in.session_id = 0x42;
    in.sequence = 0x11223344;
    in.samples[kAccelX] = 0x0102;  // 258, distinguishable byte order
    for (size_t i = 1; i < kFieldCount; i++) in.samples[i] = 0;
    in.has_timestamp = false;
    in.first_of_session = false;

    std::vector<uint8_t> raw = encode_to_raw(in);

    // Sequence: big-endian, high byte first
    REQUIRE(raw[kOffSequence + 0] == 0x11);
    REQUIRE(raw[kOffSequence + 1] == 0x22);
    REQUIRE(raw[kOffSequence + 2] == 0x33);
    REQUIRE(raw[kOffSequence + 3] == 0x44);

    // First payload field (accel X): 0x0102 -> high byte first
    REQUIRE(raw[kOffPayload + 0] == 0x01);
    REQUIRE(raw[kOffPayload + 1] == 0x02);
}

TEST_CASE("Frame decode: reserved bits are ignored (D10 forward-compat)") {
    Frame in{};
    in.version = kFormatVersion;
    in.session_id = 1;
    in.sequence = 1;
    for (size_t i = 0; i < kFieldCount; i++) in.samples[i] = 0;
    in.has_timestamp = false;
    in.first_of_session = false;

    std::vector<uint8_t> raw = encode_to_raw(in);

    // Set all reserved bits (2-4) to 1, then recompute CRC over the
    // modified bytes so the frame is still internally consistent.
    raw[kOffFlags] |= kReservedMask;
    uint16_t crc = crc16_ccitt_false(raw.data(), raw.size() - kCrcSize);
    raw[raw.size() - 2] = static_cast<uint8_t>(crc >> 8);
    raw[raw.size() - 1] = static_cast<uint8_t>(crc);

    Frame out{};
    DecodeError err = decode(raw.data(), raw.size(), out);
    REQUIRE(err == DecodeError::Ok);
    REQUIRE(out.version == kFormatVersion);
}

TEST_CASE("Frame decode: corruption and malformed input rejection") {
    Frame valid{};
    valid.version = kFormatVersion;
    valid.session_id = 5;
    valid.sequence = 999;
    for (size_t i = 0; i < kFieldCount; i++) valid.samples[i] = static_cast<int16_t>(i);
    valid.has_timestamp = false;
    valid.first_of_session = false;

    SECTION("Too short — fewer bytes than the minimum frame") {
        std::vector<uint8_t> raw = encode_to_raw(valid);
        Frame out{};
        DecodeError err = decode(raw.data(), kMinRawFrame - 1, out);
        REQUIRE(err == DecodeError::TooShort);
    }

    SECTION("Length mismatch — flags claim no timestamp, but length doesn't match either legal size") {
        std::vector<uint8_t> raw = encode_to_raw(valid);
        raw.push_back(0xAA);  // pad by one byte — no longer kMinRawFrame or kMaxRawFrame
        Frame out{};
        DecodeError err = decode(raw.data(), raw.size(), out);
        REQUIRE(err == DecodeError::LengthMismatch);
    }

    SECTION("Bad CRC — payload corrupted, CRC left stale") {
        std::vector<uint8_t> raw = encode_to_raw(valid);
        raw[kOffPayload] ^= 0xFF;  // flip a payload byte, don't recompute CRC
        Frame out{};
        DecodeError err = decode(raw.data(), raw.size(), out);
        REQUIRE(err == DecodeError::BadCrc);
    }

    SECTION("Unsupported version — flags claim a version this decoder doesn't support") {
        std::vector<uint8_t> raw = encode_to_raw(valid);

        // Clear version bits, set to 7 (never issued as a real version — reserved as
        // an escape hatch per design.md D10), then recompute CRC so the frame is
        // otherwise internally consistent (so we know it's the version check firing,
        // not a coincidental CRC failure).
        raw[kOffFlags] = static_cast<uint8_t>((raw[kOffFlags] & ~kVersionMask) | (0b111u << kVersionShift));
        uint16_t crc = crc16_ccitt_false(raw.data(), raw.size() - kCrcSize);
        raw[raw.size() - 2] = static_cast<uint8_t>(crc >> 8);
        raw[raw.size() - 1] = static_cast<uint8_t>(crc);

        Frame out{};
        DecodeError err = decode(raw.data(), raw.size(), out);
        REQUIRE(err == DecodeError::UnsupportedVersion);
    }
}

TEST_CASE("Frame encode: undersized output buffer fails safely") {
    Frame in{};
    in.version = kFormatVersion;
    in.session_id = 1;
    in.sequence = 1;
    for (size_t i = 0; i < kFieldCount; i++) in.samples[i] = 0;
    in.has_timestamp = false;
    in.first_of_session = false;

    uint8_t tiny_out[2];
    size_t n = encode(in, tiny_out, sizeof(tiny_out));
    REQUIRE(n == 0);
}