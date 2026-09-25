// queue_throughput.cpp - Micro-benchmark: the SPSC queue ALONE (no tick generation, no clock reads),
// with the two threads pinned to chosen logical CPUs, so the OS cannot move them.
//
// Usage: queue_throughput [producer_cpu=2] [consumer_cpu=4]
//   Windows: logical CPUs 2 and 3 are usually the 2 hyper-threads of the SAME physical core,
//            2 and 4 are two DIFFERENT cores (CPU 0 is avoided: it handles many interrupts).
//   Linux: check which CPUs share a core in the CORE column of `lscpu -e`.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

#include "spsc_queue.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX  // stops <windows.h> from defining min/max macros that break std::min/std::max
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void pin_this_thread(int cpu) { SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu); }
#elif defined(__linux__)
#include <pthread.h>
void pin_this_thread(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}
#else
void pin_this_thread(int) {}  // macOS: no hard pinning, the OS decides
#endif

int main(int argc, char* argv[]) {
    const int producer_cpu = argc > 1 ? std::atoi(argv[1]) : 2;
    const int consumer_cpu = argc > 2 ? std::atoi(argv[2]) : 4;
    constexpr std::uint64_t kCount = 50'000'000;

    auto queue = std::make_unique<SpscQueue<std::uint64_t, 1024>>();
    std::uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();

    std::thread consumer([&] {
        pin_this_thread(consumer_cpu);
        for (std::uint64_t i = 0; i < kCount; ++i) sum += queue->pop();
    });
    std::thread producer([&] {
        pin_this_thread(producer_cpu);
        for (std::uint64_t i = 0; i < kCount; ++i) queue->push(i);
    });
    producer.join();
    consumer.join();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const bool ok = sum == kCount * (kCount - 1) / 2;  // 0 + 1 + ... + (kCount - 1)
    std::cout << "CPU " << producer_cpu << " -> CPU " << consumer_cpu << ": "
              << static_cast<double>(kCount) / seconds / 1e6 << " million items/s, "
              << static_cast<double>(seconds) * 1e9 / static_cast<double>(kCount) << " ns per item"
              << (ok ? "" : "  ERROR: wrong sum") << "\n";
    return ok ? 0 : 1;
}
