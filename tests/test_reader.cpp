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
// Additions to tests/test_reader.cpp — StreamReader out-of-order handling

TEST_CASE("Reader: out-of-order arrival is flagged, not reassembled") {
    StreamReader reader;
    auto w0 = make_wire_frame(0);
    auto w2 = make_wire_frame(2);
    auto w1 = make_wire_frame(1);  // arrives AFTER seq 2 — out of order

    reader.feed(w0.data(), w0.size());
    reader.feed(w2.data(), w2.size());
    reader.feed(w1.data(), w1.size());

    // seq 0, then seq 2 (a gap of exactly seq 1, since it hasn't arrived
    // yet), then seq 1 arrives late — flagged as out-of-order, not
    // treated as filling the gap.
    REQUIRE(reader.stats().frames_accepted == 3);  // all three still delivered
    REQUIRE(reader.stats().gaps_detected == 1);     // the gap was real at the time seq 2 arrived
    REQUIRE(reader.stats().out_of_order_detected == 1);

    Frame out{};
    REQUIRE(reader.pop_frame(out)); REQUIRE(out.sequence == 0);
    REQUIRE(reader.pop_frame(out)); REQUIRE(out.sequence == 2);
    REQUIRE(reader.pop_frame(out)); REQUIRE(out.sequence == 1);  // delivered in ARRIVAL order, not re-sorted
}

TEST_CASE("Reader: out-of-order frame does not corrupt subsequent gap detection") {
    // This is the exact regression D17 exists to prevent: without the
    // high-water-mark fix, last_sequence_ would regress to 1 after the
    // out-of-order frame, making the next real frame (seq 5) look like
    // it closed a gap starting from 2 instead of from 3 (the true
    // high-water mark before this frame arrived).
    StreamReader reader;
    auto w0 = make_wire_frame(0);
    auto w3 = make_wire_frame(3);   // gap: 1,2 missing (2 missing)
    auto w1 = make_wire_frame(1);   // out of order — must NOT reset high-water mark
    auto w5 = make_wire_frame(5);   // gap: 4 missing (1 missing) — must be measured from 3, not from 1

    reader.feed(w0.data(), w0.size());
    reader.feed(w3.data(), w3.size());
    reader.feed(w1.data(), w1.size());
    reader.feed(w5.data(), w5.size());

    REQUIRE(reader.stats().out_of_order_detected == 1);
    // Correct total: gap of {1,2} (2 missing) when seq 3 arrived, plus
    // gap of {4} (1 missing) when seq 5 arrived = 3 total.
    // A buggy implementation that let last_sequence_ regress to 1 would
    // instead report a gap of {2,3,4} (3 missing) when seq 5 arrived,
    // for the WRONG total of 2 + 3 = 5.
    REQUIRE(reader.stats().gaps_detected == 3);
}

TEST_CASE("Reader: out-of-order frame with a lower sequence than a duplicate check") {
    // Confirms the duplicate check still fires correctly and takes
    // priority — an out-of-order frame whose sequence exactly matches
    // one already seen is a duplicate, not "out of order".
    StreamReader reader;
    auto w0 = make_wire_frame(0);
    auto w1 = make_wire_frame(1);
    auto w0_again = make_wire_frame(0);  // exact repeat, not merely "less than high-water"

    reader.feed(w0.data(), w0.size());
    reader.feed(w1.data(), w1.size());
    reader.feed(w0_again.data(), w0_again.size());

    REQUIRE(reader.stats().duplicates_detected == 1);
    REQUIRE(reader.stats().out_of_order_detected == 0);
}