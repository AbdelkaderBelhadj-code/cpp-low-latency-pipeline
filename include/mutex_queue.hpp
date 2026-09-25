// mutex_queue.hpp - The "textbook" thread-safe queue: std::queue + std::mutex + std::condition_variable.
//
// + Simple, safe, works for any number of producers and consumers.
// - Every push/pop takes the lock, so the two threads compete for it (contention).
// - When the queue is empty the consumer goes to SLEEP. Waking it up goes through the
//   operating-system scheduler, which costs microseconds. That is the price we measure.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>

template <typename T>
class MutexQueue {
public:
    explicit MutexQueue(std::size_t capacity) : capacity_(capacity) {}

    // Blocks (sleeps) while the queue is full.
    void push(const T& item) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_full_.wait(lock, [this] { return items_.size() < capacity_; });
            items_.push(item);
        }  // unlock first, then notify: the woken thread can take the lock immediately
        not_empty_.notify_one();
    }

    // Blocks (sleeps) while the queue is empty.
    T pop() {
        T item{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // wait() re-checks the condition after every wake-up: protects against spurious wake-ups.
            not_empty_.wait(lock, [this] { return !items_.empty(); });
            item = items_.front();
            items_.pop();
        }
        not_full_.notify_one();
        return item;
    }

private:
    std::mutex mutex_;                   // protects items_
    std::condition_variable not_empty_;  // "an item was added"
    std::condition_variable not_full_;   // "an item was removed"
    std::queue<T> items_;
    const std::size_t capacity_;         // bounded: a fast producer cannot eat all the memory
};
