#include "blackbox/log_writer.hpp"
#include "blackbox/frame.hpp"
#include <iomanip>
#include <sstream>

namespace blackbox {

LogWriter::LogWriter(std::string base_path, size_t segment_max_bytes)
    : base_path_(std::move(base_path)), segment_max_bytes_(segment_max_bytes) {
    // Constructor still throws on the FIRST segment failing to open —
    // that's a setup-time misconfiguration (bad path, no permissions),
    // categorically different from disk filling up mid-run (D18). A
    // constructor that can silently fail to establish basic invariants
    // is worse than one that throws early and loudly.
    if (!open_new_segment()) {
        throw std::runtime_error("LogWriter: failed to open initial segment " + segment_path());
    }
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

bool LogWriter::open_new_segment() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    out_.open(segment_path(), std::ios::binary | std::ios::trunc);
    if (!out_) {
        return false;
    }
    current_segment_bytes_ = 0;
    return true;
}

bool LogWriter::rotate_if_needed(size_t incoming_len) {
    if (current_segment_bytes_ > 0 && current_segment_bytes_ + incoming_len > segment_max_bytes_) {
        segment_index_++;
        return open_new_segment();
    }
    return true;  // no rotation needed — not a failure
}

bool LogWriter::write_frame(const Frame& frame) {
    uint8_t raw[kMaxRawFrame];
    size_t raw_len = serialize_raw(frame, raw, sizeof(raw));
    if (raw_len == 0) {
        write_failures_++;
        return false;  // should be unreachable — frame came from a successful decode()
    }

    if (!rotate_if_needed(raw_len)) {
        write_failures_++;
        return false;  // couldn't open the next segment — e.g. disk full (D18)
    }

    out_.write(reinterpret_cast<const char*>(raw), static_cast<std::streamsize>(raw_len));
    out_.flush();

    if (!out_) {
        // Write or flush actually failed (D18) — most commonly disk
        // full. Clear the stream's error state so subsequent calls
        // don't just fail immediately on a stale flag, but DON'T retry
        // this write silently; the caller must decide what to do.
        out_.clear();
        write_failures_++;
        return false;
    }

    current_segment_bytes_ += raw_len;
    bytes_written_ += raw_len;
    return true;
}

}  // namespace blackbox