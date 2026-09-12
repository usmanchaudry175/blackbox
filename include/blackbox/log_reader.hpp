#pragma once

#include "blackbox/frame.hpp"
#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>

namespace blackbox {

// One frame located on disk — enough information to jump straight back
// to it later without rescanning (used by LogIndex::seek()).
struct LocatedFrame {
    Frame frame;
    uint32_t segment_index;
    uint64_t offset;      // byte offset within that segment file
    size_t record_len;    // 20 or 24, per the flags byte's timestamp bit
};

// Reads segments written by LogWriter for a given base_path, in order
// (segment 0000, 0001, ...). Stops at the first missing segment index,
// matching LogWriter's contiguous naming.
//
// Trusts the file's own records to be self-describing (D15) — no
// delimiter or length prefix needed, since this isn't a corruption-prone
// transport, it's a file we wrote ourselves.
class LogReader {
public:
    explicit LogReader(std::string base_path);

    // Calls on_frame for each successfully decoded record, in file order
    // (== write order). A truncated trailing record at end-of-file
    // (fewer bytes remain than the flags byte claims) is treated as a
    // clean stop, not an error — the expected shape of an interrupted
    // write (Phase 6), not corruption.
    //
    // on_error fires for a record that decode() actively rejects (bad
    // CRC, unsupported version, etc.) WITHOUT stopping the read — one
    // bad record doesn't take the rest of the log down with it.
    void for_each_frame(
        const std::function<void(const LocatedFrame&)>& on_frame,
        const std::function<void(DecodeError, uint32_t segment_index, uint64_t offset)>& on_error = nullptr
    );

    // Reads exactly one record at a known (segment_index, offset) — used
    // by LogIndex::seek() to jump directly to a checkpoint. Returns false
    // if the location is out of range or the record fails to decode.
    bool read_at(uint32_t segment_index, uint64_t offset, LocatedFrame& out);

    static std::string segment_path(const std::string& base_path, uint32_t segment_index);

private:
    std::string base_path_;
};

}  // namespace blackbox