#include <catch2/catch_test_macros.hpp>
#include "blackbox/log_reader.hpp"
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

Frame make_test_frame(uint32_t sequence, bool with_timestamp) {
    Frame f{};
    f.version = kFormatVersion;
    f.session_id = 3;
    f.sequence = sequence;
    for (size_t i = 0; i < kFieldCount; i++) {
        f.samples[i] = static_cast<int16_t>(sequence * 10 + i);
    }
    f.has_timestamp = with_timestamp;
    f.timestamp_ms = with_timestamp ? sequence * 100 : 0;
    f.first_of_session = (sequence == 0);
    return f;
}

}  // namespace

TEST_CASE("LogReader: round-trips frames written by LogWriter, in order") {
    std::string base = "test_reader_roundtrip";
    TempLogCleanup cleanup(base);

    std::vector<Frame> written;
    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 30; seq++) {
            bool with_ts = (seq % 7 == 0);
            Frame f = make_test_frame(seq, with_ts);
            writer.write_frame(f);
            written.push_back(f);
        }
    }

    LogReader reader(base);
    std::vector<Frame> read_back;
    reader.for_each_frame([&](const LocatedFrame& lf) {
        read_back.push_back(lf.frame);
    });

    REQUIRE(read_back.size() == written.size());
    for (size_t i = 0; i < written.size(); i++) {
        REQUIRE(read_back[i].sequence == written[i].sequence);
        REQUIRE(read_back[i].has_timestamp == written[i].has_timestamp);
        if (read_back[i].has_timestamp) {
            REQUIRE(read_back[i].timestamp_ms == written[i].timestamp_ms);
        }
        for (size_t j = 0; j < kFieldCount; j++) {
            REQUIRE(read_back[i].samples[j] == written[i].samples[j]);
        }
    }
}

TEST_CASE("LogReader: reads correctly across a segment rotation boundary") {
    std::string base = "test_reader_multiseg";
    TempLogCleanup cleanup(base);

    constexpr size_t kSegmentLimit = 45;  // fits 2 no-timestamp frames per segment
    std::vector<Frame> written;
    {
        LogWriter writer(base, kSegmentLimit);
        for (uint32_t seq = 0; seq < 9; seq++) {
            Frame f = make_test_frame(seq, false);
            writer.write_frame(f);
            written.push_back(f);
        }
    }

    LogReader reader(base);
    std::vector<LocatedFrame> read_back;
    reader.for_each_frame([&](const LocatedFrame& lf) {
        read_back.push_back(lf);
    });

    REQUIRE(read_back.size() == written.size());
    for (size_t i = 0; i < written.size(); i++) {
        REQUIRE(read_back[i].frame.sequence == written[i].sequence);
    }

    // Confirm segment_index actually advanced — proves this test is
    // genuinely exercising the multi-segment path, not accidentally
    // fitting in one segment.
    REQUIRE(read_back.back().segment_index > read_back.front().segment_index);
}

TEST_CASE("LogReader: read_at() jumps directly to a known location") {
    std::string base = "test_reader_readat";
    TempLogCleanup cleanup(base);

    constexpr size_t kSegmentLimit = 45;
    std::vector<Frame> written;
    {
        LogWriter writer(base, kSegmentLimit);
        for (uint32_t seq = 0; seq < 9; seq++) {
            Frame f = make_test_frame(seq, false);
            writer.write_frame(f);
            written.push_back(f);
        }
    }

    LogReader reader(base);
    std::vector<LocatedFrame> all_located;
    reader.for_each_frame([&](const LocatedFrame& lf) {
        all_located.push_back(lf);
    });

    // Pick a frame in the middle, forget everything except its recorded
    // location, and confirm read_at() alone reconstructs it — this is
    // exactly what LogIndex::seek() will do against a checkpoint.
    const LocatedFrame& target = all_located[5];
    LocatedFrame result{};
    REQUIRE(reader.read_at(target.segment_index, target.offset, result));
    REQUIRE(result.frame.sequence == target.frame.sequence);
    REQUIRE(result.record_len == target.record_len);
}

