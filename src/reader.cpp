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

void StreamReader::update_sequence_tracking(const Frame& frame) {
    // Detects immediate duplicates/gaps against the most recently accepted
    // frame only. Deliberately does NOT handle out-of-order arrival
    // (Phase 6's reordering fault) — a reordered frame will currently be
    // misreported (a gap, then a "backwards" sequence that falls through
    // unclassified) rather than correctly reconciled. That's Phase 6's
    // job, not Phase 3's — flagging here rather than silently pretending
    // this handles a case it doesn't.
    if (!have_last_sequence_) {
        have_last_sequence_ = true;
        last_sequence_ = frame.sequence;
        return;
    }

    if (frame.sequence == last_sequence_) {
        stats_.duplicates_detected++;
        return;  // don't advance last_sequence_ on a duplicate
    }

    if (frame.sequence > last_sequence_ + 1) {
        stats_.gaps_detected += (frame.sequence - last_sequence_ - 1);
    }
    // frame.sequence < last_sequence_: out-of-order, not yet classified —
    // see note above.

    last_sequence_ = frame.sequence;
}

bool StreamReader::pop_frame(Frame& out) {
    if (ready_.empty()) return false;
    out = ready_.front();
    ready_.pop_front();
    return true;
}

}  // namespace blackbox