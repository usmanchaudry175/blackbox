#include <catch2/catch_test_macros.hpp>
#include "blackbox/ring_buffer.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace blackbox;

TEST_CASE("RingBuffer: single-threaded push/pop and full/empty detection") {
    SpscRingBuffer<int, 4> rb;  // capacity 4 -> 3 usable slots (one always empty)

    int out;
    REQUIRE_FALSE(rb.try_pop(out));  // empty

    REQUIRE(rb.try_push(1));
    REQUIRE(rb.try_push(2));
    REQUIRE(rb.try_push(3));
    REQUIRE_FALSE(rb.try_push(4));  // full — only 3 usable slots

    REQUIRE(rb.try_pop(out)); REQUIRE(out == 1);
    REQUIRE(rb.try_pop(out)); REQUIRE(out == 2);
    REQUIRE(rb.try_pop(out)); REQUIRE(out == 3);
    REQUIRE_FALSE(rb.try_pop(out));  // empty again
}

TEST_CASE("RingBuffer: index wraparound correctness") {
    // Small capacity, many more operations than slots — forces head_/tail_
    // to wrap past the array bounds repeatedly. A bug in the mask
    // arithmetic would only show up here, not in a test that never wraps.
    SpscRingBuffer<int, 4> rb;

    for (int cycle = 0; cycle < 100; cycle++) {
        REQUIRE(rb.try_push(cycle));
        REQUIRE(rb.try_push(cycle * 1000));

        int a, b;
        REQUIRE(rb.try_pop(a));
        REQUIRE(rb.try_pop(b));
        REQUIRE(a == cycle);
        REQUIRE(b == cycle * 1000);
    }
}

TEST_CASE("RingBuffer: two real threads, spin-retry — no loss, strict FIFO order") {
    // Producer and consumer both retry on failure, so nothing should ever
    // actually be dropped here — this test isolates whether the
    // memory-ordering (acquire/release) is correct, not the drop policy.
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

    REQUIRE(received.size() == static_cast<size_t>(kCount));

    // FIFO order must be exact — this is what a memory-ordering bug would
    // break first: values arriving out of sequence, or a stale/torn read.
    for (int i = 0; i < kCount; i++) {
        REQUIRE(received[static_cast<size_t>(i)] == i);
    }
}

TEST_CASE("RingBuffer: real drops under a fast producer, slow consumer") {
    constexpr size_t kCapacity = 8;
    constexpr int kCount = 5000;

    SpscRingBuffer<int, kCapacity> rb;
    std::atomic<int> dropped{0};
    std::atomic<bool> producer_done{false};
    std::vector<int> received;
    received.reserve(kCount);

    std::thread producer([&] {
        for (int i = 0; i < kCount; i++) {
            if (!rb.try_push(i)) {
                dropped++;
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        int val;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (rb.try_pop(val)) {
                received.push_back(val);
            } else if (producer_done.load(std::memory_order_acquire)) {
                // Producer's done and the buffer reported empty — but
                // there's an unavoidable race between those two checks,
                // so confirm emptiness a second time before giving up.
                if (!rb.try_pop(val)) {
                    break;
                }
                received.push_back(val);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });

    producer.join();
    consumer.join();

    INFO("dropped=" << dropped.load() << " received=" << received.size());

    for (size_t i = 1; i < received.size(); i++) {
        REQUIRE(received[i] > received[i - 1]);
    }

    REQUIRE(static_cast<int>(received.size()) + dropped.load() == kCount);
    REQUIRE(dropped.load() > 0);
}