TEST_CASE("LogReader: read_at() fails cleanly on an out-of-range location") {
    std::string base = "test_reader_readat_bad";
    TempLogCleanup cleanup(base);

    {
        LogWriter writer(base, 1024 * 1024);
        writer.write_frame(make_test_frame(0, false));
    }

    LogReader reader(base);
    LocatedFrame result{};
    REQUIRE_FALSE(reader.read_at(0, 99999, result));   // offset past end of file
    REQUIRE_FALSE(reader.read_at(7, 0, result));        // segment doesn't exist
}

TEST_CASE("LogReader: truncated trailing record stops cleanly, not as an error") {
    // Simulates Phase 6's "kill -9 mid-write" scenario: a record that's
    // partially on disk. This must be treated as a clean end-of-data,
    // not reported via on_error — a truncated tail is the expected shape
    // of an interrupted write, not corruption.
    std::string base = "test_reader_truncated";
    TempLogCleanup cleanup(base);

    std::vector<Frame> written;
    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 5; seq++) {
            Frame f = make_test_frame(seq, false);
            writer.write_frame(f);
            written.push_back(f);
        }
    }

    // Manually truncate the segment mid-record.
    std::string path = LogReader::segment_path(base, 0);
    auto full_size = fs::file_size(path);
    fs::resize_file(path, full_size - 5);  // chop off part of the last record

    LogReader reader(base);
    std::vector<Frame> read_back;
    bool error_fired = false;
    reader.for_each_frame(
        [&](const LocatedFrame& lf) { read_back.push_back(lf.frame); },
        [&](DecodeError, uint32_t, uint64_t) { error_fired = true; }
    );

    REQUIRE(read_back.size() == written.size() - 1);  // last record lost, rest intact
    REQUIRE_FALSE(error_fired);  // truncation is NOT reported as a decode error
}

TEST_CASE("LogReader: a corrupted record fires on_error without stopping the read") {
    std::string base = "test_reader_corrupt";
    TempLogCleanup cleanup(base);

    std::vector<Frame> written;
    {
        LogWriter writer(base, 1024 * 1024);
        for (uint32_t seq = 0; seq < 5; seq++) {
            Frame f = make_test_frame(seq, false);
            writer.write_frame(f);
            written.push_back(f);
        }
    }

    // Corrupt a payload byte inside the middle record (record index 2,
    // each record is kMinRawFrame bytes, no timestamps in this test).
    std::string path = LogReader::segment_path(base, 0);
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(f.is_open());
        size_t corrupt_offset = 2 * kMinRawFrame + kOffPayload;
        f.seekp(static_cast<std::streamoff>(corrupt_offset));
        char byte;
        f.seekg(static_cast<std::streamoff>(corrupt_offset));
        f.read(&byte, 1);
        byte ^= 0xFF;
        f.seekp(static_cast<std::streamoff>(corrupt_offset));
        f.write(&byte, 1);
    }

    LogReader reader(base);
    std::vector<Frame> read_back;
    std::vector<uint64_t> error_offsets;
    reader.for_each_frame(
        [&](const LocatedFrame& lf) { read_back.push_back(lf.frame); },
        [&](DecodeError err, uint32_t seg, uint64_t offset) {
            REQUIRE(err == DecodeError::BadCrc);
            REQUIRE(seg == 0);
            error_offsets.push_back(offset);
        }
    );

    // 4 good records survive; the corrupted one is skipped via on_error,
    // not silently dropped and not fatal to the rest of the read.
    REQUIRE(read_back.size() == 4);
    REQUIRE(error_offsets.size() == 1);
    REQUIRE(error_offsets[0] == 2 * kMinRawFrame);

    // Confirm the records AFTER the corrupted one were still read
    // correctly — proves offset tracking continued past the bad record
    // rather than getting confused by it.
    bool found_seq_3 = false, found_seq_4 = false;
    for (const auto& f : read_back) {
        if (f.sequence == 3) found_seq_3 = true;
        if (f.sequence == 4) found_seq_4 = true;
    }
    REQUIRE(found_seq_3);
    REQUIRE(found_seq_4);
}