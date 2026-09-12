// Phase 7 benchmark: reconstruction fidelity under bursty wire-layer
// frame loss at 5/10/20% target rates.
//
// ASSUMPTION TO VERIFY: StreamReader's public interface is inferred as
//   void feed(const uint8_t* data, size_t len);
//   bool pop_frame(Frame& out);
//   Stats stats() const; // .frames_accepted, .gaps_detected,
//                        // .duplicates_detected, .out_of_order_detected
// None of these have been directly confirmed against reader.hpp in this
// session — fix call sites if names differ.
#include "blackbox/frame.hpp"
#include "blackbox/reader.hpp"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace blackbox;

namespace {

Frame make_bench_frame(uint32_t sequence) {
    Frame f{};
    f.version = kFormatVersion;
    f.session_id = 1;
    f.sequence = sequence;
    for (size_t i = 0; i < kFieldCount; i++) {
        f.samples[i] = static_cast<int16_t>(sequence + i);
    }
    f.has_timestamp = false;
    f.first_of_session = (sequence == 0);
    return f;
}

struct LossResult {
    uint64_t frames_sent;
    uint64_t frames_dropped;
    uint64_t frames_accepted;
    uint64_t gaps_detected;
    uint64_t duplicates_detected;
    uint64_t out_of_order_detected;
    uint64_t payload_mismatches;
    double fidelity_pct;
};

LossResult run_bursty_loss_test(uint64_t frame_count, double target_loss_pct,
                                 uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> burst_len_dist(3, 15);
    std::uniform_int_distribution<int> gap_between_bursts_dist(50, 500);

    std::vector<bool> dropped(frame_count, false);
    uint64_t target_drop_count =
        static_cast<uint64_t>(frame_count * (target_loss_pct / 100.0));
    uint64_t dropped_so_far = 0;
    uint64_t cursor = gap_between_bursts_dist(rng);

    // Wrap around if we run out of frame_count before hitting the
    // target — keeps burst density independent of frame_count so
    // higher loss targets don't silently under-deliver.
    while (dropped_so_far < target_drop_count) {
        if (cursor >= frame_count) {
            cursor = cursor % frame_count;  // wrap, don't stop
            if (cursor == 0) cursor = 1;    // avoid re-landing exactly on 0 forever
        }
        int burst_len = burst_len_dist(rng);
        for (int i = 0; i < burst_len && dropped_so_far < target_drop_count; i++) {
            uint64_t idx = (cursor + i) % frame_count;
            if (!dropped[idx]) {
                dropped[idx] = true;
                dropped_so_far++;
            }
        }
        cursor += burst_len + gap_between_bursts_dist(rng);
    }
    // Build the wire stream from surviving frames only, and keep the
    // original frames around (indexed by sequence) to check payload
    // integrity on the way back out.
    std::vector<uint8_t> wire_stream;
    wire_stream.reserve(frame_count * kMaxWireFrame);  // upper-bound reserve
    std::vector<Frame> original_frames;
    original_frames.reserve(frame_count);

    for (uint64_t seq = 0; seq < frame_count; seq++) {
        Frame f = make_bench_frame(static_cast<uint32_t>(seq));
        original_frames.push_back(f);
        if (dropped[seq]) continue;

        uint8_t encoded[kMaxWireFrame];
        size_t len = encode(f, encoded, sizeof(encoded));
        wire_stream.insert(wire_stream.end(), encoded, encoded + len);
    }

    // Feed the lossy stream through a fresh StreamReader.
    StreamReader reader;
    reader.feed(wire_stream.data(), wire_stream.size());

    uint64_t payload_mismatches = 0;
    Frame recovered{};
    while (reader.pop_frame(recovered)) {
        if (recovered.sequence >= original_frames.size()) {
            payload_mismatches++;
            continue;
        }
        const Frame& expected = original_frames[recovered.sequence];
        bool samples_match = true;
        for (size_t i = 0; i < kFieldCount; i++) {
            if (recovered.samples[i] != expected.samples[i]) {
                samples_match = false;
                break;
            }
        }
        if (!samples_match) payload_mismatches++;
    }

    auto stats = reader.stats();

    LossResult result{};
    result.frames_sent = frame_count;
    result.frames_dropped = dropped_so_far;
    result.frames_accepted = stats.frames_accepted;
    result.gaps_detected = stats.gaps_detected;
    result.duplicates_detected = stats.duplicates_detected;
    result.out_of_order_detected = stats.out_of_order_detected;
    result.payload_mismatches = payload_mismatches;
    result.fidelity_pct =
        100.0 * static_cast<double>(result.frames_accepted) / frame_count;
    return result;
}

void report(double target_pct, const LossResult& r) {
    printf("--- Target loss: %.0f%% (bursty, burst length 3-15) ---\n", target_pct);
    printf("  frames sent:              %llu\n", static_cast<unsigned long long>(r.frames_sent));
    printf("  frames dropped:           %llu (%.2f%% actual)\n",
           static_cast<unsigned long long>(r.frames_dropped),
           100.0 * r.frames_dropped / r.frames_sent);
    printf("  frames accepted:          %llu\n", static_cast<unsigned long long>(r.frames_accepted));
    printf("  fidelity:                 %.3f%%\n", r.fidelity_pct);
    printf("  gaps_detected (sum):      %llu (expected: %llu)\n",
           static_cast<unsigned long long>(r.gaps_detected),
           static_cast<unsigned long long>(r.frames_dropped));
    printf("  duplicates_detected:      %llu (expected: 0)\n", static_cast<unsigned long long>(r.duplicates_detected));
    printf("  out_of_order_detected:    %llu (expected: 0)\n", static_cast<unsigned long long>(r.out_of_order_detected));
    printf("  payload_mismatches:       %llu (expected: 0)\n\n", static_cast<unsigned long long>(r.payload_mismatches));
}

}  // namespace

int main() {
    const uint64_t kFrameCount = 100000;
    const double loss_targets[] = {5.0, 10.0, 20.0};
    uint32_t seed = 42;  // fixed seed for reproducibility

    for (double target : loss_targets) {
        LossResult r = run_bursty_loss_test(kFrameCount, target, seed++);
        report(target, r);
    }

    return 0;
}