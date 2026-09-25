// tick.hpp - The message that travels from the producer thread to the consumer thread.
#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

// One market-data update ("tick"). Plain data only: copying it is a simple 32-byte memcpy.
struct Tick {
    std::uint64_t seq;     // sequence number 0, 1, 2, ... -> the consumer can detect lost/reordered ticks
    double price;          // last traded price
    std::int32_t qty;      // traded quantity
    std::int64_t send_ns;  // producer timestamp taken just before push() -> used to measure latency
};

// Special sequence number used as a "poison pill": it tells the consumer to stop.
inline constexpr std::uint64_t kEndOfStream = std::numeric_limits<std::uint64_t>::max();

// Compile-time checks: they cost nothing at run time and document our assumptions.
static_assert(std::is_trivially_copyable_v<Tick>, "Tick must be plain data (cheap to copy)");
static_assert(sizeof(Tick) == 32, "8 + 8 + 4 (+4 padding) + 8 bytes: two ticks fit in one 64-byte cache line");
