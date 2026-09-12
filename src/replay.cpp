#include "blackbox/replay.hpp"

namespace blackbox {

std::vector<ReplayEvent> replay(const std::string& base_path) {
    std::vector<ReplayEvent> events;
    LogReader reader(base_path);

    bool have_last = false;
    uint32_t last_sequence = 0;

    reader.for_each_frame([&](const LocatedFrame& lf) {
        uint32_t seq = lf.frame.sequence;

        if (!have_last) {
            have_last = true;
            last_sequence = seq;
            events.push_back({ReplayEvent::Kind::Frame, lf, 0, 0});
            return;
        }

        if (seq == last_sequence) {
            events.push_back({ReplayEvent::Kind::Duplicate, lf, 0, 0});
            return;  // don't advance last_sequence on a duplicate
        }

        if (seq > last_sequence + 1) {
            ReplayEvent gap{};
            gap.kind = ReplayEvent::Kind::Gap;
            gap.gap_start = last_sequence + 1;
            gap.gap_end = seq - 1;
            events.push_back(gap);
        }
        // seq < last_sequence: out-of-order, not yet handled — Phase 6's
        // scope, same as StreamReader and LogIndex.

        events.push_back({ReplayEvent::Kind::Frame, lf, 0, 0});
        last_sequence = seq;
    });

    return events;
}

}  // namespace blackbox