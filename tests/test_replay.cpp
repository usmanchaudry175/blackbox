#include <catch2/catch_test_macros.hpp>
#include "blackbox/replay.hpp"
#include "blackbox/log_writer.hpp"
#include "blackbox/frame.hpp"
#include <filesystem>
#include <vector>

using namespace blackbox;
namespace fs = std::filesystem;

namespace {

struct TempLogCleanup {
    std::string base_path;
    explicit TempLogCleanup(std::string base) : base_path(std::move(base)) {}
    ~TempLogCleanup() {
        for (uint32_t i = 0; i < 100; i++) {
            std::string path = LogReader::segment_path(base_path, i);
            std::error_code ec;
            bool existed = fs::exists(path);
            fs::remove(path, ec);
            if (!existed && i > 0) break;
        }
    }
};

Frame make_test_frame(uint32_t sequence) {
    Frame f{};
    f.version = kFormatVersion;
    f.session_id = 1;
    f.sequence = sequence;
    for (size_t i = 0; i < kFieldCount; i++) {
        f.samples[i] = static_cast<int16_t>(sequence + i);
    }
    f.has_timestamp = false;
    f.first_of_session = (sequence == 0);
    return f;
}

}  // namespace

TEST_CASE("Replay: no gaps, no duplicates — pure Frame events in order") {
    std::string base = "test_replay_clean";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 10; seq++) {
            writer.write_frame(make_test_frame(seq));
        }
    }

    auto events = replay(base);

    REQUIRE(events.size() == 10);
    for (uint32_t i = 0; i < 10; i++) {
        REQUIRE(events[i].kind == ReplayEvent::Kind::Frame);
        REQUIRE(events[i].frame.frame.sequence == i);
    }
}

TEST_CASE("Replay: a single gap produces exactly one Gap event with correct bounds") {
    std::string base = "test_replay_gap";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 20; seq++) {
            if (seq >= 5 && seq <= 8) continue;  // 5,6,7,8 missing — 4 dropped
            writer.write_frame(make_test_frame(seq));
        }
    }

    auto events = replay(base);

    // Expect exactly one Gap event, positioned between seq=4's Frame and
    // seq=9's Frame, with no Frame events for the missing range.
    bool found_gap = false;
    for (size_t i = 0; i < events.size(); i++) {
        if (events[i].kind == ReplayEvent::Kind::Gap) {
            REQUIRE_FALSE(found_gap);  // exactly one gap event, not split into several
            found_gap = true;
            REQUIRE(events[i].gap_start == 5);
            REQUIRE(events[i].gap_end == 8);

            // The frame immediately before this gap event must be seq=4,
            // and immediately after must be seq=9 — proves the gap is
            // correctly positioned in the stream, not just correct in
            // isolation.
            REQUIRE(events[i - 1].kind == ReplayEvent::Kind::Frame);
            REQUIRE(events[i - 1].frame.frame.sequence == 4);
            REQUIRE(events[i + 1].kind == ReplayEvent::Kind::Frame);
            REQUIRE(events[i + 1].frame.frame.sequence == 9);
        } else {
            // Every Frame event's sequence must be outside the dropped range.
            REQUIRE_FALSE((events[i].frame.frame.sequence >= 5 && events[i].frame.frame.sequence <= 8));
        }
    }
    REQUIRE(found_gap);

    // Total events: 16 real frames + 1 gap event = 17.
    REQUIRE(events.size() == 17);
}

TEST_CASE("Replay: multiple separate gaps are reported as distinct events") {
    std::string base = "test_replay_multigap";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 30; seq++) {
            if (seq == 3) continue;           // single-frame gap
            if (seq >= 10 && seq <= 11) continue;  // two-frame gap
            writer.write_frame(make_test_frame(seq));
        }
    }

    auto events = replay(base);

    std::vector<std::pair<uint32_t, uint32_t>> gaps;
    for (const auto& ev : events) {
        if (ev.kind == ReplayEvent::Kind::Gap) {
            gaps.push_back({ev.gap_start, ev.gap_end});
        }
    }

    REQUIRE(gaps.size() == 2);
    REQUIRE(gaps[0] == std::make_pair(3u, 3u));
    REQUIRE(gaps[1] == std::make_pair(10u, 11u));
}

TEST_CASE("Replay: a duplicate frame produces a Duplicate event, not a second Frame") {
    std::string base = "test_replay_dup";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        writer.write_frame(make_test_frame(0));
        writer.write_frame(make_test_frame(1));
        writer.write_frame(make_test_frame(1));  // duplicate, written twice
        writer.write_frame(make_test_frame(2));
    }

    auto events = replay(base);

    REQUIRE(events.size() == 4);
    REQUIRE(events[0].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[0].frame.frame.sequence == 0);
    REQUIRE(events[1].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[1].frame.frame.sequence == 1);
    REQUIRE(events[2].kind == ReplayEvent::Kind::Duplicate);
    REQUIRE(events[2].frame.frame.sequence == 1);
    REQUIRE(events[3].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[3].frame.frame.sequence == 2);
}

