#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <fstream>

namespace blackbox {

struct Frame;

// Owns all disk I/O for the append-only segmented log. Intended to be
// driven exclusively by one writer thread — not thread-safe itself,
// since the SpscRingBuffer upstream already guarantees single-consumer
// access into whatever calls write_frame().
class LogWriter {
public:
    LogWriter(std::string base_path, size_t segment_max_bytes);
    ~LogWriter();

    // Returns false on any write failure (most commonly disk full).
    // The frame is NOT retried or buffered internally — the caller
    // decides what to do (drop, alert, halt), matching the ring
    // buffer's try_push() pattern: this class reports success/failure,
    // it doesn't impose a recovery policy (D18).
    bool write_frame(const Frame& frame);

    uint64_t bytes_written() const { return bytes_written_; }
    uint32_t segment_count() const { return segment_index_ + 1; }
    uint64_t write_failures() const { return write_failures_; }

private:
    bool rotate_if_needed(size_t incoming_len);  // now returns bool too — rotation itself can fail on disk-full
    bool open_new_segment();                      // returns bool instead of throwing (D18 — see below)
    std::string segment_path() const;

    std::string base_path_;
    size_t segment_max_bytes_;
    size_t current_segment_bytes_ = 0;
    uint32_t segment_index_ = 0;
    uint64_t bytes_written_ = 0;
    uint64_t write_failures_ = 0;
    std::ofstream out_;
};

}  // namespace blackbox