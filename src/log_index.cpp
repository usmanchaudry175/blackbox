#include "blackbox/log_index.hpp"
#include <fstream>
#include <algorithm>
#include <cstring>

namespace blackbox {

LogIndex LogIndex::build(const std::string& base_path, uint32_t interval) {
    LogIndex index;
    index.interval_ = interval;

    LogReader reader(base_path);
    uint32_t frame_count = 0;

    reader.for_each_frame(
        [&](const LocatedFrame& lf) {
            if (frame_count % interval == 0) {
                index.checkpoints_.push_back({lf.frame.sequence, lf.segment_index, lf.offset});
            }
            frame_count++;
        }
        // on_error deliberately omitted: a rejected record contributes
        // nothing to frame_count, since it was never a real, retrievable
        // frame to checkpoint against.
    );

    return index;
}

bool LogIndex::seek(uint32_t target_sequence, LogReader& reader, LocatedFrame& out) const {
    if (checkpoints_.empty() || target_sequence < checkpoints_.front().sequence) {
        return false;
    }

    // Binary search for the last checkpoint with sequence <= target.
    auto it = std::upper_bound(
        checkpoints_.begin(), checkpoints_.end(), target_sequence,
        [](uint32_t seq, const Checkpoint& cp) { return seq < cp.sequence; }
    );
    // upper_bound found the first checkpoint STRICTLY GREATER than
    // target; the one we want is one step back.
    --it;

    uint32_t segment = it->segment_index;
    uint64_t offset = it->offset;

    // Bounded scan: at most `interval` physical records separate this
    // checkpoint from the next one (or end of log), regardless of
    // sequence-number gaps — checkpoints are placed by frame count, not
    // sequence arithmetic, so this bound holds unconditionally.
    for (uint32_t steps = 0; steps <= interval_; steps++) {
        if (!reader.read_at(segment, offset, out)) {
            // read_at failed at this (segment, offset) — most likely
            // we've walked off the end of the current segment. Try the
            // start of the next one before giving up entirely.
            if (!reader.read_at(segment + 1, 0, out)) {
                return false;  // genuinely no more data
            }
            segment += 1;
        }

        if (out.frame.sequence == target_sequence) {
            return true;
        }
        if (out.frame.sequence > target_sequence) {
            // Overshot — target_sequence is a gap, not present in the
            // log. `out` is left holding the frame immediately after
            // the gap, which is useful context for a caller, not
            // nothing.
            return false;
        }

        offset = out.offset + out.record_len;
    }

    return false;  // exceeded the expected bound — treat as not found
                   // rather than scanning indefinitely (same defensive
                   // posture as D13's oversized-frame discard)
}

bool LogIndex::save(const std::string& index_path) const {
    std::ofstream out(index_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    uint32_t magic = 0x424C4958;  // "BLIX" — sanity check on load, not a format version
    uint32_t count = static_cast<uint32_t>(checkpoints_.size());
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&interval_), sizeof(interval_));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& cp : checkpoints_) {
        out.write(reinterpret_cast<const char*>(&cp.sequence), sizeof(cp.sequence));
        out.write(reinterpret_cast<const char*>(&cp.segment_index), sizeof(cp.segment_index));
        out.write(reinterpret_cast<const char*>(&cp.offset), sizeof(cp.offset));
    }

    return static_cast<bool>(out);
}

LogIndex LogIndex::load(const std::string& index_path) {
    LogIndex index;
    std::ifstream in(index_path, std::ios::binary);
    if (!in) return index;  // empty index — caller should check checkpoint_count()

    uint32_t magic = 0, count = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&index.interval_), sizeof(index.interval_));
    in.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (magic != 0x424C4958 || !in) {
        return LogIndex{};  // malformed sidecar file — return empty, don't guess
    }

    index.checkpoints_.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        Checkpoint cp{};
        in.read(reinterpret_cast<char*>(&cp.sequence), sizeof(cp.sequence));
        in.read(reinterpret_cast<char*>(&cp.segment_index), sizeof(cp.segment_index));
        in.read(reinterpret_cast<char*>(&cp.offset), sizeof(cp.offset));
        if (!in) return LogIndex{};  // truncated file — don't return a partial index
        index.checkpoints_.push_back(cp);
    }

    return index;
}

}  // namespace blackbox