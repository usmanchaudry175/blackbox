#pragma once

#include "blackbox/log_reader.hpp"
#include <cstdint>
#include <vector>
#include <string>

namespace blackbox {

// Sparse index: one checkpoint every `interval` frames (by physical
// position in the log, not by sequence-number arithmetic — this matters
// because gaps exist. Checkpointing "every 100th sequence number" would
// silently skip an interval entirely if that exact sequence was dropped;
// checkpointing "every 100th frame actually present" always produces
// evenly-spaced checkpoints regardless of loss.
//
// seek() combines binary search over checkpoints (O(log checkpoints))
// with a bounded forward scan from the nearest one (O(interval) worst
// case) — this is the seek-vs-scan tradeoff the roadmap calls out: not
// a full index (O(1) but large), not a full scan (O(n) but tiny), a
// deliberate middle point sized by `interval`.
//
// Assumes the log's sequence numbers are non-decreasing in file order —
// true for everything built so far, since nothing upstream reorders
// frames yet. Reordering support is explicitly Phase 6's scope, not
// this class's; see reader.cpp's identical caveat.
class LogIndex {
public:
    struct Checkpoint {
        uint32_t sequence;
        uint32_t segment_index;
        uint64_t offset;
    };

    // Scans the entire log at base_path once, recording a checkpoint
    // every `interval` frames encountered (including the very first).
    // Corrupted/rejected records (per LogReader's on_error) are skipped
    // and do not count toward the interval, since they were never
    // actually retrievable frames to begin with.
    static LogIndex build(const std::string& base_path, uint32_t interval = 100);

    // Finds the target sequence number and reads it out via `reader`.
    // Returns false if target_sequence is before the first checkpoint,
    // or genuinely absent from the log (a gap) — in the gap case, `out`
    // is left as the last frame examined before the loop realised it
    // had overshot, so a caller can report exactly where the gap is
    // rather than getting nothing at all.
    bool seek(uint32_t target_sequence, LogReader& reader, LocatedFrame& out) const;

    size_t checkpoint_count() const { return checkpoints_.size(); }
    uint32_t interval() const { return interval_; }

    // Sidecar file I/O — host-local only, not the wire format, so this
    // does NOT follow D11's big-endian rule; it's read only by the same
    // build of this program that wrote it, never transmitted or shared
    // across architectures the way wire-format bytes are.
    bool save(const std::string& index_path) const;
    static LogIndex load(const std::string& index_path);

private:
    std::vector<Checkpoint> checkpoints_;
    uint32_t interval_ = 100;
};

}  // namespace blackbox