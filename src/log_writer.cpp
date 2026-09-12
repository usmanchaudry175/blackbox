#include "blackbox/log_writer.hpp"
#include "blackbox/frame.hpp"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace blackbox {

LogWriter::LogWriter(std::string base_path, size_t segment_max_bytes)
    : base_path_(std::move(base_path)), segment_max_bytes_(segment_max_bytes) {
    open_new_segment();
}

LogWriter::~LogWriter() {
    if (out_.is_open()) {
        out_.flush();
    }
}

std::string LogWriter::segment_path() const {
    std::ostringstream oss;
    oss << base_path_ << "." << std::setfill('0') << std::setw(4) << segment_index_ << ".log";
    return oss.str();
}

void LogWriter::open_new_segment() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    out_.open(segment_path(), std::ios::binary | std::ios::trunc);
    if (!out_) {
        throw std::runtime_error("LogWriter: failed to open segment " + segment_path());
    }
    current_segment_bytes_ = 0;
}

void LogWriter::rotate_if_needed(size_t incoming_len) {
    // Rotate BEFORE writing if this record would push the segment over
    // the limit — guarantees a segment never contains a partial record,
    // which matters for Phase 5's offset-based seeking and Phase 6's
    // "kill -9 mid-write, truncate the partial trailing record cleanly"
    // requirement (a clean segment boundary is not itself a partial write).
    if (current_segment_bytes_ > 0 && current_segment_bytes_ + incoming_len > segment_max_bytes_) {
        segment_index_++;
        open_new_segment();
    }
}

void LogWriter::write_frame(const Frame& frame) {
    uint8_t raw[kMaxRawFrame];
    size_t raw_len = serialize_raw(frame, raw, sizeof(raw));
    if (raw_len == 0) {
        return;  // should be unreachable — frame came from a successful decode()
    }

    rotate_if_needed(raw_len);

    out_.write(reinterpret_cast<const char*>(raw), static_cast<std::streamsize>(raw_len));
    out_.flush();  // Phase 6 will revisit whether every-frame flush is the
                   // right durability/throughput tradeoff once benchmarked (Phase 7)

    current_segment_bytes_ += raw_len;
    bytes_written_ += raw_len;
}

}  // namespace blackbox