// unit_tests.cpp - Small tests without any framework. Exit code 0 = all tests passed.
//
// We use our own CHECK macro instead of assert(): assert() disappears when NDEBUG is defined
// (e.g. in Release builds), and we want the tests to run in Release too.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "latency_stats.hpp"
#include "mutex_queue.hpp"
#include "online_stats.hpp"
#include "spsc_queue.hpp"

namespace {

int failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            ++failures;                                                           \
            std::cerr << "FAILED line " << __LINE__ << ": " #condition "\n";      \
        }                                                                         \
    } while (0)

void test_spsc_single_thread() {
    SpscQueue<int, 4> q;
    int x = 0;
    CHECK(!q.try_pop(x));                                      // starts empty
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));                                    // full at capacity
    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_pop(x));
        CHECK(x == i);                                         // FIFO order
    }
    CHECK(!q.try_pop(x));                                      // empty again
    for (int round = 0; round < 10; ++round) {                 // indices wrap around the array many times
        CHECK(q.try_push(round));
        CHECK(q.try_pop(x) && x == round);
    }
}

// The real test of a concurrent queue: two threads, many items, nothing lost, nothing reordered.
template <typename Queue>
void test_two_threads(Queue& q, const char* name) {
    constexpr std::uint64_t kCount = 500'000;
    bool in_order = true;
    std::uint64_t sum = 0;
    std::thread producer([&q] {
        for (std::uint64_t i = 1; i <= kCount; ++i) q.push(i);
    });
    std::thread consumer([&q, &in_order, &sum] {
        for (std::uint64_t i = 1; i <= kCount; ++i) {
            const std::uint64_t value = q.pop();
            if (value != i) in_order = false;
            sum += value;
        }
    });
    producer.join();
    consumer.join();
    CHECK(in_order);
    CHECK(sum == kCount * (kCount + 1) / 2);
    std::cout << "  " << name << ": " << kCount << " items passed between 2 threads, in order\n";
}

void test_online_stats() {
    OnlineStats s;
    for (double x : {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0}) s.add(x);
    CHECK(s.count() == 8);
    CHECK(std::abs(s.mean() - 5.0) < 1e-12);
    CHECK(std::abs(s.variance() - 32.0 / 7.0) < 1e-12);       // sum of squared deviations = 32, n - 1 = 7
    CHECK(s.min() == 2.0 && s.max() == 9.0);
}

void test_percentiles() {
    std::vector<std::int64_t> v;
    for (std::int64_t i = 1; i <= 1000; ++i) v.push_back(i);  // already sorted: 1..1000
    CHECK(percentile(v, 50.0) == 500);
    CHECK(percentile(v, 99.0) == 990);
    CHECK(percentile(v, 99.9) == 999);
    CHECK(percentile(v, 100.0) == 1000);
    CHECK(percentile(v, 0.0) == 1);
    const LatencySummary s = summarize({5, 1, 3, 2, 4});       // unsorted input is fine
    CHECK(s.p50_ns == 3 && s.max_ns == 5 && std::abs(s.mean_ns - 3.0) < 1e-12);
}

}  // namespace

int main() {
    std::cout << "running unit tests\n";
    test_spsc_single_thread();
    test_online_stats();
    test_percentiles();

    MutexQueue<std::uint64_t> mutex_queue(1024);
    test_two_threads(mutex_queue, "MutexQueue");
    SpscQueue<std::uint64_t, 1024> spsc_queue;
    test_two_threads(spsc_queue, "SpscQueue");

    std::cout << (failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
