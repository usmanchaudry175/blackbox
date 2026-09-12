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
    // base_path: files are named "<base_path>.0000.log", ".0001.log", etc.
    // segment_max_bytes: a new segment opens once the current one would
    // exceed this size — never mid-frame, so every segment holds only
    // whole records.
    LogWriter(std::string base_path, size_t segment_max_bytes);
    ~LogWriter();

    // Serialises and appends one frame (raw bytes, no COBS/delimiter —
    // D15). Rotates to a new segment first if needed.
    void write_frame(const Frame& frame);

    uint64_t bytes_written() const { return bytes_written_; }
    uint32_t segment_count() const { return segment_index_ + 1; }

private:
    void rotate_if_needed(size_t incoming_len);
    void open_new_segment();
    std::string segment_path() const;

    std::string base_path_;
    size_t segment_max_bytes_;
    size_t current_segment_bytes_ = 0;
    uint32_t segment_index_ = 0;
    uint64_t bytes_written_ = 0;
    std::ofstream out_;
};

}  // namespace blackbox