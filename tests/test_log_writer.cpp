#include <catch2/catch_test_macros.hpp>
#include "blackbox/log_writer.hpp"
#include "blackbox/frame.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>

using namespace blackbox;
namespace fs = std::filesystem;

namespace {

// RAII helper: removes any "<base>.NNNN.log" segment files left behind,
// pass or fail, so a failed test run doesn't leave stray files for the
// next run to collide with.
struct TempLogCleanup {
    std::string base_path;
    explicit TempLogCleanup(std::string base) : base_path(std::move(base)) {}
    ~TempLogCleanup() {
        for (uint32_t i = 0; i < 100; i++) {
            std::ostringstream oss;
            oss << base_path << "." << std::setfill('0') << std::setw(4) << i << ".log";
            std::error_code ec;
            bool existed = fs::exists(oss.str());
            fs::remove(oss.str(), ec);
            if (!existed && i > 0) break;  // past the last real segment
        }
    }
};

Frame make_test_frame(uint32_t sequence, bool with_timestamp) {
    Frame f{};
    f.version = kFormatVersion;
    f.session_id = 7;
    f.sequence = sequence;
    for (size_t i = 0; i < kFieldCount; i++) {
        f.samples[i] = static_cast<int16_t>(sequence + i);
    }
    f.has_timestamp = with_timestamp;
    f.timestamp_ms = with_timestamp ? sequence * 10 : 0;
    f.first_of_session = (sequence == 0);
    return f;
}

std::vector<uint8_t> read_whole_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
}

// Parses one segment's raw bytes into Frames, relying on each record's
// own flags byte to determine its length — D15's "self-describing
// record" claim, actually exercised here rather than just asserted in
// a comment. No delimiter or length prefix exists in this format; the
// only reason this works is that the reader trusts a file it wrote
// itself (see log_writer.hpp's header comment).
//
// NOTE: this parsing logic will need to become real library code (not
// duplicated in a test file) once Phase 5's replay tool needs the exact
// same thing.
std::vector<Frame> parse_segment(const std::vector<uint8_t>& buf) {
    std::vector<Frame> out;
    size_t offset = 0;
    while (offset < buf.size()) {
        uint8_t flags = buf[offset];
        bool has_ts = (flags & kFlagTimestamp) != 0;
        size_t record_len = has_ts ? kMaxRawFrame : kMinRawFrame;
        REQUIRE(offset + record_len <= buf.size());

        Frame f{};
        DecodeError err = decode(buf.data() + offset, record_len, f);
        REQUIRE(err == DecodeError::Ok);
        out.push_back(f);
        offset += record_len;
    }
    return out;
}

}  // namespace

TEST_CASE("LogWriter: single frame round-trips exactly") {
    std::string base = "test_log_single";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        writer.write_frame(make_test_frame(42, false));
    }  // destructor flushes and closes

    auto buf = read_whole_file(base + ".0000.log");
    REQUIRE(buf.size() == kMinRawFrame);

    auto frames = parse_segment(buf);
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0].sequence == 42);
}

TEST_CASE("LogWriter: multiple frames, mixed timestamp presence, no rotation") {
    std::string base = "test_log_multi";
    TempLogCleanup cleanup(base);

    std::vector<Frame> written;
    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 20; seq++) {
            bool with_ts = (seq % 5 == 0);
            Frame f = make_test_frame(seq, with_ts);
            writer.write_frame(f);
            written.push_back(f);
        }
        REQUIRE(writer.segment_count() == 1);
    }

    auto buf = read_whole_file(base + ".0000.log");
    auto frames = parse_segment(buf);

    REQUIRE(frames.size() == written.size());
    for (size_t i = 0; i < frames.size(); i++) {
        REQUIRE(frames[i].sequence == written[i].sequence);
        REQUIRE(frames[i].has_timestamp == written[i].has_timestamp);
        if (frames[i].has_timestamp) {
            REQUIRE(frames[i].timestamp_ms == written[i].timestamp_ms);
        }
        for (size_t j = 0; j < kFieldCount; j++) {
            REQUIRE(frames[i].samples[j] == written[i].samples[j]);
        }
    }
}

TEST_CASE("LogWriter: rotation never splits a record across segments") {
    // kMinRawFrame is 20 bytes. This limit fits exactly 2 frames (40B)
    // but not 3 (60B) — chosen specifically to NOT align evenly, so an
    // off-by-one in rotate_if_needed() would show up as a truncated
    // record rather than accidentally working by coincidence.
    std::string base = "test_log_rotate";
    TempLogCleanup cleanup(base);

    constexpr size_t kSegmentLimit = 45;
    std::vector<Frame> written;
    {
        LogWriter writer(base, kSegmentLimit);
        for (uint32_t seq = 0; seq < 7; seq++) {
            Frame f = make_test_frame(seq, false);  // fixed 20-byte records
            writer.write_frame(f);
            written.push_back(f);
        }
    }

    std::vector<Frame> all_read;
    for (uint32_t seg = 0; ; seg++) {
        std::ostringstream oss;
        oss << base << "." << std::setfill('0') << std::setw(4) << seg << ".log";
        if (!fs::exists(oss.str())) break;

        auto buf = read_whole_file(oss.str());
        // Every segment's size must be an exact multiple of one record —
        // a partial record here means rotation happened mid-write, which
        // is exactly the bug this test exists to catch.
        REQUIRE(buf.size() % kMinRawFrame == 0);
        REQUIRE(buf.size() <= kSegmentLimit);

        auto frames = parse_segment(buf);
        all_read.insert(all_read.end(), frames.begin(), frames.end());
    }

    REQUIRE(all_read.size() == written.size());
    for (size_t i = 0; i < all_read.size(); i++) {
        REQUIRE(all_read[i].sequence == written[i].sequence);
    }
}

TEST_CASE("LogWriter: bytes_written() and segment_count() are accurate") {
    std::string base = "test_log_stats";
    TempLogCleanup cleanup(base);

    LogWriter writer(base, 45);
    uint64_t expected_bytes = 0;
    for (uint32_t seq = 0; seq < 7; seq++) {
        writer.write_frame(make_test_frame(seq, false));
        expected_bytes += kMinRawFrame;
    }

    REQUIRE(writer.bytes_written() == expected_bytes);
    REQUIRE(writer.segment_count() == 4);  // 7 frames, 2 per segment: 2,2,2,1
}