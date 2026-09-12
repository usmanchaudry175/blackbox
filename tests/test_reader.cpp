#include <catch2/catch_test_macros.hpp>
#include "blackbox/reader.hpp"
#include "blackbox/frame.hpp"
#include <vector>
#include <cstring>

using namespace blackbox;

std::vector<uint8_t> make_wire_frame(uint32_t sequence, uint8_t session_id = 1,
                                      bool first_of_session = false) {
    Frame f{};
    f.version = kFormatVersion;
    f.session_id = session_id;
    f.sequence = sequence;
    for (size_t i = 0; i < kFieldCount; i++) {
        f.samples[i] = static_cast<int16_t>(sequence + i);
    }
    f.has_timestamp = false;
    f.first_of_session = first_of_session;

    std::vector<uint8_t> wire(kMaxWireFrame);
    size_t n = encode(f, wire.data(), wire.size());
    REQUIRE(n > 0);
    wire.resize(n);
    return wire;
}

TEST_CASE("Reader: single frame, single feed") {
    StreamReader reader;
    auto wire = make_wire_frame(1);
    reader.feed(wire.data(), wire.size());

    Frame out{};
    REQUIRE(reader.pop_frame(out));
    REQUIRE(out.sequence == 1);
    REQUIRE_FALSE(reader.pop_frame(out));  // nothing left

    REQUIRE(reader.stats().frames_accepted == 1);
    REQUIRE(reader.stats().crc_failures == 0);
}

TEST_CASE("Reader: multiple frames in one feed call") {
    StreamReader reader;
    std::vector<uint8_t> stream;
    for (uint32_t seq = 0; seq < 5; seq++) {
        auto wire = make_wire_frame(seq);
        stream.insert(stream.end(), wire.begin(), wire.end());
    }
    reader.feed(stream.data(), stream.size());

    REQUIRE(reader.stats().frames_accepted == 5);
    for (uint32_t seq = 0; seq < 5; seq++) {
        Frame out{};
        REQUIRE(reader.pop_frame(out));
        REQUIRE(out.sequence == seq);  // arrival order preserved
    }
}

TEST_CASE("Reader: byte-by-byte partial reads across feed() calls") {
    // Real reads never align with frame boundaries — this is the case
    // Phase 3's roadmap entry explicitly calls out.
    StreamReader reader;
    std::vector<uint8_t> stream;
    for (uint32_t seq = 0; seq < 3; seq++) {
        auto wire = make_wire_frame(seq);
        stream.insert(stream.end(), wire.begin(), wire.end());
    }

    for (uint8_t b : stream) {
        reader.feed(&b, 1);  // one byte per feed() call
    }

    REQUIRE(reader.stats().frames_accepted == 3);
    for (uint32_t seq = 0; seq < 3; seq++) {
        Frame out{};
        REQUIRE(reader.pop_frame(out));
        REQUIRE(out.sequence == seq);
    }
}

TEST_CASE("Reader: gap detection") {
    StreamReader reader;
    auto w0 = make_wire_frame(0);
    auto w5 = make_wire_frame(5);  // sequences 1-4 missing
    reader.feed(w0.data(), w0.size());
    reader.feed(w5.data(), w5.size());

    REQUIRE(reader.stats().frames_accepted == 2);
    REQUIRE(reader.stats().gaps_detected == 4);
}

TEST_CASE("Reader: duplicate detection") {
    StreamReader reader;
    auto w0 = make_wire_frame(0);
    auto w0_again = make_wire_frame(0);
    reader.feed(w0.data(), w0.size());
    reader.feed(w0_again.data(), w0_again.size());

    // Both frames still delivered — duplicates are flagged, not suppressed
    // (matches Phase 5's "never silently skip" philosophy).
    REQUIRE(reader.stats().frames_accepted == 2);
    REQUIRE(reader.stats().duplicates_detected == 1);

    Frame out{};
    REQUIRE(reader.pop_frame(out));
    REQUIRE(reader.pop_frame(out));
    REQUIRE_FALSE(reader.pop_frame(out));
}

TEST_CASE("Reader: mid-stream corruption recovers at next frame") {
    StreamReader reader;
    auto w0 = make_wire_frame(0);
    auto w1 = make_wire_frame(1);
    auto w2 = make_wire_frame(2);

    // Corrupt a payload byte inside w1's COBS-encoded bytes. This must not
    // introduce a stray 0x00 (which would just look like an early, extra
    // delimiter rather than exercising CRC/corruption rejection) — flip a
    // high bit instead, which COBS's structure guarantees won't touch the
    // group-length code bytes at typical small offsets like this.
    w1[2] ^= 0x80;

    std::vector<uint8_t> stream;
    stream.insert(stream.end(), w0.begin(), w0.end());
    stream.insert(stream.end(), w1.begin(), w1.end());
    stream.insert(stream.end(), w2.begin(), w2.end());
    reader.feed(stream.data(), stream.size());

    // w0 and w2 survive; w1 is rejected (bad CRC, most likely) but doesn't
    // take w2 down with it — proving delimiter-based resync works.
    REQUIRE(reader.stats().frames_accepted == 2);
    REQUIRE(reader.stats().crc_failures >= 1);

    Frame out{};
    REQUIRE(reader.pop_frame(out));
    REQUIRE(out.sequence == 0);
    REQUIRE(reader.pop_frame(out));
    REQUIRE(out.sequence == 2);
}

TEST_CASE("Reader: oversized stream with no delimiter is discarded (D13)") {
    StreamReader reader;
    // Feed far more non-zero bytes than kMaxWireFrame - 1 without ever
    // sending a delimiter — this is the unbounded-buffer-growth path D13
    // explicitly calls a denial-of-service bug if unguarded.
    std::vector<uint8_t> junk(kMaxWireFrame * 3, 0xAA);
    reader.feed(junk.data(), junk.size());

    REQUIRE(reader.stats().oversized_discards >= 1);
    REQUIRE(reader.stats().frames_accepted == 0);

    // Confirm the reader actually recovered, not just discarded once and
    // gotten stuck — a real frame sent afterward should still work.
    auto w = make_wire_frame(42);
    reader.feed(w.data(), w.size());
    Frame out{};
    REQUIRE(reader.pop_frame(out));
    REQUIRE(out.sequence == 42);
}

TEST_CASE("Reader: empty feed is a no-op") {
    StreamReader reader;
    reader.feed(nullptr, 0);
    REQUIRE(reader.stats().bytes_fed == 0);
    REQUIRE(reader.stats().frames_accepted == 0);
}