// spsc_queue.hpp - Lock-free Single-Producer / Single-Consumer ring buffer.
//
// Why it is fast:
//   * no lock and no system call: the threads only read/write two atomic counters;
//   * nobody sleeps: an empty or full queue is handled by spinning (busy-waiting);
//   * one fixed-size array allocated once: no memory allocation per message;
//   * each counter lives on its own cache line: no "false sharing" between the two threads.
//
// Why it is correct without a lock (ONLY with exactly one producer thread and one consumer thread):
//   * write_index_ is written only by the producer, read_index_ only by the consumer;
//   * the producer fills a slot, THEN publishes it with a release store on write_index_;
//     the consumer reads write_index_ with an acquire load, THEN reads the slot.
//     release/acquire guarantees the consumer never sees a half-written slot.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
// PAUSE instruction: tells the CPU "I am spinning". Saves power, leaves resources to the sibling
// hyper-thread, and avoids a pipeline flush when the loop exits.
inline void cpu_relax() { _mm_pause(); }
#else
inline void cpu_relax() {}
#endif

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer thread only. Returns false if the queue is full.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t write = write_index_.load(std::memory_order_relaxed);  // our own counter
        const std::size_t read = read_index_.load(std::memory_order_acquire);    // consumer's progress
        if (write - read == Capacity) return false;                              // full
        buffer_[write & kMask] = item;                                           // 1. fill the slot
        write_index_.store(write + 1, std::memory_order_release);                // 2. publish it
        return true;
    }

    // Consumer thread only. Returns false if the queue is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);    // our own counter
        const std::size_t write = write_index_.load(std::memory_order_acquire);  // producer's progress
        if (read == write) return false;                                         // empty
        out = buffer_[read & kMask];                                             // 1. copy the slot
        read_index_.store(read + 1, std::memory_order_release);                  // 2. hand it back
        return true;
    }

    // Spinning versions: never sleep -> lowest latency, but a waiting thread keeps one CPU core busy.
    void push(const T& item) noexcept {
        while (!try_push(item)) cpu_relax();
    }
    T pop() noexcept {
        T item{};
        while (!try_pop(item)) cpu_relax();
        return item;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;  // i % Capacity == (i & kMask) when Capacity = 2^k

    // The counters only grow (0, 1, 2, ...) and are wrapped into the array with kMask.
    // Unsigned arithmetic makes (write - read) correct even if a counter overflows one day.
    alignas(64) std::atomic<std::size_t> write_index_{0};  // next slot to write (owned by the producer)
    alignas(64) std::atomic<std::size_t> read_index_{0};   // next slot to read (owned by the consumer)
    alignas(64) std::array<T, Capacity> buffer_{};
};
