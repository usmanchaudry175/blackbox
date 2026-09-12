// Phase 7 benchmark: pure encode/decode throughput vs. disk-bound
// write_frame() throughput, plus p50/p99 latency and bytes/frame for both.
//
// ASSUMPTION TO VERIFY: encode() is assumed to have the signature
//   size_t encode(const Frame& frame, uint8_t* out, size_t out_capacity)
// mirroring serialize_raw()'s pattern seen in LogWriter::write_frame().
// If frame.hpp declares it differently, fix the call site below.
#include "blackbox/frame.hpp"
#include "blackbox/log_writer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace blackbox;
using Clock = std::chrono::steady_clock;

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

// p50/p99 from a vector of per-frame latencies (nanoseconds). Sorts in
// place — caller's copy, not shared.
struct Percentiles {
    double p50_ns;
    double p99_ns;
};

Percentiles compute_percentiles(std::vector<double>& latencies_ns) {
    std::sort(latencies_ns.begin(), latencies_ns.end());
    size_t n = latencies_ns.size();
    return {
        latencies_ns[static_cast<size_t>(n * 0.50)],
        latencies_ns[static_cast<size_t>(n * 0.99)]
    };
}

void report(const char* label, uint64_t frame_count, double total_seconds,
            const Percentiles& pct, size_t bytes_per_frame) {
    double frames_per_sec = static_cast<double>(frame_count) / total_seconds;
    printf("--- %s ---\n", label);
    printf("  frames:          %llu\n", static_cast<unsigned long long>(frame_count));
    printf("  wall time:       %.3f s\n", total_seconds);
    printf("  frames/sec:      %.0f\n", frames_per_sec);
    printf("  p50 latency:     %.0f ns (%.3f us)\n", pct.p50_ns, pct.p50_ns / 1000.0);
    printf("  p99 latency:     %.0f ns (%.3f us)\n", pct.p99_ns, pct.p99_ns / 1000.0);
    printf("  bytes/frame:     %zu\n\n", bytes_per_frame);
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t kFrameCount = 500000;
    const std::string disk_bench_path = (argc > 1) ? argv[1] : "bench_writer_log";

    // --- Benchmark A: pure encode + decode round-trip (CPU-bound, no I/O) ---
    {
        std::vector<double> latencies_ns;
        latencies_ns.reserve(kFrameCount);

        uint8_t encoded[kMaxWireFrame];
        Frame decoded{};

        auto start = Clock::now();
        for (uint32_t seq = 0; seq < kFrameCount; seq++) {
            Frame f = make_bench_frame(seq);

            auto t0 = Clock::now();
            size_t encoded_len = encode(f, encoded, sizeof(encoded));
            decode(encoded, encoded_len, decoded);
            auto t1 = Clock::now();

            latencies_ns.push_back(
                std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        auto end = Clock::now();

        double total_seconds = std::chrono::duration<double>(end - start).count();
        Percentiles pct = compute_percentiles(latencies_ns);

        // bytes/frame: encoded (wire) size for a base frame, no timestamp
        uint8_t sample_encoded[kMaxWireFrame];
        Frame sample = make_bench_frame(0);
        size_t sample_len = encode(sample, sample_encoded, sizeof(sample_encoded));

        report("Pure encode/decode (CPU-bound)", kFrameCount, total_seconds, pct, sample_len);
    }

    // --- Benchmark B: disk-bound write_frame() throughput ---
    {
        std::vector<double> latencies_ns;
        latencies_ns.reserve(kFrameCount);

        LogWriter writer(disk_bench_path, /*segment_bytes=*/16 * 1024 * 1024);

        auto start = Clock::now();
        for (uint32_t seq = 0; seq < kFrameCount; seq++) {
            Frame f = make_bench_frame(seq);

            auto t0 = Clock::now();
            bool ok = writer.write_frame(f);
            auto t1 = Clock::now();

            if (!ok) {
                fprintf(stderr, "write_frame failed at seq=%u — aborting benchmark B\n", seq);
                break;
            }
            latencies_ns.push_back(
                std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
        auto end = Clock::now();

        double total_seconds = std::chrono::duration<double>(end - start).count();
        Percentiles pct = compute_percentiles(latencies_ns);

        // raw (on-disk) bytes/frame — the D15 storage format, not wire format
        size_t raw_bytes_per_frame = writer.bytes_written() / latencies_ns.size();

        report("Disk-bound write_frame() (I/O-bound)", latencies_ns.size(), total_seconds, pct, raw_bytes_per_frame);
    }

    return 0;
}