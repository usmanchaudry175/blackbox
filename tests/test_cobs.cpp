#include <catch2/catch_test_macros.hpp>
#include "blackbox/cobs.hpp"
#include <vector>
#include <cstring>

using namespace blackbox;
TEST_CASE("known vector") {
    const uint8_t in[] = {0x11, 0x00, 0x22, 0x33};
    uint8_t out[16] = {};

    size_t n = cobs_encode(in, sizeof(in), out, sizeof(out));

    // Structural verification of the COBS output frame
    REQUIRE(n == 5);
    REQUIRE(out[0] == 0x02); // Distance to first 0x00 (2 bytes ahead)
    REQUIRE(out[1] == 0x11); // Original non-zero data
    REQUIRE(out[2] == 0x03); // Distance to the end of the frame (3 bytes ahead)
    REQUIRE(out[3] == 0x22); // Original non-zero data
    REQUIRE(out[4] == 0x33); // Original non-zero data

    // Verify it rounds-trip cleanly back to the identical source vector
    uint8_t decoded[16] = {};
    size_t decoded_len = cobs_decode(out, n, decoded, sizeof(decoded));
    
    REQUIRE(decoded_len == sizeof(in));
    REQUIRE(std::memcmp(in, decoded, sizeof(in)) == 0);
}

// Helper to perform a round-trip check and validate safety constraints
void verify_round_trip(const std::vector<uint8_t>& raw) {
    size_t max_len = cobs_max_encoded(raw.size());
    std::vector<uint8_t> encoded(max_len);
    
    // 1. Encode
    size_t encoded_len = cobs_encode(raw.data(), raw.size(), encoded.data(), encoded.size());
    
    // Ensure encoded length never exceeds the worst-case formula bound
    REQUIRE(encoded_len <= max_len);
    REQUIRE(encoded_len > 0);
    
    // Verify no 0x00 bytes exist inside the encoded payload
    for (size_t i = 0; i < encoded_len; ++i) {
        REQUIRE(encoded[i] != 0x00);
    }
    
    // 2. Decode
    std::vector<uint8_t> decoded(raw.size() == 0 ? 1 : raw.size());
    size_t decoded_len = cobs_decode(encoded.data(), encoded_len, decoded.data(), decoded.size());
    
    REQUIRE(decoded_len == raw.size());
    
    // 3. Structural verification
    if (raw.size() > 0) {
        REQUIRE(std::memcmp(raw.data(), decoded.data(), raw.size()) == 0);
    }
}

TEST_CASE("COBS Round-Trip: Empty, Zeros, No-Zeros, Single Bytes") {
    SECTION("Empty payload") {
        verify_round_trip({});
    }

    SECTION("All zeros payload") {
        verify_round_trip({0x00, 0x00, 0x00, 0x00});
    }

    SECTION("No zeros payload") {
        verify_round_trip({0x11, 0x22, 0x33, 0x44});
    }

    SECTION("Single byte payloads") {
        verify_round_trip({0x00});
        verify_round_trip({0xFF});
    }
}

TEST_CASE("COBS Round-Trip: Group Length Boundaries") {
    // 253 non-zero bytes
    SECTION("253 non-zero bytes") {
        std::vector<uint8_t> raw(253, 0xAA);
        verify_round_trip(raw);
        
        std::vector<uint8_t> enc(cobs_max_encoded(raw.size()));
        size_t enc_len = cobs_encode(raw.data(), raw.size(), enc.data(), enc.size());
        // 1 code byte + 253 data bytes = 254 bytes total
        REQUIRE(enc_len == 254); 
    }

    // 254 non-zero bytes (Exact max overhead split block)
    SECTION("254 non-zero bytes") {
        std::vector<uint8_t> raw(254, 0xAA);
        verify_round_trip(raw);
        
        std::vector<uint8_t> enc(cobs_max_encoded(raw.size()));
        size_t enc_len = cobs_encode(raw.data(), raw.size(), enc.data(), enc.size());
        // 1 code byte (0xFF) + 254 data bytes = 255 bytes total
        REQUIRE(enc_len == cobs_max_encoded(raw.size()));
        REQUIRE(enc_len == 255);
        REQUIRE(enc[0] == 0xFF);
    }

    // 255 non-zero bytes (Triggers a split group)
    SECTION("255 non-zero bytes") {
        std::vector<uint8_t> raw(255, 0xAA);
        verify_round_trip(raw);
        
        std::vector<uint8_t> enc(cobs_max_encoded(raw.size()));
        size_t enc_len = cobs_encode(raw.data(), raw.size(), enc.data(), enc.size());
        // 1 code byte (0xFF) + 254 data bytes + 1 code byte (0x02) + 1 data byte = 257 bytes total
        REQUIRE(enc_len == cobs_max_encoded(raw.size()));
        REQUIRE(enc_len == 257);
        REQUIRE(enc[0] == 0xFF);
        REQUIRE(enc[255] == 0x02);
    }
}

TEST_CASE("COBS Error Rejection") {
    SECTION("Leading 0x00 inside encoded payload") {
        const uint8_t bad_enc[] = {0x00, 0x11, 0x22};
        uint8_t out[8] = {};
        size_t n = cobs_decode(bad_enc, sizeof(bad_enc), out, sizeof(out));
        REQUIRE(n == 0); // Must safely reject malformed zero code
    }

    SECTION("Code pointing past the end of the input frame") {
        // Frame claims the block is 5 bytes long, but buffer only has 2 bytes total
        const uint8_t bad_enc[] = {0x05, 0x11}; 
        uint8_t out[8] = {};
        size_t n = cobs_decode(bad_enc, sizeof(bad_enc), out, sizeof(out));
        REQUIRE(n == 0); // Must safely prevent out-of-bounds frame parse
    }

    SECTION("Undersized output buffer on encoding") {
        const uint8_t raw[] = {0x11, 0x22, 0x33};
        uint8_t small_out[2] = {}; // cobs_max_encoded(3) needs 4 bytes
        size_t n = cobs_encode(raw, sizeof(raw), small_out, sizeof(small_out));
        REQUIRE(n == 0); // Safeguarded entry failure
    }

    SECTION("Undersized output buffer on decoding") {
        const uint8_t enc[] = {0x04, 0x11, 0x22, 0x33}; // Decodes to 3 bytes
        uint8_t small_out[2] = {};
        size_t n = cobs_decode(enc, sizeof(enc), small_out, sizeof(small_out));
        REQUIRE(n == 0); // Safely aborted due to small target space
    }
}
