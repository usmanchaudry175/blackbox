#include "blackbox/cobs.hpp"
#include "blackbox/frame.hpp"
#include <cstdint>
#include <cstddef>

// libFuzzer entry point. `data`/`size` is an arbitrary, adversarial byte
// buffer that libFuzzer mutates using code-coverage feedback across runs.
//
// This fuzzes both layers together — cobs_decode() then decode() — because
// that's the real attack surface: arbitrary bytes arriving off a serial
// link, not a pre-validated frame. Correctness here means: never crash,
// never read/write out of bounds, regardless of input. The actual
// DecodeError result is not checked — rejecting malformed input is
// success, not failure; only a crash or a sanitizer-caught memory error
// counts as a finding.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    uint8_t raw[blackbox::kMaxRawFrame];
    size_t raw_len = blackbox::cobs_decode(data, size, raw, sizeof(raw));

    if (raw_len > 0) {
        blackbox::Frame frame{};
        blackbox::decode(raw, raw_len, frame);
    }

    return 0;  // required return value — always 0, per libFuzzer's API
}