// latency_stats.hpp - Summarize latency samples with percentiles (the language of latency).
//
// Why percentiles and not only the average? Latency distributions have a long right tail:
// a few very slow messages (OS interrupts, cache misses, page faults...) hide inside a nice-looking mean.
//   p50 = the typical message, p99 = 1 message in 100 is slower than this, max = the worst case.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

struct LatencySummary {
    double mean_ns = 0.0;
    std::int64_t p50_ns = 0, p90_ns = 0, p99_ns = 0, p999_ns = 0, max_ns = 0;
};

// Nearest-rank percentile of a SORTED vector: the smallest sample such that at least p% of all
// samples are <= it. Other definitions interpolate between samples; this one always returns a real sample.
inline std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    const double n = static_cast<double>(sorted.size());
    const auto rank = static_cast<std::size_t>(std::ceil(p * n / 100.0));  // 1-based rank
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

// Takes the samples BY VALUE: we sort a copy and leave the caller's data (in arrival order) untouched.
inline LatencySummary summarize(std::vector<std::int64_t> samples) {
    LatencySummary s;
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());  // O(n log n): fine at the end of a run, never in the hot path
    s.mean_ns = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
    s.p50_ns = percentile(samples, 50.0);
    s.p90_ns = percentile(samples, 90.0);
    s.p99_ns = percentile(samples, 99.0);
    s.p999_ns = percentile(samples, 99.9);
    s.max_ns = samples.back();
    return s;
}
