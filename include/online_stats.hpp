// online_stats.hpp - Mean, variance, min and max in ONE pass with O(1) memory (Welford's algorithm).
//
// "Online" = we update the statistics as each value arrives, without storing the values.
// Perfect for a stream of ticks. The naive formula  var = E[x^2] - E[x]^2  subtracts two huge,
// almost equal numbers and loses precision; Welford's update stays numerically stable.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

class OnlineStats {
public:
    void add(double x) {
        ++n_;
        const double delta = x - mean_;               // distance to the OLD mean
        mean_ += delta / static_cast<double>(n_);     // move the mean a little towards x
        m2_ += delta * (x - mean_);                   // OLD delta times NEW delta: sum of squared deviations
        min_ = std::min(min_, x);
        max_ = std::max(max_, x);
    }

    std::size_t count() const { return n_; }
    double mean() const { return mean_; }
    // Sample variance divides by (n - 1) (Bessel's correction): unbiased estimate of the true variance.
    double variance() const { return n_ > 1 ? m2_ / static_cast<double>(n_ - 1) : 0.0; }
    double stddev() const { return std::sqrt(variance()); }
    double min() const { return min_; }
    double max() const { return max_; }

private:
    std::size_t n_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;  // sum of squared deviations from the current mean
    double min_ = std::numeric_limits<double>::infinity();
    double max_ = -std::numeric_limits<double>::infinity();
};
