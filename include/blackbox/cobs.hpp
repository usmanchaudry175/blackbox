#pragma once
#include <cstdint>
#include <cstddef>

namespace blackbox {

// Worst-case encoded size for n input bytes: one overhead byte, plus one more
// per 254 consecutive non-zero bytes. Excludes the trailing delimiter.
inline constexpr size_t cobs_max_encoded(size_t n) {
    return n + (n + 253) / 254 + (n == 0 ? 1 : 0);
}

// Encodes `in` into `out`. `out` must be at least cobs_max_encoded(in_len).
// Returns bytes written, or 0 if `out` is too small.
// Output contains no 0x00 bytes. Does not append the delimiter.
size_t cobs_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);

// Decodes `in` (delimiter already stripped) into `out`.
// Returns bytes written, or 0 on malformed input or insufficient output space.
size_t cobs_decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);

}  // namespace blackbox