#include "blackbox/replay.hpp"
#include <iostream>
#include <iomanip>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <base_path>\n";
        return 1;
    }

    auto events = blackbox::replay(argv[1]);

    uint64_t frame_count = 0, gap_count = 0, dup_count = 0, missing_total = 0;

    for (const auto& ev : events) {
        switch (ev.kind) {
            case blackbox::ReplayEvent::Kind::Frame:
                std::cout << "FRAME seq=" << ev.frame.frame.sequence
                          << " session=" << static_cast<int>(ev.frame.frame.session_id)
                          << "\n";
                frame_count++;
                break;
            case blackbox::ReplayEvent::Kind::Duplicate:
                std::cout << "DUPLICATE seq=" << ev.frame.frame.sequence << "\n";
                dup_count++;
                break;
            case blackbox::ReplayEvent::Kind::Gap:
                std::cout << "GAP missing=[" << ev.gap_start << ".." << ev.gap_end << "] "
                          << "count=" << (ev.gap_end - ev.gap_start + 1) << "\n";
                gap_count++;
                missing_total += (ev.gap_end - ev.gap_start + 1);
                break;
        }
    }

    std::cout << "\n--- Summary ---\n"
              << "Frames: " << frame_count << "\n"
              << "Duplicates: " << dup_count << "\n"
              << "Gap events: " << gap_count << " (total " << missing_total << " missing sequence numbers)\n";

    return 0;
}