TEST_CASE("Replay: duplicate does not trigger a false gap afterward") {
    // A naive implementation might advance "last_sequence" on the
    // duplicate and then miscompute the next gap check. This confirms
    // last_sequence correctly stays at the ORIGINAL frame's value.
    std::string base = "test_replay_dup_then_gap_check";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        writer.write_frame(make_test_frame(5));
        writer.write_frame(make_test_frame(5));  // duplicate
        writer.write_frame(make_test_frame(6));  // should NOT be seen as a gap
    }

    auto events = replay(base);

    REQUIRE(events.size() == 3);
    REQUIRE(events[0].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[1].kind == ReplayEvent::Kind::Duplicate);
    REQUIRE(events[2].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[2].frame.frame.sequence == 6);
    // Critically: no Gap event should appear anywhere.
    for (const auto& ev : events) {
        REQUIRE(ev.kind != ReplayEvent::Kind::Gap);
    }
}

TEST_CASE("Replay: empty log produces no events, doesn't crash") {
    std::string base = "test_replay_empty";
    TempLogCleanup cleanup(base);
    {
        LogWriter writer(base, 1024 * 1024);
        // write nothing
    }

    auto events = replay(base);
    REQUIRE(events.empty());
}
// Additions to tests/test_replay.cpp — replay() out-of-order handling

TEST_CASE("Replay: out-of-order frame produces an OutOfOrder event, arrival order preserved") {
    std::string base = "test_replay_ooo";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        // LogWriter/LogReader operate on whatever order write_frame() is
        // called in — writing out of sequence order here simulates a log
        // that somehow ended up with reordered records (e.g. from a
        // future multi-source merge), independent of how StreamReader's
        // own out-of-order detection works upstream.
        writer.write_frame(make_test_frame(0));
        writer.write_frame(make_test_frame(2));
        writer.write_frame(make_test_frame(1));  // out of order
    }

    auto events = replay(base);

    REQUIRE(events.size() == 4);  // Frame(0), Gap(1..1), Frame(2), OutOfOrder(1)
    REQUIRE(events[0].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[0].frame.frame.sequence == 0);
    REQUIRE(events[1].kind == ReplayEvent::Kind::Gap);
    REQUIRE(events[1].gap_start == 1);
    REQUIRE(events[1].gap_end == 1);
    REQUIRE(events[2].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[2].frame.frame.sequence == 2);
    REQUIRE(events[3].kind == ReplayEvent::Kind::OutOfOrder);
    REQUIRE(events[3].frame.frame.sequence == 1);
}

TEST_CASE("Replay: out-of-order frame does not corrupt a subsequent gap calculation") {
    std::string base = "test_replay_ooo_gap_regression";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        writer.write_frame(make_test_frame(0));
        writer.write_frame(make_test_frame(3));  // gap 1,2
        writer.write_frame(make_test_frame(1));  // out of order
        writer.write_frame(make_test_frame(5));  // gap should be just {4}, not {2,3,4}
    }

    auto events = replay(base);

    std::vector<std::pair<uint32_t, uint32_t>> gaps;
    for (const auto& ev : events) {
        if (ev.kind == ReplayEvent::Kind::Gap) {
            gaps.push_back({ev.gap_start, ev.gap_end});
        }
    }

    REQUIRE(gaps.size() == 2);
    REQUIRE((gaps[0] == std::make_pair(1u, 2u)));
    REQUIRE((gaps[1] == std::make_pair(4u, 4u)));  // NOT {2,3,4} — proves no regression
}
TEST_CASE("Replay: duplicate frame with a lower sequence than the current high-water mark") {
    // Confirms the fix in replay()'s seen-set tracking: an exact repeat of
    // an already-seen sequence must be classified as Duplicate even when
    // that sequence is BEHIND the current high-water mark, not just equal
    // to it. The pre-fix implementation only checked seq == last_sequence,
    // so this exact case (frame(0) repeated after frame(1) had already
    // advanced last_sequence to 1) would misclassify the repeat as
    // OutOfOrder instead of Duplicate.
    std::string base = "test_replay_dup_below_high_water";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        writer.write_frame(make_test_frame(0));
        writer.write_frame(make_test_frame(1));
        writer.write_frame(make_test_frame(0));  // exact repeat, not merely "less than high-water"
    }

    auto events = replay(base);

    REQUIRE(events.size() == 3);
    REQUIRE(events[0].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[0].frame.frame.sequence == 0);
    REQUIRE(events[1].kind == ReplayEvent::Kind::Frame);
    REQUIRE(events[1].frame.frame.sequence == 1);
    REQUIRE(events[2].kind == ReplayEvent::Kind::Duplicate);
    REQUIRE(events[2].frame.frame.sequence == 0);

    // Critically: no OutOfOrder event should appear — this is the
    // exact misclassification the seen-set fix corrects.
    for (const auto& ev : events) {
        REQUIRE(ev.kind != ReplayEvent::Kind::OutOfOrder);
    }
}