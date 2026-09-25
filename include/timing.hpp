// timing.hpp - A precise clock for latency measurement, and a way to "measure the ruler" first.
//
// std::chrono::steady_clock is portable, but its resolution depends on the OS and the compiler
// (about 100 ns with MSVC on Windows, 1000 ns with some MinGW builds, ~30 ns on Linux).
// A lock-free queue hands over a message in ~100 ns, so on x86 CPUs we use a finer ruler:
// the Time Stamp Counter (TSC), a CPU register that counts at a constant rate
// (sub-nanosecond steps, ~10 ns to read). We calibrate it once against steady_clock.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define USE_TSC 1
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define USE_TSC 1
#endif

// Portable monotonic clock: it never jumps backwards (the wall clock can, e.g. after an NTP update).
inline std::int64_t steady_now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef USE_TSC
struct TscCalibration {
    std::uint64_t start_ticks;
    double ns_per_tick;
};

// Count TSC ticks during ~50 ms of steady_clock time -> how many nanoseconds one tick lasts.
inline TscCalibration calibrate_tsc() {
    const std::uint64_t ticks0 = __rdtsc();
    const std::int64_t ns0 = steady_now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t ticks1 = __rdtsc();
    const std::int64_t ns1 = steady_now_ns();
    return {ticks0, static_cast<double>(ns1 - ns0) / static_cast<double>(ticks1 - ticks0)};
}

inline const char* clock_name() { return "CPU time-stamp counter (rdtsc)"; }

inline std::int64_t now_ns() {
    static const TscCalibration cal = calibrate_tsc();  // "magic static": computed once, thread-safe (C++11)
    return static_cast<std::int64_t>(static_cast<double>(__rdtsc() - cal.start_ticks) * cal.ns_per_tick);
}
#else
inline const char* clock_name() { return "std::chrono::steady_clock"; }
inline std::int64_t now_ns() { return steady_now_ns(); }
#endif

struct ClockInfo {
    double call_cost_ns;         // average cost of one now_ns() call
    std::int64_t resolution_ns;  // smallest non-zero step the clock can show
};

// A latency number only means something if the clock is much finer than the latency itself.
inline ClockInfo measure_clock(int calls = 1'000'000) {
    std::int64_t smallest_step = std::numeric_limits<std::int64_t>::max();
    const std::int64_t first = now_ns();
    std::int64_t previous = first;
    for (int i = 0; i < calls; ++i) {
        const std::int64_t t = now_ns();
        if (t > previous) smallest_step = std::min(smallest_step, t - previous);
        previous = t;
    }
    return {static_cast<double>(previous - first) / calls, smallest_step};
}
