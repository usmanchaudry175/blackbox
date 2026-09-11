#pragma once

#include <cstdint>
#include <cstddef>

// CRC-16/CCITT-FALSE — poly 0x1021, init 0xFFFF, no reflection, no final XOR.
// Chosen over other "CRC-16-CCITT" variants specifically because it has no
// bit reflection, matching the big-endian, no-byte-swap-tricks philosophy
// used throughout the rest of this format (see design.md D11).
//
// Known-answer test vector: crc16_ccitt_false("123456789", 9) == 0x29B1

namespace blackbox {

uint16_t crc16_ccitt_false(const uint8_t* data, size_t len);

}  // namespace blackbox