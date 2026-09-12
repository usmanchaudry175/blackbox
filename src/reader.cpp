#include "blackbox/reader.hpp"
#include "blackbox/cobs.hpp"

namespace blackbox {

void StreamReader::feed(const uint8_t* data, size_t len) {
    stats_.bytes_fed += len;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b == kDelimiter) {
            if (!pending_.empty()) {
                process_raw_candidate(pending_.data(), pending_.size());
            }
            pending_.clear();
            continue;
        }

        pending_.push_back(b);

        // D13 reader rule: a valid frame's COBS-encoded form (excluding the
        // delimiter, which we strip before accumulating) never exceeds
        // kMaxWireFrame - 1. Exceeding it with no delimiter in sight means
        // the stream is corrupt — discard and resynchronise at the next
        // real delimiter, rather than growing this buffer unboundedly
        // (the exact denial-of-service path D13 calls out).
        if (pending_.size() > kMaxWireFrame - 1) {
            stats_.oversized_discards++;
            pending_.clear();
        }
    }
}

void StreamReader::process_raw_candidate(const uint8_t* wire, size_t wire_len) {
    uint8_t raw[kMaxRawFrame];
    size_t raw_len = cobs_decode(wire, wire_len, raw, sizeof(raw));

    if (raw_len == 0) {
        // Malformed COBS encoding — no finer-grained stat for this yet;
        // bucketed with too_short since both mean "not a usable frame."
        stats_.too_short++;
        return;
    }

    Frame frame{};
    DecodeError err = decode(raw, raw_len, frame);

    switch (err) {
        case DecodeError::TooShort:           stats_.too_short++;            return;
        case DecodeError::LengthMismatch:     stats_.length_mismatches++;    return;
        case DecodeError::BadCrc:             stats_.crc_failures++;         return;
        case DecodeError::UnsupportedVersion: stats_.unsupported_version++;  return;
        case DecodeError::Ok:                 break;
    }

    update_sequence_tracking(frame);
    stats_.frames_accepted++;
    ready_.push_back(frame);
}
void StreamReader::prune_seen_sequences_before(uint32_t threshold) {
    // Guard against underflow when last_sequence_ hasn't advanced past
    // kSeenWindow yet (e.g. early in a session).
    for (auto it = seen_sequences_.begin(); it != seen_sequences_.end(); ) {
        if (*it < threshold) {
            it = seen_sequences_.erase(it);
        } else {
            ++it;
        }
    }
}

void StreamReader::update_sequence_tracking(const Frame& frame) {
    if (!have_last_sequence_) {
        have_last_sequence_ = true;
        last_sequence_ = frame.sequence;
        seen_sequences_.insert(frame.sequence);
        return;
    }

    if (seen_sequences_.count(frame.sequence)) {
        stats_.duplicates_detected++;
        return;
    }

    if (frame.sequence < last_sequence_) {
        // Below the high-water mark but never actually seen before —
        // genuinely late/reordered, not a duplicate (D17).
        stats_.out_of_order_detected++;
        seen_sequences_.insert(frame.sequence);
        return;
    }

    if (frame.sequence > last_sequence_ + 1) {
        stats_.gaps_detected += (frame.sequence - last_sequence_ - 1);
    }

    last_sequence_ = frame.sequence;
    seen_sequences_.insert(frame.sequence);

    if (last_sequence_ >= kSeenWindow) {
        prune_seen_sequences_before(last_sequence_ - kSeenWindow);
    }
}
bool StreamReader::pop_frame(Frame& out) {
    if (ready_.empty()) return false;
    out = ready_.front();
    ready_.pop_front();
    return true;
}

}  // namespace blackbox