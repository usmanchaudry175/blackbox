// Standalone probe for the kill -9 mid-write integration test. Loops
// indefinitely writing frames until killed externally — see
// scripts/test_kill_mid_write.sh. Not a Catch2 test.
#include "blackbox/log_writer.hpp"
#include "blackbox/frame.hpp"
#include <cstdio>
#include <unistd.h>

using namespace blackbox;

namespace {

Frame make_probe_frame(uint32_t sequence) {
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

// Writes the last confirmed-safe sequence count to a side file, fsync'd
// so the marker itself survives a SIGKILL landing right after this call.
void write_progress(const std::string& progress_path, uint32_t seq) {
    FILE* f = fopen(progress_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "%u\n", seq);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <base_path> <progress_file>\n", argv[0]);
        return 2;
    }
    const std::string base = argv[1];
    const std::string progress_path = argv[2];

    LogWriter writer(base, /*segment_bytes=*/1024 * 1024);

    uint32_t seq = 0;
    const uint32_t kProgressInterval = 50;

    while (true) {
        if (!writer.write_frame(make_probe_frame(seq))) {
            fprintf(stderr, "write_frame failed at seq=%u\n", seq);
            return 1;
        }
        seq++;
        if (seq % kProgressInterval == 0) {
            write_progress(progress_path, seq);
        }
    }
}