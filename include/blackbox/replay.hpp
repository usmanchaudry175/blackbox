#pragma once

#include "blackbox/log_reader.hpp"
#include <cstdint>
#include <vector>
#include <string>

namespace blackbox {

// One event in a reconstructed recording. A Frame event is a normal
// recovered record. A Gap event means one or more sequence numbers
// between the previous and this frame are genuinely missing — reported
// explicitly, never silently absorbed into the frame stream. A
// Duplicate event means this frame's sequence number was already seen;
// it's still reported (with its data), not discarded, matching
// StreamReader's "flag, don't suppress" philosophy.
struct ReplayEvent {
    enum class Kind { Frame, Gap, Duplicate };

    Kind kind;
    LocatedFrame frame;        // valid for Frame and Duplicate
    uint32_t gap_start = 0;    // valid for Gap — inclusive
    uint32_t gap_end = 0;      // valid for Gap — inclusive
};

// Reconstructs a recording from base_path's segments, in file order
// (== arrival/write order). Assumes sequence numbers are non-decreasing
// in file order — true for everything built so far, since nothing
// upstream reorders frames yet (reordering is Phase 6's explicit scope,
// same caveat as reader.cpp and log_index.cpp).
//
// Corrupted/rejected records (per LogReader's on_error) are silently
// absent from the output stream and DO count as gap-worthy missing
// sequence numbers if their sequence can't be recovered — there is no
// way to know a corrupted record's intended sequence number, so it is
// indistinguishable from a frame that was never written at all.
std::vector<ReplayEvent> replay(const std::string& base_path);

}  // namespace blackbox