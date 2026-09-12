#include "blackbox/log_reader.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>

namespace blackbox {

namespace {
// A record's total length is determined entirely by its own flags byte —
// no separate length field, matching the wire format's self-describing
// design (D10/D12). This must agree exactly with decode()'s own length
// check; it exists here only to know how many bytes to read BEFORE
// calling decode(), since decode() requires the full buffer up front.
size_t expected_record_len(uint8_t flags) {
    return (flags & kFlagTimestamp) ? kMaxRawFrame : kMinRawFrame;
}
}  // namespace

LogReader::LogReader(std::string base_path) : base_path_(std::move(base_path)) {}

std::string LogReader::segment_path(const std::string& base_path, uint32_t segment_index) {
    std::ostringstream oss;
    oss << base_path << "." << std::setfill('0') << std::setw(4) << segment_index << ".log";
    return oss.str();
}

bool LogReader::read_at(uint32_t segment_index, uint64_t offset, LocatedFrame& out) {
    std::ifstream in(segment_path(base_path_, segment_index), std::ios::binary);
    if (!in) return false;

    in.seekg(static_cast<std::streamoff>(offset));
    if (!in) return false;

    uint8_t buf[kMaxRawFrame];
    in.read(reinterpret_cast<char*>(buf), 1);
    if (in.gcount() != 1) return false;

    size_t len = expected_record_len(buf[0]);
    in.read(reinterpret_cast<char*>(buf) + 1, static_cast<std::streamsize>(len - 1));
    if (static_cast<size_t>(in.gcount()) != len - 1) return false;  // truncated

    Frame frame{};
    if (decode(buf, len, frame) != DecodeError::Ok) return false;

    out.frame = frame;
    out.segment_index = segment_index;
    out.offset = offset;
    out.record_len = len;
    return true;
}

void LogReader::for_each_frame(
    const std::function<void(const LocatedFrame&)>& on_frame,
    const std::function<void(DecodeError, uint32_t, uint64_t)>& on_error
) {
    for (uint32_t seg = 0; ; seg++) {
        std::string path = segment_path(base_path_, seg);
        if (!std::filesystem::exists(path)) break;

        std::ifstream in(path, std::ios::binary);
        if (!in) break;

        uint64_t offset = 0;
        while (true) {
            uint8_t buf[kMaxRawFrame];
            in.read(reinterpret_cast<char*>(buf), 1);
            if (in.gcount() != 1) break;  // clean end of segment

            size_t len = expected_record_len(buf[0]);
            in.read(reinterpret_cast<char*>(buf) + 1, static_cast<std::streamsize>(len - 1));
            if (static_cast<size_t>(in.gcount()) != len - 1) {
                // Fewer bytes remain than this record claims — a
                // truncated trailing record. Expected shape of an
                // interrupted write (Phase 6), not corruption: stop
                // this segment cleanly, don't report as an error.
                break;
            }

            Frame frame{};
            DecodeError err = decode(buf, len, frame);
            if (err != DecodeError::Ok) {
                if (on_error) on_error(err, seg, offset);
                offset += len;
                continue;  // one bad record doesn't stop the rest
            }

            LocatedFrame located{frame, seg, offset, len};
            on_frame(located);
            offset += len;
        }
    }
}

}  // namespace blackbox