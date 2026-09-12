#include "blackbox/reader.hpp"
#include "blackbox/log_writer.hpp"
#include "blackbox/frame.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

// Reads raw bytes from a PTY slave path, decodes frames via StreamReader,
// and persists each successfully decoded frame via LogWriter. Runs until
// either the frame count target is hit or the duration elapses, whichever
// comes first — a fixed target makes this reproducible against a known
// generator run, rather than an open-ended capture.
int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <pty_path> <log_base_path> <target_frame_count> <max_duration_seconds>\n", argv[0]);
        return 1;
    }

    const char* pty_path = argv[1];
    const char* log_base = argv[2];
    uint64_t target_frames = std::stoull(argv[3]);
    double max_duration_s = std::stod(argv[4]);

    int fd = open(pty_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", pty_path, strerror(errno));
        return 1;
    }

    blackbox::StreamReader reader;
    blackbox::LogWriter writer(log_base, 1024 * 1024);

    uint8_t buf[4096];
    uint64_t frames_written = 0;
    auto start = std::chrono::steady_clock::now();

    while (frames_written < target_frames) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > max_duration_s) {
            fprintf(stderr, "Stopped: max duration (%.1fs) reached with %llu/%llu frames\n",
                    max_duration_s, static_cast<unsigned long long>(frames_written),
                    static_cast<unsigned long long>(target_frames));
            break;
        }

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            reader.feed(buf, static_cast<size_t>(n));

            blackbox::Frame frame{};
            while (reader.pop_frame(frame)) {
                writer.write_frame(frame);
                frames_written++;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "read() error: %s\n", strerror(errno));
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    close(fd);

    const auto& stats = reader.stats();
    fprintf(stderr, "\n--- Capture stats ---\n");
    fprintf(stderr, "bytes_fed:            %llu\n", static_cast<unsigned long long>(stats.bytes_fed));
    fprintf(stderr, "frames_accepted:      %llu\n", static_cast<unsigned long long>(stats.frames_accepted));
    fprintf(stderr, "crc_failures:         %llu\n", static_cast<unsigned long long>(stats.crc_failures));
    fprintf(stderr, "length_mismatches:    %llu\n", static_cast<unsigned long long>(stats.length_mismatches));
    fprintf(stderr, "unsupported_version:  %llu\n", static_cast<unsigned long long>(stats.unsupported_version));
    fprintf(stderr, "too_short:            %llu\n", static_cast<unsigned long long>(stats.too_short));
    fprintf(stderr, "oversized_discards:   %llu\n", static_cast<unsigned long long>(stats.oversized_discards));
    fprintf(stderr, "gaps_detected:        %llu\n", static_cast<unsigned long long>(stats.gaps_detected));
    fprintf(stderr, "duplicates_detected:  %llu\n", static_cast<unsigned long long>(stats.duplicates_detected));
    fprintf(stderr, "bytes_written to log: %llu\n", static_cast<unsigned long long>(writer.bytes_written()));
    fprintf(stderr, "segment_count:        %u\n", writer.segment_count());

    return 0;
}