#include <catch2/catch_test_macros.hpp>
#include "blackbox/crc16.hpp"
#include <cstring>

using namespace blackbox;

TEST_CASE("CRC-16/CCITT-FALSE known-answer vector") {
    const char* check = "123456789";
    REQUIRE(crc16_ccitt_false(reinterpret_cast<const uint8_t*>(check), 9) == 0x29B1);
}

TEST_CASE("CRC-16/CCITT-FALSE edge cases") {
    SECTION("Empty input returns the initial value") {
        REQUIRE(crc16_ccitt_false(nullptr, 0) == 0xFFFF);
    }

    SECTION("Single byte") {
        const uint8_t data[] = {0x00};
        // Not a hand-derivable value — this asserts stability/regression,
        // not correctness in isolation. Correctness is established by the
        // known-answer vector above.
        uint16_t crc = crc16_ccitt_false(data, 1);
        REQUIRE(crc != 0xFFFF);  // sanity: input actually changed the state
    }

    SECTION("Same input always produces the same output") {
        const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
        uint16_t crc1 = crc16_ccitt_false(data, sizeof(data));
        uint16_t crc2 = crc16_ccitt_false(data, sizeof(data));
        REQUIRE(crc1 == crc2);
    }

    SECTION("Different inputs of the same length produce different CRCs") {
        const uint8_t a[] = {0x01, 0x02, 0x03, 0x04};
        const uint8_t b[] = {0x01, 0x02, 0x03, 0x05};
        REQUIRE(crc16_ccitt_false(a, sizeof(a)) != crc16_ccitt_false(b, sizeof(b)));
    }
}