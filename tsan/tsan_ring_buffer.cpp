// tests/tsan_ring_buffer.cpp — standalone, no Catch2, built separately under
// ThreadSanitizer. ring_buffer.hpp is header-only and has no dependency on
// the rest of the project, so nothing else needs linking here.
#include "blackbox/ring_buffer.hpp"
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>

using namespace blackbox;

int main() {
    constexpr size_t kCapacity = 64;
    constexpr int kCount = 200000;

    SpscRingBuffer<int, kCapacity> rb;
    std::vector<int> received;
    received.reserve(kCount);

    std::thread producer([&] {
        for (int i = 0; i < kCount; i++) {
            while (!rb.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        int val;
        int received_count = 0;
        while (received_count < kCount) {
            if (rb.try_pop(val)) {
                received.push_back(val);
                received_count++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    if (received.size() != static_cast<size_t>(kCount)) {
        std::fprintf(stderr, "FAIL: expected %d items, got %zu\n", kCount, received.size());
        return 1;
    }

    for (int i = 0; i < kCount; i++) {
        if (received[static_cast<size_t>(i)] != i) {
            std::fprintf(stderr, "FAIL: order broken at index %d: got %d\n", i, received[static_cast<size_t>(i)]);
            return 1;
        }
    }

    std::printf("PASS: %d items, correct FIFO order, no TSan findings above this line\n", kCount);
    return 0;
}