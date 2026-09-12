// tools/bench_fsync.cpp
//
// Measures the cost of fsync() per frame, in isolation from LogWriter.
// Uses raw POSIX I/O with the same serialize_raw() byte layout as
// LogWriter's on-disk format (D15), so bytes/frame matches, but does
// NOT exercise LogWriter's segment rotation or write_failures() logic.
// This characterizes fsync's overhead, not LogWriter's real durable
// throughput — report it as such.
#include "blackbox/frame.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <string>

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

struct Percentiles { double p50_ns; double p99_ns; };

Percentiles compute_percentiles(std::vector<double>& latencies_ns) {
    std::sort(latencies_ns.begin(), latencies_ns.end());
    size_t n = latencies_ns.size();
    return {latencies_ns[static_cast<size_t>(n * 0.50)],
            latencies_ns[static_cast<size_t>(n * 0.99)]};
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t kFrameCount = 50000;  // smaller: fsync per frame is slow
    const std::string path = (argc > 1) ? argv[1] : "bench_fsync_log";

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "failed to open %s\n", path.c_str());
        return 1;
    }

    std::vector<double> latencies_ns;
    latencies_ns.reserve(kFrameCount);

    uint8_t raw[kMaxRawFrame];  // ASSUMPTION: matches serialize_raw()'s buffer sizing convention
    size_t total_bytes = 0;

    auto start = Clock::now();
    for (uint32_t seq = 0; seq < kFrameCount; seq++) {
        Frame f = make_bench_frame(seq);
        size_t len = serialize_raw(f, raw, sizeof(raw));  // ASSUMPTION: same signature as encode()'s call site

        auto t0 = Clock::now();
        ssize_t written = write(fd, raw, len);
        fsync(fd);
        auto t1 = Clock::now();

        if (written != static_cast<ssize_t>(len)) {
            fprintf(stderr, "short write at seq=%u\n", seq);
            break;
        }
        total_bytes += len;
        latencies_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    auto end = Clock::now();
    close(fd);

    double total_seconds = std::chrono::duration<double>(end - start).count();
    Percentiles pct = compute_percentiles(latencies_ns);
    double frames_per_sec = static_cast<double>(latencies_ns.size()) / total_seconds;

    printf("--- write() + fsync() per frame (durable, isolated from LogWriter) ---\n");
    printf("  frames:          %zu\n", latencies_ns.size());
    printf("  wall time:       %.3f s\n", total_seconds);
    printf("  frames/sec:      %.0f\n", frames_per_sec);
    printf("  p50 latency:     %.0f ns (%.3f us)\n", pct.p50_ns, pct.p50_ns / 1000.0);
    printf("  p99 latency:     %.0f ns (%.3f us)\n", pct.p99_ns, pct.p99_ns / 1000.0);
    printf("  bytes/frame:     %zu\n", total_bytes / latencies_ns.size());

    return 0;
}