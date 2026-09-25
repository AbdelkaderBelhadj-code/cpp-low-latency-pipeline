// main.cpp - Low-latency tick pipeline: how fast can two threads pass data to each other?
//
//   [producer thread]  --- Tick --->  [ queue ]  --- Tick --->  [consumer thread]
//     "market feed"                                               "strategy"
//     makes 1 tick every gap_ns                                   checks the order, computes statistics,
//     stamps the send time                                        records latency = receive time - send time
//
// The same pipeline runs twice, once with MutexQueue and once with SpscQueue, so we can compare them.
// Usage: latency_pipeline [num_ticks=100000] [gap_ns=5000] [csv_path=latencies.csv]

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "latency_stats.hpp"
#include "mutex_queue.hpp"
#include "online_stats.hpp"
#include "spsc_queue.hpp"
#include "tick.hpp"
#include "timing.hpp"

namespace {

constexpr std::size_t kQueueCapacity = 1024;  // power of two (required by SpscQueue)

struct Config {
    std::uint64_t num_ticks = 100'000;
    std::int64_t gap_ns = 5'000;  // time between two ticks: 5 us = 200,000 ticks/s (0 = as fast as possible)
    double volatility = 0.001;    // standard deviation of each tick's log-return (0.1%)
};

struct RunResult {
    std::string name;
    std::vector<std::int64_t> latencies_ns;  // one sample per tick, in arrival order
    double seconds = 0.0;                    // duration of the whole run
    bool in_order = true;                    // did every tick arrive exactly once, in order?
    OnlineStats log_returns;                 // computed by the consumer while the ticks arrive
    double vwap = 0.0;                       // volume-weighted average price
};

// Runs producer + consumer through ANY queue type that has push() and pop().
// Compile-time polymorphism: the template is compiled once per queue type -> no virtual call in the hot path.
template <typename Queue>
RunResult run_pipeline(Queue& queue, const std::string& name, const Config& cfg) {
    RunResult result;
    result.name = name;
    // Allocate AND touch all memory before starting: no malloc and no page fault in the hot path.
    result.latencies_ns.assign(cfg.num_ticks, 0);

    const std::int64_t start_ns = now_ns();

    // Start barrier: the producer waits until the consumer thread is really running.
    // Without it, the first ticks would also measure how long the OS takes to start a thread.
    std::atomic<bool> consumer_ready{false};

    std::thread consumer([&queue, &result, &consumer_ready] {
        consumer_ready.store(true, std::memory_order_release);
        std::uint64_t received = 0;
        double previous_price = 0.0, notional = 0.0, volume = 0.0;
        for (;;) {
            const Tick tick = queue.pop();
            const std::int64_t latency_ns = now_ns() - tick.send_ns;  // measure first, work after
            if (tick.seq == kEndOfStream) break;

            if (tick.seq != received) result.in_order = false;  // lost, duplicated or reordered tick
            if (received < result.latencies_ns.size()) result.latencies_ns[received] = latency_ns;
            ++received;

            if (previous_price > 0.0) result.log_returns.add(std::log(tick.price / previous_price));
            previous_price = tick.price;
            notional += tick.price * tick.qty;
            volume += tick.qty;
        }
        if (received != result.latencies_ns.size()) result.in_order = false;
        result.vwap = volume > 0.0 ? notional / volume : 0.0;
    });

    std::thread producer([&queue, &cfg, &consumer_ready] {
        while (!consumer_ready.load(std::memory_order_acquire)) {}
        std::mt19937_64 rng(42);  // fixed seed: every run sends exactly the same prices
        std::normal_distribution<double> log_return(0.0, cfg.volatility);
        std::uniform_int_distribution<std::int32_t> quantity(1, 100);
        double price = 100.0;
        std::int64_t next_send_ns = now_ns();

        for (std::uint64_t seq = 0; seq < cfg.num_ticks; ++seq) {
            while (now_ns() < next_send_ns) {}  // pace the feed like a real exchange (busy-wait)
            next_send_ns += cfg.gap_ns;
            price *= std::exp(log_return(rng));  // geometric random walk: the price stays positive
            queue.push(Tick{seq, price, quantity(rng), now_ns()});
        }
        queue.push(Tick{kEndOfStream, 0.0, 0, now_ns()});  // "poison pill": tells the consumer to stop
    });

    // join() waits for a thread to finish. It also synchronizes memory:
    // everything the consumer wrote into `result` is now safely visible to this thread.
    producer.join();
    consumer.join();
    result.seconds = static_cast<double>(now_ns() - start_ns) / 1e9;
    return result;
}

void print_report(const std::vector<RunResult>& results, const Config& cfg) {
    std::cout << "\nlatency (ns)      mean       p50       p90       p99     p99.9         max\n";
    std::vector<LatencySummary> summaries;
    for (const RunResult& r : results) {
        const LatencySummary s = summarize(r.latencies_ns);
        summaries.push_back(s);
        std::cout << std::left << std::setw(12) << r.name << std::right << std::fixed << std::setprecision(0)
                  << std::setw(10) << s.mean_ns << std::setw(10) << s.p50_ns << std::setw(10) << s.p90_ns
                  << std::setw(10) << s.p99_ns << std::setw(10) << s.p999_ns << std::setw(12) << s.max_ns << "\n";
    }

    std::cout << "\nconsumer checks (same seed -> both queues must deliver exactly the same data)\n";
    for (const RunResult& r : results) {
        std::cout << "  " << std::left << std::setw(10) << r.name << std::right << " | "
                  << (r.in_order ? "all ticks received in order" : "ERROR: lost or reordered ticks") << " | "
                  << std::setprecision(0) << static_cast<double>(cfg.num_ticks) / r.seconds << " ticks/s | "
                  << "stdev(log-return) = " << std::setprecision(6) << r.log_returns.stddev()
                  << " (expected " << cfg.volatility << ") | VWAP = " << std::setprecision(4) << r.vwap << "\n";
    }

    if (summaries.size() == 2 && summaries[1].p50_ns > 0 && summaries[1].p99_ns > 0) {
        std::cout << "\n" << results[1].name << " vs " << results[0].name << ": p50 is " << std::setprecision(1)
                  << static_cast<double>(summaries[0].p50_ns) / static_cast<double>(summaries[1].p50_ns)
                  << "x lower, p99 is "
                  << static_cast<double>(summaries[0].p99_ns) / static_cast<double>(summaries[1].p99_ns)
                  << "x lower\n";
    }
}

// Raw samples for offline analysis (the python-performance-concurrency repo loads this file with Python).
void write_csv(const std::string& path, const std::vector<RunResult>& results) {
    std::ofstream out(path);
    if (!out) {
        std::cerr << "cannot write " << path << "\n";
        return;
    }
    out << "queue,seq,latency_ns\n";
    std::size_t rows = 0;
    for (const RunResult& r : results) {
        for (std::size_t i = 0; i < r.latencies_ns.size(); ++i, ++rows) {
            out << r.name << ',' << i << ',' << r.latencies_ns[i] << '\n';  // '\n', not std::endl (no flush per line)
        }
    }
    std::cout << "latency samples written to " << path << " (" << rows << " rows)\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    Config cfg;
    if (argc > 1) cfg.num_ticks = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) cfg.gap_ns = std::strtoll(argv[2], nullptr, 10);
    const std::string csv_path = argc > 3 ? argv[3] : "latencies.csv";
    if (cfg.num_ticks == 0 || cfg.gap_ns < 0) {
        std::cerr << "usage: latency_pipeline [num_ticks > 0] [gap_ns >= 0] [csv_path]\n";
        return 1;
    }

    const ClockInfo clock = measure_clock();
    std::cout << "C++ low-latency tick pipeline\n"
              << "  ticks : " << cfg.num_ticks << " per run, one every " << cfg.gap_ns << " ns, queue capacity "
              << kQueueCapacity << "\n"
              << "  clock : " << clock_name() << ", resolution " << clock.resolution_ns << " ns, one call costs ~"
              << std::fixed << std::setprecision(0) << clock.call_cost_ns << " ns\n"
              << "  cores : " << std::thread::hardware_concurrency() << " hardware threads\n";

    MutexQueue<Tick> mutex_queue(kQueueCapacity);
    // The ring buffer stores its 1024 ticks inline (32 KB): keep big objects on the heap, not on the stack.
    auto spsc_queue = std::make_unique<SpscQueue<Tick, kQueueCapacity>>();

    std::vector<RunResult> results;
    results.push_back(run_pipeline(mutex_queue, "MutexQueue", cfg));  // the returned result is moved, not copied
    results.push_back(run_pipeline(*spsc_queue, "SpscQueue", cfg));

    print_report(results, cfg);
    write_csv(csv_path, results);
    return 0;
}
