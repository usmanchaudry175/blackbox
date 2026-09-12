#include <catch2/catch_test_macros.hpp>
#include "blackbox/log_index.hpp"
#include "blackbox/log_writer.hpp"
#include "blackbox/log_reader.hpp"
#include "blackbox/frame.hpp"
#include <filesystem>
#include <vector>

using namespace blackbox;
namespace fs = std::filesystem;

namespace {

struct TempLogCleanup {
    std::string base_path;
    std::string index_path;
    explicit TempLogCleanup(std::string base) : base_path(std::move(base)), index_path(base_path + ".idx") {}
    ~TempLogCleanup() {
        for (uint32_t i = 0; i < 100; i++) {
            std::string path = LogReader::segment_path(base_path, i);
            std::error_code ec;
            bool existed = fs::exists(path);
            fs::remove(path, ec);
            if (!existed && i > 0) break;
        }
        std::error_code ec;
        fs::remove(index_path, ec);
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

TEST_CASE("LogIndex: build produces the expected checkpoint count") {
    std::string base = "test_index_count";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 25; seq++) {
            writer.write_frame(make_test_frame(seq));
        }
    }

    // interval=5, 25 frames -> checkpoints at frame positions 0,5,10,15,20 = 5 checkpoints
    LogIndex index = LogIndex::build(base, 5);
    REQUIRE(index.checkpoint_count() == 5);
    REQUIRE(index.interval() == 5);
}

TEST_CASE("LogIndex: seek finds an exact match at a checkpoint boundary") {
    std::string base = "test_index_exact_checkpoint";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 30; seq++) {
            writer.write_frame(make_test_frame(seq));
        }
    }

    LogIndex index = LogIndex::build(base, 5);
    LogReader reader(base);

    LocatedFrame out{};
    REQUIRE(index.seek(15, reader, out));  // exactly on a checkpoint
    REQUIRE(out.frame.sequence == 15);
}

TEST_CASE("LogIndex: seek finds a target BETWEEN checkpoints, via bounded scan") {
    std::string base = "test_index_between";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 30; seq++) {
            writer.write_frame(make_test_frame(seq));
        }
    }

    LogIndex index = LogIndex::build(base, 5);
    LogReader reader(base);

    LocatedFrame out{};
    REQUIRE(index.seek(17, reader, out));  // checkpoint at 15, scan 2 forward
    REQUIRE(out.frame.sequence == 17);

    REQUIRE(index.seek(0, reader, out));   // the very first frame/checkpoint
    REQUIRE(out.frame.sequence == 0);

    REQUIRE(index.seek(29, reader, out));  // last frame, well past the last checkpoint (25)
    REQUIRE(out.frame.sequence == 29);
}

TEST_CASE("LogIndex: seek into a gap returns false, `out` holds the frame just after it") {
    std::string base = "test_index_gap";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 30; seq++) {
            if (seq >= 10 && seq <= 14) continue;  // sequences 10-14 never written
            writer.write_frame(make_test_frame(seq));
        }
    }

    LogIndex index = LogIndex::build(base, 5);
    LogReader reader(base);

    LocatedFrame out{};
    bool found = index.seek(12, reader, out);  // 12 is in the missing range
    REQUIRE_FALSE(found);
    REQUIRE(out.frame.sequence == 15);  // first real frame after the gap

    // Confirm sequences immediately outside the gap are still findable.
    REQUIRE(index.seek(9, reader, out));
    REQUIRE(out.frame.sequence == 9);
    REQUIRE(index.seek(15, reader, out));
    REQUIRE(out.frame.sequence == 15);
}

TEST_CASE("LogIndex: seek before the first checkpoint fails cleanly") {
    std::string base = "test_index_before_start";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 100; seq < 120; seq++) {  // log starts at sequence 100, not 0
            writer.write_frame(make_test_frame(seq));
        }
    }

    LogIndex index = LogIndex::build(base, 5);
    LogReader reader(base);

    LocatedFrame out{};
    REQUIRE_FALSE(index.seek(50, reader, out));  // before anything in the log
}

TEST_CASE("LogIndex: seek past the end of the log fails cleanly, doesn't hang") {
    std::string base = "test_index_past_end";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 10; seq++) {
            writer.write_frame(make_test_frame(seq));
        }
    }

    LogIndex index = LogIndex::build(base, 5);
    LogReader reader(base);

    LocatedFrame out{};
    REQUIRE_FALSE(index.seek(9999, reader, out));
}

TEST_CASE("LogIndex: seek correctly crosses a segment rotation boundary") {
    // Deliberately misaligned: interval=3, segment fits exactly 2 records
    // (kMinRawFrame*2=40 bytes) — so a checkpoint's forward scan WILL
    // cross into the next segment file before finding some targets,
    // exercising seek()'s "+1 segment" fallback explicitly.
    std::string base = "test_index_segment_cross";
    TempLogCleanup cleanup(base);

    constexpr size_t kSegmentLimit = kMinRawFrame * 2;
    {
        LogWriter writer(base, kSegmentLimit);
        for (uint32_t seq = 0; seq < 20; seq++) {
            writer.write_frame(make_test_frame(seq));
        }
    }

    LogIndex index = LogIndex::build(base, 3);
    LogReader reader(base);

    // Sweep every sequence — several of these targets necessarily
    // require the scan to walk across at least one segment boundary,
    // given segments only hold 2 records each and the interval is 3.
    for (uint32_t seq = 0; seq < 20; seq++) {
        LocatedFrame out{};
        REQUIRE(index.seek(seq, reader, out));
        REQUIRE(out.frame.sequence == seq);
    }
}

TEST_CASE("LogIndex: save and load round-trips exactly, seek still works after reload") {
    std::string base = "test_index_saveload";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 25; seq++) {
            writer.write_frame(make_test_frame(seq));
        }
    }

    LogIndex original = LogIndex::build(base, 5);
    REQUIRE(original.save(cleanup.index_path));

    LogIndex loaded = LogIndex::load(cleanup.index_path);
    REQUIRE(loaded.checkpoint_count() == original.checkpoint_count());
    REQUIRE(loaded.interval() == original.interval());

    LogReader reader(base);
    LocatedFrame out{};
    REQUIRE(loaded.seek(17, reader, out));
    REQUIRE(out.frame.sequence == 17);
}

TEST_CASE("LogIndex: load on a missing file returns an empty index, not a crash") {
    LogIndex index = LogIndex::load("this_file_does_not_exist.idx");
    REQUIRE(index.checkpoint_count() == 0);
}

TEST_CASE("LogIndex: load on a malformed (wrong magic) file returns empty, doesn't misparse") {
    std::string bad_path = "test_index_malformed.idx";
    {
        std::ofstream out(bad_path, std::ios::binary);
        uint32_t junk = 0xDEADBEEF;
        out.write(reinterpret_cast<const char*>(&junk), sizeof(junk));
        out.write(reinterpret_cast<const char*>(&junk), sizeof(junk));
        out.write(reinterpret_cast<const char*>(&junk), sizeof(junk));
    }

    LogIndex index = LogIndex::load(bad_path);
    REQUIRE(index.checkpoint_count() == 0);

    std::error_code ec;
    fs::remove(bad_path, ec);
}