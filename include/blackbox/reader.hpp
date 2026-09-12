#pragma once

#include "blackbox/frame.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <deque>
#include <unordered_set>

namespace blackbox {

class StreamReader {
public:
    struct Stats {
        uint64_t bytes_fed = 0;
        uint64_t frames_accepted = 0;
        uint64_t crc_failures = 0;
        uint64_t length_mismatches = 0;
        uint64_t unsupported_version = 0;
        uint64_t too_short = 0;
        uint64_t oversized_discards = 0;  // D13 — no delimiter within kMaxWireFrame
        uint64_t gaps_detected = 0;       // total missing sequence numbers, summed
        uint64_t duplicates_detected = 0;
        uint64_t out_of_order_detected = 0;
    };

    // Feed newly received bytes (from a socket, PTY, or serial port).
    // May produce zero or more decoded frames, retrievable via pop_frame().
    void feed(const uint8_t* data, size_t len);

    // Pops the next decoded frame in arrival order. Returns false if none queued.
    bool pop_frame(Frame& out);

    const Stats& stats() const { return stats_; }

private:
    static constexpr uint32_t kSeenWindow = 1024;
    void process_raw_candidate(const uint8_t* wire, size_t wire_len);
    void update_sequence_tracking(const Frame& frame);

    std::vector<uint8_t> pending_;  // bytes accumulated since the last delimiter
    std::deque<Frame> ready_;
    std::unordered_set<uint32_t> seen_sequences_;  // for duplicate detection
    Stats stats_;

    bool have_last_sequence_ = false;
    uint32_t last_sequence_ = 0;
    void prune_seen_sequences_before(uint32_t threshold);
};

}  // namespace blackbox