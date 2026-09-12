#include "blackbox/replay.hpp"
#include <unordered_set>
namespace blackbox {

std::vector<ReplayEvent> replay(const std::string& base_path) {
    std::vector<ReplayEvent> events;
    LogReader reader(base_path);

    bool have_last = false;
    uint32_t last_sequence = 0;
    std::unordered_set<uint32_t> seen_sequences;
    static constexpr uint32_t kSeenWindow = 1024;  // D19

    reader.for_each_frame([&](const LocatedFrame& lf) {
        uint32_t seq = lf.frame.sequence;

        if (!have_last) {
            have_last = true;
            last_sequence = seq;
            seen_sequences.insert(seq);
            events.push_back({ReplayEvent::Kind::Frame, lf, 0, 0});
            return;
        }

        if (seen_sequences.count(seq)) {
            events.push_back({ReplayEvent::Kind::Duplicate, lf, 0, 0});
            return;
        }

        if (seq < last_sequence) {
            events.push_back({ReplayEvent::Kind::OutOfOrder, lf, 0, 0});
            seen_sequences.insert(seq);
            return;  // does not move last_sequence backward (D17)
        }

        if (seq > last_sequence + 1) {
            ReplayEvent gap{};
            gap.kind = ReplayEvent::Kind::Gap;
            gap.gap_start = last_sequence + 1;
            gap.gap_end = seq - 1;
            events.push_back(gap);
        }

        events.push_back({ReplayEvent::Kind::Frame, lf, 0, 0});
        last_sequence = seq;
        seen_sequences.insert(seq);

        if (last_sequence >= kSeenWindow) {
            for (auto it = seen_sequences.begin(); it != seen_sequences.end(); ) {
                if (*it < last_sequence - kSeenWindow) {
                    it = seen_sequences.erase(it);
                } else {
                    ++it;
                }
            }
        }
    });

    return events;
}

}  // namespace blackbox