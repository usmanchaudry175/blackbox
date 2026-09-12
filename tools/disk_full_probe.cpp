// Standalone probe for the disk-full integration test. Not a Catch2 test —
// it needs a real size-limited filesystem (see scripts/test_disk_full.sh),
// which isn't something to run unattended in a normal `ctest` pass.
#include "blackbox/log_writer.hpp"
#include "blackbox/frame.hpp"
#include <cstdio>
#include <cstdlib>

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
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <base_path>\n", argv[0]);
        return 2;
    }
    const std::string base = argv[1];

    // Small segment size so rotation (open_new_segment/rotate_if_needed)
    // is exercised repeatedly under disk pressure, not just a single
    // fwrite failure at the very end.
    LogWriter writer(base, /*segment_bytes=*/4096);

    uint32_t successes = 0;
    uint32_t failures = 0;
    const uint32_t kMaxAttempts = 200000;  // generous upper bound; tmpfs will fill first

    for (uint32_t seq = 0; seq < kMaxAttempts; seq++) {
        if (writer.write_frame(make_probe_frame(seq))) {
            successes++;
        } else {
            failures++;
            if (failures >= 50) break;  // confirmed sustained failure, stop
        }
    }

    printf("successes=%u failures=%u reported_write_failures=%llu\n",
       successes, failures,
       static_cast<unsigned long long>(writer.write_failures()));
       
    // Sanity: we must have written something before hitting the wall,
    // and the writer must have observed at least one failure — otherwise
    // the tmpfs size in the script wasn't actually small enough to bind.
    if (successes == 0 || failures == 0) {
        fprintf(stderr, "probe did not exercise disk-full path meaningfully\n");
        return 1;
    }
    return 0;
}