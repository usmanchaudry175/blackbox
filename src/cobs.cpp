#include "blackbox/cobs.hpp"

namespace blackbox {

size_t cobs_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
    if (out_len < 1 || out_len < cobs_max_encoded(in_len)) {
        return 0;
    }

    size_t read_idx = 0;
    size_t write_idx = 1;
    size_t code_idx = 0;
    uint8_t code = 1;

    while (read_idx < in_len) {
        if (in[read_idx] == 0x00) {
            out[code_idx] = code;
            code_idx = write_idx++;
            code = 1;
        } else {
            out[write_idx++] = in[read_idx];
            code++;
            // Split at 254 data bytes, but only if more data follows —
            // otherwise we would emit a trailing empty group.
            if (code == 0xFF && (read_idx + 1) < in_len) {
                out[code_idx] = code;
                code_idx = write_idx++;
                code = 1;
            }
        }
        read_idx++;
    }

    out[code_idx] = code;
    return write_idx;
}

size_t cobs_decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
    size_t read_idx = 0;
    size_t write_idx = 0;

    while (read_idx < in_len) {
        uint8_t code = in[read_idx++];

        // A zero code byte is malformed — encoded output never contains 0x00.
        if (code == 0x00) {
            return 0;
        }

        // Ordered so nothing underflows: code >= 1 and read_idx <= in_len.
        if (static_cast<size_t>(code) - 1 > in_len - read_idx) {
            return 0;
        }
        

        for (uint8_t i = 1; i < code; i++) {
            if (write_idx >= out_len) {
                return 0;
            }
            out[write_idx++] = in[read_idx++];
        }
    

        // A group shorter than 0xFF was terminated by a zero in the original,
        // unless we have reached the end of the input.
        if (code < 0xFF && read_idx < in_len) {
            if (write_idx >= out_len) {
                return 0;
            }
            out[write_idx++] = 0x00;
        }
    }

    return write_idx;
}

}  // namespace blackbox