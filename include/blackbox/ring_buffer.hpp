#pragma once

#include <atomic>
#include <array>
#include <cstddef>

namespace blackbox {

// Single-producer/single-consumer lock-free ring buffer.
//
// Correctness relies on exactly one thread ever calling try_push() and
// exactly one (possibly different) thread ever calling try_pop() — this
// is NOT safe for multiple producers or multiple consumers.
//
// head_ is written only by the producer, tail_ only by the consumer.
// Each is padded to its own cache line (alignas(64)) so the producer
// and consumer threads never invalidate each other's cache line just by
// writing their own index — without this, every push/pop would force a
// cache-coherency round-trip between cores even though the two atomics
// are logically independent (false sharing).
//
// One slot is always left empty by design: head == tail means empty,
// (head + 1) == tail means full. This is the standard trick that lets
// "full" and "empty" be distinguished without a separate size counter
// (which itself would be a third piece of shared state to synchronise).
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity > 1, "Capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer-only. Returns false if the buffer is full (drop-newest
    // policy — the caller is responsible for counting drops; this class
    // only reports success/failure, it doesn't decide what failure means).
    bool try_push(const T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) & kMask;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // full
        }

        buffer_[head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Consumer-only. Returns false if the buffer is empty.
    bool try_pop(T& out) {
        size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == head_.load(std::memory_order_acquire)) {
            return false;  // empty
        }

        out = buffer_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Approximate — only exact if called from the producer thread itself
    // (the consumer's tail_ may have moved between the two loads below).
    // Useful for monitoring/logging, not for correctness decisions.
    size_t size_approx() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & kMask;
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};

    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace blackbox