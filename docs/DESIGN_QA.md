# Design Q&A — C++ Low-Latency Tick Pipeline

Questions an engineer or interviewer might ask about this project, and the C++, concurrency and latency
concepts behind it, each with a short answer that points to the code where it can.

Contents:
1. [Questions about the project](#1-questions-about-the-project)
2. [Component deep-dive](#2-component-deep-dive-file-by-file)
3. [C++ language fundamentals](#3-c-language-fundamentals)
4. [Concurrency and multithreading](#4-concurrency-and-multithreading)
5. [Latency and performance](#5-latency-and-performance)
6. [Live-coding exercises](#6-live-coding-exercises-with-short-solutions)

---

## 1. Questions about the project

**1. What problem does this project solve?**
It measures the cost of passing data between two threads, and shows how to make that cost small and
*predictable*. A trading system usually has one thread reading market data and another making decisions, and
the handoff between them sits on the critical path.

**2. Why use two threads instead of one?**
The thread that reads the feed must never fall behind, and computation must not delay the next read. Splitting them
gives parallelism and isolation. The price is the handoff between threads, which is exactly what I measure.

**3. Why compare two queues?**
The mutex queue is the textbook, general, safe solution, so it's the baseline. The SPSC queue is specialised and
optimised. Measuring both turns "lock-free is faster" from an opinion into numbers and exposes the trade-offs.

**4. What were the results, and why is the lock-free queue faster?**
p50 was 75 ns vs 5.3 µs, and p99 was 116 ns vs 60 µs. There are four reasons:
- no sleep and no OS wake-up, because the consumer spins;
- no lock, so no contention and no system calls;
- a pre-allocated array, so there's no `malloc` per message (`std::queue` sits on `std::deque`, which allocates blocks);
- on x86, acquire loads and release stores compile to plain `mov` instructions.

**5. What are the downsides of the lock-free queue?**
- It only works with exactly one producer and one consumer.
- The spinning consumer burns 100% of a core even when there's nothing to do (power, and one core fewer for other work).
- It's harder to write and to review, because memory-ordering bugs are subtle.
- The capacity is fixed.
- On a machine with more threads than cores, spinning can starve the thread you are waiting for.

**6. When would you still choose the mutex queue?**
When there are several producers or consumers, when latency isn't critical (logging, background jobs), when CPU and
power matter more than microseconds, or when code simplicity matters most.

**7. How exactly did you measure latency?**
The producer writes a timestamp into the tick just before `push()`, and the consumer reads the clock right after
`pop()`. Latency is the difference. Both threads use the same clock (the TSC, which is synchronised across cores
on modern CPUs).

**8. Why `rdtsc` and not `std::chrono`?**
I measured the clock first. `steady_clock` had 1 µs resolution with my GCC build, so a 75 ns handoff showed as 0.
The TSC is a CPU register that counts at a constant rate (invariant TSC). It has sub-nanosecond steps and costs about
8–20 ns to read. I convert ticks to nanoseconds with a 50 ms calibration against `steady_clock`.
Caveats: it needs an invariant, synchronised TSC (true on modern single-socket x86), it isn't portable (the code falls
back to `steady_clock`), it can be unreliable in some VMs, and it isn't a serialising instruction (use `rdtscp`/`lfence`
for very short intervals).

**9. What does "measure the ruler" mean?**
Before measuring something, measure the resolution and the overhead of your measuring tool. A clock with 1,000 ns
steps can't measure a 75 ns event. `measure_clock()` prints both numbers at start-up.

**10. Why send one tick every 5 µs instead of as fast as possible?**
If the producer is faster than the consumer, the queue fills up and "latency" becomes time spent waiting in the queue,
which depends on the queue size rather than the handoff. Real feeds arrive at some rate, so pacing measures latency
at a realistic load. The burst mode (`gap_ns = 0`) measures throughput instead.

**11. What is the difference between latency and throughput?**
Latency is the time one item takes (ns per item). Throughput is how many items per second the system handles.
Batching improves throughput but hurts latency, and a full queue can have huge throughput *and* terrible latency.
They are linked by **Little's law**: items in the system = throughput × latency.

**12. Why percentiles instead of the average?**
Latency distributions are skewed, with a long right tail. The mean mixes the typical case with rare disasters and
describes neither. p50 is the typical case, p99 and p99.9 are the tail, and max is the worst case. Tails matter even
more with fan-out: if one request touches 100 services, P(at least one is slower than its p99) = 1 − 0.99¹⁰⁰ ≈ 63%.

**13. Why is the MutexQueue mean (17 µs) three times the median (5.3 µs)?**
A few ticks waited milliseconds (OS scheduling, a sleeping consumer, CPU power states), and they pull the mean up.
The median only looks at the middle value, so the size of those outliers doesn't move it.

**14. What causes the tail (p99.9, max)?**
Mostly things outside my code: interrupts, the scheduler preempting or moving my thread, other processes, CPU
frequency changes and idle states (C-states), page faults, cache misses, and SMIs.
Fixes: pin threads to isolated cores (Linux `isolcpus`, `nohz_full`), move interrupts elsewhere, disable deep
C-states and frequency scaling, pre-fault and lock memory, use huge pages, and avoid system calls in the hot path.

**15. How do you know nothing was lost, duplicated or reordered?**
- Every tick has a sequence number, and the consumer checks `seq == expected`. It also counts the ticks.
- Both runs use the same random seed, so they must produce *identical* VWAP and volatility, and the measured
  volatility (0.001001) matches the producer's parameter (0.001).
- The unit tests check empty/full/wrap-around/FIFO order on one thread, and run a 2-thread stress test with 500,000 items (order and sum).
- On Linux, ThreadSanitizer would find data races automatically.

**16. What is the "poison pill" and why use it?**
It's a special message (`seq = UINT64_MAX`) that tells the consumer to stop. Because it travels through the same FIFO,
everything sent before it is processed first. The alternative, an `atomic<bool> stop` flag, could stop the consumer
before it has drained the queue.

**17. What is the start barrier for?**
The producer waits until the consumer has signalled "I'm running" (an `atomic<bool>` with release/acquire).
In my first version the first ticks showed about 100 µs of latency. That was the OS starting the thread, not the queue.
C++20 has `std::latch` for this.

**18. Why `assign(n, 0)` for the latency vector instead of `reserve(n)`?**
`reserve` allocates, but the OS maps physical pages lazily on the first write, and each first write is a page fault
costing about a microsecond. `assign` writes zeros everywhere, so every page is mapped *before* the benchmark and the
hot path has no allocations and no page faults.

**19. Why is `run_pipeline` a template and not a function taking a base-class interface?**
It's static polymorphism. The compiler generates one version per queue type and can inline `push`/`pop`, so there's no
virtual call (an indirect branch that blocks inlining) in the hot path, and the queues don't need a common base class.
The cost is more generated code, and errors appear at compile time.

**20. Why a fixed random seed?**
Reproducibility. Both runs get the same prices, so the only difference is the queue, and I can check that both results
are identical. It also makes bugs reproducible.

**21. How are prices generated? Why `price *= exp(noise)`?**
It's a geometric random walk: log-returns are normal with a standard deviation of 0.001. Multiplying by `exp(r)` keeps
prices positive (an additive walk can go negative), and log-returns add up over time.

**22. What does the consumer compute, and why "online"?**
The volatility (standard deviation of log-returns) with Welford's algorithm, and the VWAP = Σ(price·qty) / Σqty.
Online means O(1) memory and one update per tick, with no stored history. That's normal in streaming systems.

**23. What surprised you?**
1. The clock resolution was 1 µs, so I had to change clocks.
2. Thread placement: the same physical core gave about 177 M items/s, different cores about 28 M items/s (`bench/queue_throughput.cpp`).
3. `alignas(64)` made no measurable difference, because every push/pop reads the *other* thread's counter anyway (true sharing).
4. The mutex histogram has two peaks: about 1–2 µs and about 5 µs. My hypothesis is whether the consumer's core was in
   a light or a deep idle state when it was woken (or whether the OS spun before really sleeping). I'd test it by
   disabling deep C-states or pinning the threads.
5. In the SPSC run, the first 1,000 ticks were faster (37 ns vs 76 ns). My hypothesis is that both threads started as
   hyper-threads of the same core and the OS later moved one. That matches the pinned benchmark.

**24. Is your measurement affected by "coordinated omission"?**
Partly. I timestamp the *actual* send time. If the producer is itself delayed (for example, blocked on a full
MutexQueue), the ticks it should have sent during that delay are sent late, and that delay isn't counted. The fix is to
measure from the *scheduled* send time (`next_send_ns`) instead of the actual push time. At 5 µs pacing the queue rarely
fills, so the effect is small, but it's worth saying you know about it.

**25. How would you support several producers or consumers?**
A multi-producer or multi-consumer queue needs atomic read-modify-write operations (`fetch_add` or compare-and-swap)
to claim slots, plus a per-slot sequence number (for example Dmitry Vyukov's bounded MPMC queue). That's harder and
slower. Often it's better to keep **one SPSC queue per producer–consumer pair**, or to **shard by key** (for example by
symbol) so each thread owns its own data.

**26. How would this look in a real trading system?**
A feed-handler thread reads UDP multicast (often with kernel bypass: Solarflare/OpenOnload, DPDK), decodes it, and passes
it through an SPSC queue to the strategy thread, which sends orders through another SPSC queue to the order-gateway thread.
Threads are pinned to isolated cores, memory is pre-allocated, and there are no locks, allocations or system calls
in the hot path. Logging goes through yet another queue to a background thread.

**27. What would you improve?**
- Pin the threads in the main program.
- Add a cached copy of the other thread's index in the SPSC queue.
- Use `pop_batch()` to take many items at once.
- Record into an HDR histogram instead of every sample.
- Use `std::jthread`.
- Measure from the scheduled send time.
- Run under ThreadSanitizer.
- Compare with hyper-threading off.

---

## 2. Component deep-dive (file by file)

### tick.hpp
**Why is `sizeof(Tick)` 32 and not 28?**
Alignment. `send_ns` (8 bytes) must sit at an offset that's a multiple of 8, so the compiler adds 4 padding bytes after the
4-byte `qty`. Even with the fields reordered, the struct's size must be a multiple of its alignment (8), so it would still be 32.
Reordering fields from largest to smallest is the usual way to reduce padding.

**What does "trivially copyable" mean? Why does it matter?**
The type can be copied with `memcpy`: it has no user-defined copy/move operations and no destructor. That makes it cheap and
safe to store in a ring buffer, and it's required for `std::atomic<T>` and for sending bytes over a network.

**Why `static_assert`?**
It's a compile-time check. It documents an assumption and breaks the *build* (not production) if someone changes the struct.

**Why `inline constexpr` for `kEndOfStream`?**
`constexpr` makes it a compile-time constant. `inline` (C++17 inline variables) allows a definition in a header that's included
in many `.cpp` files without "multiple definition" link errors.

### timing.hpp
**`steady_clock` vs `system_clock` vs `high_resolution_clock`?**
`system_clock` is wall-clock time, which can jump (NTP, a user changing the time). Use it for dates and timestamps.
`steady_clock` is monotonic and never goes backwards. Use it for durations.
`high_resolution_clock` is just an alias for one of the two, so avoid it.

**What is a "magic static"?**
A function-local `static` variable. Since C++11 it's initialised exactly once, thread-safely, on the first call. I use it
for the TSC calibration. The only cost is a cheap "already initialised?" check on each call.

**Is `rdtsc` reliable?**
On modern x86 with invariant TSC, yes: it ticks at a constant rate whatever the CPU frequency, and it's synchronised across
the cores of a socket. It's less reliable on very old CPUs, in some virtual machines, and across sockets.
It isn't serialising: the CPU may execute it out of order.

### mutex_queue.hpp
**Why `std::unique_lock` and not `std::lock_guard`?**
`condition_variable::wait` has to unlock the mutex while sleeping and re-lock it on wake-up. `lock_guard` can't be unlocked,
but `unique_lock` can (and it's movable).

**What is a spurious wake-up? How do you handle it?**
`wait()` may return without any `notify` (the OS is allowed to do that). Always wait with a predicate:
`cv.wait(lock, [&]{ return !items_.empty(); })`, which loops until the condition is really true.

**What is a lost wake-up?**
The consumer checks "empty", then the producer adds an item and notifies, *then* the consumer goes to sleep and misses the
notification forever. The fix is to change the shared state under the mutex, check the condition under the same mutex, and
let `wait` release the lock atomically as it sleeps.

**Why notify after releasing the lock?**
If you notify while still holding the lock, the woken thread wakes up and immediately blocks on the mutex. Notifying after
unlocking avoids that extra round-trip. Both versions are correct.

**Why is the queue bounded? Why two condition variables?**
Bounded means backpressure. With an unbounded queue, a slow consumer makes memory and latency grow without limit.
There are two different conditions to wait for: producers wait for "not full" and consumers wait for "not empty".

**Why can't you copy a MutexQueue?**
`std::mutex` can't be copied or moved, so the compiler deletes the queue's copy operations automatically.
Copying a lock wouldn't make sense anyway.

### spsc_queue.hpp
**Explain the ring buffer.**
It's a fixed array of `Capacity` slots with two counters that only ever increase: `write_index_` (next slot to write) and
`read_index_` (next slot to read). The slot for counter `i` is `i & (Capacity - 1)`.
The queue is empty when `read == write` and full when `write - read == Capacity`.

**Why must `Capacity` be a power of two?**
Then `i % Capacity` equals `i & (Capacity - 1)`: one AND instruction instead of a division (tens of cycles).
Because 2⁶⁴ is divisible by the capacity, the mapping also stays correct when the counter wraps around.

**Why do the counters keep growing instead of resetting to 0?**
Then "full" (`write - read == Capacity`) and "empty" (`== 0`) can't be confused, and no slot is wasted.
A 64-bit counter would take centuries to overflow, and unsigned subtraction stays correct after a wrap anyway.

**Explain `memory_order_release` / `memory_order_acquire` in this code.**
The producer writes the slot (a normal write), *then* does `write_index_.store(release)`. Release means: every write before
this store becomes visible to any thread that *acquire-loads* this variable and sees the new value. The consumer does
`write_index_.load(acquire)`, so if it sees the new index it's guaranteed to see the slot's content.
The same happens in reverse for `read_index_`: the consumer's release store after copying the slot guarantees that the
producer won't overwrite that slot before the consumer has finished reading it.

**Why is the load of your *own* counter `relaxed`?**
Only this thread writes it, so this thread always sees its own latest value. No synchronisation is needed.

**What would happen with `relaxed` everywhere?**
A data race. The compiler, or a weakly-ordered CPU like ARM, could make the new index visible before the slot's data,
and the consumer would read garbage. It's undefined behaviour in the C++ memory model, even if x86 happens to hide it.

**What do these atomics compile to on x86?**
An acquire load is a plain `mov`, and a release store is a plain `mov` too. x86 is "TSO": it doesn't reorder loads with
loads or stores with stores, so only the compiler must be stopped from reordering. A `seq_cst` store needs `xchg` or
`mov + mfence`. On ARM, acquire/release use special `ldar`/`stlr` instructions.

**What is false sharing? Did `alignas(64)` help?**
False sharing is when two threads write *different* variables that happen to sit on the same 64-byte cache line, so the line
bounces between cores. `alignas(64)` gives each hot variable its own line. In my design it made **no measurable difference**
(I tested it), because every push reads `read_index_` and every pop reads `write_index_`. That's *true* sharing, and those lines
bounce anyway. C++17 also offers `std::hardware_destructive_interference_size`.

**What is the "cached index" optimization?**
The producer keeps a *private copy* of `read_index_` and only reloads the shared atomic when its copy says "full". The
consumer does the same with `write_index_` for "empty". Most operations then touch only their own cache line, and that's
when padding starts to pay off.

**Why spin instead of sleep? What does `_mm_pause()` do?**
Sleeping means a system call plus an OS wake-up (microseconds). A spinning thread sees new data within about 100 ns.
`PAUSE` tells the CPU "this is a spin loop": it saves power, leaves resources to the sibling hyper-thread, and avoids a
pipeline flush when the loop exits. (On Skylake-family Intel CPUs `PAUSE` takes about 140 cycles, so some systems spin
without it on dedicated cores.)

**Is the queue lock-free? Wait-free?**
`try_push`/`try_pop` are *wait-free*: they finish in a bounded number of steps, whatever the other thread does. `push`/`pop`
spin until there is space or data, so they wait by design, but they never take a lock.

**What happens if two producers use this queue?**
A data race: both read the same `write_index_`, write the same slot, and one item is lost. That's undefined behaviour.
Multiple producers need `fetch_add` or CAS to claim slots.

**Why create it with `std::make_unique`?**
It holds 1,024 × 32 bytes = 32 KB inline. Keeping big objects off the stack avoids a stack overflow if the capacity grows
(thread stacks are about 1 MB on Windows). Since C++17, `new` respects `alignas(64)` (aligned new).

### online_stats.hpp
**Explain Welford's algorithm.**
For each new value `x`: `n += 1; delta = x - mean; mean += delta / n; M2 += delta * (x - mean)`. Then `variance = M2 / (n-1)`.
It takes one pass, uses O(1) memory, and is numerically stable.

**Why not `variance = E[x²] − E[x]²`?**
Catastrophic cancellation. With prices around 100 moving by 0.01, both terms are huge and almost equal, so subtracting them
wipes out most significant digits. The result can even come out negative.

**Why divide by n − 1?**
Bessel's correction. The sample mean sits closer to the data than the true mean does, so squared deviations around it
underestimate the true variance. Dividing by n − 1 makes the estimate unbiased.

### latency_stats.hpp
**How do you compute a percentile? Which definition?**
Nearest rank: sort, take `rank = ceil(p/100 × n)`, and return the rank-th value (1-based), which is always a real sample.
Other definitions interpolate between samples (numpy's default is "linear"). With large n they almost agree. With small n,
say which one you use. The Python analysis in [python-performance-concurrency](https://github.com/AbdelkaderBelhadj-code/python-performance-concurrency) uses the same definition, and its numbers match the C++ output exactly.

**Can you avoid the full sort?**
Yes. `std::nth_element` (quickselect) finds one percentile in O(n) on average. For huge or streaming data you'd use an HDR
histogram or a t-digest: fixed memory, approximate but bounded error, and mergeable across threads.

**Why is `summarize` given the vector by value?**
It must sort the data, but the caller needs the original arrival order (for the CSV). Passing by value makes the copy
explicit. If the caller passes a temporary, it's moved instead of copied.

### main.cpp
**What do the lambda captures `[&queue, &cfg]` mean? Any danger?**
They capture by reference: the lambda uses the original objects, with no copy. The danger is a dangling reference if the
thread outlives them. It's safe here because both threads are joined before the function returns.

**What if a `std::thread` is destroyed without `join()` or `detach()`?**
`std::terminate()` is called and the program dies. C++20 `std::jthread` joins automatically in its destructor (RAII). That's
also why an exception thrown between creating and joining the threads would be a problem in this code.

**The consumer writes into `result` while `main` waits. Is that a data race?**
No. Only the consumer writes those fields while it runs, and the calling thread reads them only after `join()`. `join()`
creates a happens-before relationship: everything the thread did is visible afterwards.

**Why `'\n'` instead of `std::endl` in the CSV writer?**
`std::endl` also flushes the stream. 200,000 flushes means 200,000 system calls. `'\n'` lets `ofstream` buffer the output.

**`results.push_back(run_pipeline(...))`: is the big vector copied?**
No. The function returns a temporary, and `push_back(T&&)` moves it: only the pointer to the latency buffer is transferred,
in O(1). Inside the function, the return is usually elided entirely (NRVO).

**What does the anonymous `namespace { ... }` do?**
It gives internal linkage: those names are private to this `.cpp` file (like `static` functions in C), so there are no clashes at link time.

### tests/unit_tests.cpp
**Why not `assert()`?**
`assert` compiles to nothing when `NDEBUG` is defined, and CMake's Release build defines it, so the tests would silently check
nothing. `CHECK` always runs, counts failures, and returns a non-zero exit code, so CI notices.

**How do you test concurrent code?**
- Deterministic single-thread tests for edge cases (empty, full, wrap-around).
- Multi-thread stress tests that check invariants (order, count, sum), run many times with varied timing.
- ThreadSanitizer to find data races.
- Model checkers (Relacy, CDSChecker) for lock-free algorithms.
- A written argument ("this release pairs with that acquire") reviewed by someone else.

---

## 3. C++ language fundamentals

**Pointer vs reference?**
A pointer is an object holding an address. It can be null, can be reassigned, and supports arithmetic. A reference is an
alias: it must be initialised, can't be null (in valid code), and can't be re-bound. Prefer references for "must exist"
parameters, and pointers (or `std::optional`) when "nothing" is a valid value.

**Stack vs heap?**
The stack holds automatic storage: allocation is just moving a pointer, it's freed automatically at the end of the scope,
it's small (about 1–8 MB), and it's cache-hot. The heap is dynamic storage: `new`/`malloc` are slower (they may take a lock
or make a system call), the size is flexible, and you must free it (so use smart pointers).

**What is RAII?**
Resource Acquisition Is Initialization: an object acquires a resource in its constructor and releases it in its destructor.
Destructors run automatically, including during exceptions, so nothing leaks. Examples: `std::lock_guard`,
`std::unique_ptr`, `std::ofstream`, `std::jthread`.

**`unique_ptr`, `shared_ptr`, `weak_ptr`?**
- `unique_ptr`: a single owner, movable but not copyable, and zero overhead compared with a raw pointer.
- `shared_ptr`: shared ownership through a reference count kept in a control block. The count is updated atomically, which
  has a cost. Use `make_shared`, which does one allocation for the object and the control block.
- `weak_ptr`: observes without owning. It breaks reference cycles, and `lock()` gets a `shared_ptr` if the object is still alive.

**`new`/`delete` vs `malloc`/`free`?**
`new` allocates *and* calls the constructor, and `delete` calls the destructor *and* frees. `malloc`/`free` only handle raw
bytes. Never mix them. In modern C++, avoid raw `new`/`delete` and use `make_unique`/`make_shared` or containers.

**Rule of 0 / 3 / 5?**
- Rule of 5: if a class manages a resource and defines one of destructor, copy constructor, copy assignment, move
  constructor or move assignment, it probably needs all five (the rule of 3 is the pre-C++11 version, without the moves).
- Rule of 0: better still, let members like `vector` and `unique_ptr` manage resources, and write none of them.

**What is move semantics? What does `std::move` do?**
Moving transfers the *resources* (for example a heap buffer pointer) from an object that's about to die, instead of copying
them. `std::move` moves nothing: it's a cast to an rvalue reference (`T&&`) that *allows* a move constructor or move assignment
to be chosen. A moved-from object is valid but in an unspecified state: assign to it, destroy it, or call functions
with no preconditions (like `clear()`), but don't rely on its value.

**lvalue vs rvalue?**
An lvalue has a name or identity and you can take its address (`x`, `v[0]`). An rvalue is a temporary (`x + 1`, a function
returning by value). `T&&` binds to rvalues. `std::forward` preserves the kind of value in templates (perfect forwarding).

**Why should move constructors be `noexcept`?**
`std::vector` moves elements on reallocation only if the move constructor is `noexcept`. Otherwise it copies, to keep the
strong exception guarantee. A missing `noexcept` can silently turn moves into copies.

**`const`, `constexpr`, `consteval`?**
`const` means "I won't modify it" (it may still be computed at run time). `constexpr` means it *can* be evaluated at compile
time (it must be for constant variables). `consteval` (C++20) means it *must* be evaluated at compile time.

**What is a const member function? And `mutable`?**
`void f() const` promises not to modify the object, and it can be called on const objects. `mutable` members (a mutex, a
cache) may still change inside const functions.

**What does `inline` mean today?**
Mostly: "this definition may appear in several translation units" (for example in a header), without violating the One
Definition Rule. Whether a call is actually inlined is the compiler's decision.

**The meanings of `static`?**
1. Static local variable: lives for the whole program, initialised once (thread-safe since C++11).
2. Static data member: one copy shared by all instances.
3. Static member function: has no `this`.
4. `static` at namespace scope: internal linkage (private to the `.cpp` file).

**How do virtual functions work? What do they cost?**
Each polymorphic class has a vtable (a table of function pointers), and each object has a hidden vptr pointing to it. A
virtual call loads the vptr, loads the function pointer, and makes an indirect call. That costs a few ns, can be
mispredicted, and above all **prevents inlining**. That's why hot paths prefer templates.

**Why must a base class have a virtual destructor?**
`delete basePtr;` on a derived object without a virtual destructor is undefined behaviour: the derived part isn't destroyed.
The rule is: polymorphic base class means a public virtual destructor (or a protected non-virtual one).

**Static vs dynamic polymorphism? What is CRTP?**
Dynamic polymorphism uses virtual functions: the type is chosen at run time. Static polymorphism uses templates or overloads:
the type is chosen at compile time, it can be inlined, and there's no vtable. CRTP (`class D : public Base<D>`) lets a base
class call derived functions without `virtual`.

**Why are templates usually written in headers?**
The compiler needs the full definition to instantiate a template for each type it's used with, in every translation unit
that uses it.

**STL container complexities?**
- `vector`: O(1) amortised `push_back`, O(1) random access, O(n) insertion in the middle. Contiguous memory, so it's the best
  for caches.
- `deque`: O(1) push and pop at both ends, stored in blocks.
- `list`: O(1) insert and erase with an iterator, but pointer-chasing makes it cache-unfriendly.
- `map`/`set`: red-black tree, O(log n), sorted.
- `unordered_map`/`unordered_set`: hash table, O(1) on average, O(n) in the worst case, not sorted.
In practice `vector` wins most of the time, even where big-O says otherwise, because of the cache.

**How does `vector` grow? What is iterator invalidation?**
When the size exceeds the capacity, `vector` allocates a bigger buffer (2× in libstdc++, 1.5× in MSVC), moves the elements,
and frees the old buffer. All iterators, pointers and references to elements become invalid. Use `reserve()` when you know
the size. Other examples: `erase` invalidates from the erased element onwards, and a `list` only invalidates the erased element.

**`emplace_back` vs `push_back`?**
`push_back(obj)` copies or moves an existing object. `emplace_back(args...)` constructs the object in place from constructor
arguments. For already-built objects they are equivalent.

**`std::array` vs C array vs `std::vector`?**
`std::array` has a fixed size known at compile time, is stored inline with no heap allocation, knows its size, and can be
copied. A C array decays to a pointer. `vector` has a dynamic size on the heap.

**Give examples of undefined behaviour.**
Signed integer overflow, out-of-bounds access, dereferencing null or dangling pointers, use-after-free, double free, reading
uninitialised variables, **data races**, violating strict aliasing, and modifying a string literal. With UB the compiler may
assume "it never happens", so bugs can look completely unrelated to their cause.

**Lambdas: captures, `mutable`, `std::function` cost?**
- `[=]` captures by value (a copy), `[&]` by reference, and `[x, &y]` is explicit (recommended).
- `mutable` allows modifying the captured copies.
- Each lambda has its own unique type. `std::function` erases that type, so it may heap-allocate large lambdas and makes an
  indirect call that can't be inlined. Prefer templates or `auto` parameters in hot code.

**Exceptions and performance?**
With the "zero-cost" model, there's no run-time cost when nothing is thrown, but throwing is very slow (µs). Low-latency code
avoids exceptions on the hot path and uses error codes or `std::expected` (C++23). Mark functions that can't throw as `noexcept`.

**`volatile` vs `std::atomic`?**
`volatile` only stops the compiler from removing or merging accesses (for memory-mapped hardware). It gives **no atomicity
and no ordering** between threads. For threads, use `std::atomic`.

**`struct` vs `class`?**
The only difference is the default access: public for `struct`, private for `class`. By convention, `struct` is used for
plain data.

**What is object slicing?**
Copying a `Derived` into a `Base` *by value* keeps only the Base part. The derived fields and the virtual behaviour are lost.
Pass polymorphic objects by reference or pointer.

**What are the C++ casts?**
- `static_cast`: checked at compile time (numbers, up/down casts you know are safe).
- `dynamic_cast`: checked at run time for polymorphic types; returns `nullptr` or throws on failure.
- `const_cast`: removes `const` (dangerous if the object really is const).
- `reinterpret_cast`: re-reads the bits (low level, often UB).
Avoid C-style casts, because they silently try all of these.

**`nullptr` vs `NULL`?**
`nullptr` has its own type (`std::nullptr_t`) and never gets confused with the integer 0 in overload resolution.

**The stages of compilation?**
The preprocessor expands `#include` and macros, the compiler turns each `.cpp` (translation unit) into assembly, the
assembler produces object files, and the linker combines them and resolves symbols. `#pragma once` or include guards stop
a header from being included twice in the same translation unit.

**`-O2` vs `-O3`, `-march=native`, LTO, PGO?**
- `-O2`: the solid default.
- `-O3`: more aggressive inlining and vectorisation (not always faster: bigger code).
- `-march=native`: uses every instruction this CPU has (AVX2, ...), so the binary isn't portable.
- LTO (link-time optimisation): optimises across `.cpp` files.
- PGO (profile-guided optimisation): compiles, runs on real data, then recompiles using the measured branch statistics.

**Useful C++17 features you used or should know?**
Structured bindings (`auto [a, b] = pair;`), `if constexpr`, `std::optional`, `std::variant`, `std::string_view`,
inline variables, `[[nodiscard]]`, class template argument deduction, `std::clamp`, and fold expressions.

---

## 4. Concurrency and multithreading

**Process vs thread?**
A process has its own address space: it's isolated and more expensive to create. Communication between processes needs IPC
(pipes, sockets, shared memory). Threads share the process's memory: they're cheap to communicate between, but that shared
memory must be synchronised. A crash in one thread kills the whole process.

**Data race vs race condition?**
- A *data race* is two threads accessing the same memory location at the same time, at least one of them writing, with no
  synchronisation. It's undefined behaviour in C++.
- A *race condition* is a logic bug where the result depends on timing (for example check-then-act), even when every access
  is synchronised.

**What does a mutex do? How is it implemented?**
It guarantees mutual exclusion around a critical section. The fast path is an atomic compare-and-swap in user space
(nanoseconds). When the mutex is contended, the thread asks the OS to put it to sleep (Linux `futex`, Windows `SRWLOCK`),
and waking it costs microseconds.

**What is a deadlock? How do you avoid it?**
Two or more threads each wait for a lock the other holds. There are four necessary conditions: mutual exclusion, hold and
wait, no preemption, and circular wait. To avoid it:
- always take locks in the same global order;
- use `std::scoped_lock(m1, m2)` to lock several mutexes safely;
- keep critical sections small, and never call unknown code while holding a lock;
- use `try_lock` with a timeout.

**Livelock, starvation, priority inversion?**
- Livelock: threads keep reacting to each other and make no progress.
- Starvation: one thread never gets the resource.
- Priority inversion: a low-priority thread holds a lock that a high-priority thread needs, and a medium-priority thread keeps
  running instead. The fix is priority inheritance.

**`lock_guard` vs `unique_lock` vs `scoped_lock` vs `shared_lock`?**
- `lock_guard`: the simplest RAII lock.
- `unique_lock`: can be unlocked, relocked, deferred, moved, and is needed for condition variables.
- `scoped_lock` (C++17): locks several mutexes without deadlock.
- `shared_lock` with `shared_mutex`: many readers or one writer.

**What is `std::atomic`? Which operations are atomic?**
Operations on it are indivisible and follow the memory model: `load`, `store`, `exchange`, `fetch_add`,
`compare_exchange_weak/strong`, and `++`. `is_lock_free()` tells you whether it's implemented without a hidden lock
(it is for integer and pointer sizes).

**`compare_exchange_weak` vs `compare_exchange_strong`?**
Both do: "if the value == expected, write desired and return true; otherwise load the current value into expected and return
false". `weak` may fail spuriously (on LL/SC architectures like ARM), so use it inside a loop. `strong` never fails spuriously.

**The memory orders?**
- `relaxed`: atomicity only, no ordering (counters, statistics).
- `acquire` (loads) / `release` (stores): pair up to publish data from one thread to another.
- `acq_rel`: both, for read-modify-write operations.
- `seq_cst` (the default): one global order that all threads agree on. It's the easiest to reason about and costs a little
  more (a full fence on x86 stores).
- `consume`: discouraged; compilers treat it as acquire.

**What does "happens-before" mean?**
If A happens-before B, then B sees the effects of A. You get it from program order within a thread, from a release that
synchronises-with an acquire reading the value it wrote, from mutex unlock→lock, and from thread start and `join`.

**What is a CAS loop? What is the ABA problem?**
A CAS loop reads the value, computes a new one, and tries `compare_exchange`, retrying if another thread changed the value
in between.
ABA: a thread reads A, other threads change it A→B→A, and the CAS wrongly succeeds (classic case: a lock-free stack popping a
node that was freed and reused). Fixes: tagged pointers (a version counter), hazard pointers, epoch-based reclamation.

**Lock-free vs wait-free vs obstruction-free?**
- Lock-free: the *system* always makes progress, but one thread may retry forever.
- Wait-free: *every* thread finishes in a bounded number of steps.
- Obstruction-free: a thread makes progress if it runs alone.
- "Blocking": a stalled lock holder can stop everyone.

**Implement a spinlock. When is it a bad idea?**
Use `std::atomic_flag` with `test_and_set(acquire)` in a loop and `clear(release)` to unlock (see section 6). It's better
to "test-and-test-and-set": spin on a plain load and only try the exchange when the lock looks free, to reduce cache-line
traffic. It's bad when critical sections are long, when there are more threads than cores, or on a single core: you burn
CPU while the lock holder may not even be running.

**What is a condition variable? What is the usage pattern?**
It lets a thread sleep until a condition may have changed. The pattern: lock the mutex, `wait(lock, predicate)`, and use the
data. On the other side: lock, change the state, unlock, `notify_one`/`notify_all`.

**What is a thread pool? Why use one?**
Creating a thread costs tens of microseconds, so a pool keeps N worker threads that take tasks from a shared queue. It also
limits concurrency. For CPU-bound tasks, size it to about the number of cores; for I/O-bound tasks, use more threads.

**`std::async`, `std::future`, `std::promise`?**
`std::async` runs a function (possibly in a new thread) and returns a `future`. `future.get()` waits for the result or
re-throws its exception. A `promise` is the writing end of a future. Watch out: the future returned by `std::async` blocks
in its destructor.

**What is `thread_local`?**
Each thread gets its own copy of the variable. Useful for per-thread counters, buffers or random generators, with no
synchronisation needed.

**How many threads should you use?**
For CPU-bound work, about the number of physical cores (hyper-threads add maybe +0–30%). For I/O-bound work, more, because
threads spend their time waiting. Measure: beyond that point, extra threads only add context switches.

**What does a context switch cost?**
Directly about 1–5 µs (saving and restoring registers, entering the kernel). Indirectly, cold caches and TLB for the next
thread. That's why low-latency threads never block.

**What is Amdahl's law?**
Speed-up = 1 / ((1 − p) + p / N), where p is the parallel fraction and N the number of cores. With p = 95%, the speed-up can
never exceed 20× however many cores you add.

**How do you make a thread-safe singleton?**
Use a function-local static (`static T& instance() { static T t; return t; }`), which is thread-safe since C++11. The
alternative is `std::call_once`. Avoid double-checked locking unless it's done with atomics.

**Why is `counter++` on a shared `int` not thread-safe?**
It's three steps: read, add, write. Two threads can read the same old value, and one increment is lost. Use
`std::atomic<int>` (`fetch_add`) or a mutex.

**What is a memory fence?**
An instruction that stops certain loads and stores from being reordered across it (`std::atomic_thread_fence`, `mfence` on
x86). It's usually better to attach the ordering to the atomic operation itself (acquire/release).

**Which tools find concurrency bugs?**
ThreadSanitizer (`-fsanitize=thread`, for data races), Helgrind/DRD (Valgrind), model checkers for lock-free code, and
stress tests that run many iterations under different timings.

---

## 5. Latency and performance

**Latency numbers you should know (orders of magnitude):**

| Operation | Time |
|---|---|
| L1 cache hit | ~1 ns (4 cycles) |
| L2 cache hit | ~4 ns |
| L3 cache hit | ~10–20 ns |
| Cache line transfer between two cores | ~40–100 ns |
| Main memory (DRAM) | ~80–100 ns |
| Uncontended mutex lock + unlock | ~20 ns |
| System call | ~0.1–1 µs |
| Context switch / waking a sleeping thread | ~1–10 µs |
| SSD random read | ~20–100 µs |
| Network round trip in a data centre | ~100–500 µs |
| Paris ↔ New York round trip | ~70 ms |

**What is a cache line? What are temporal and spatial locality?**
The CPU moves memory in 64-byte blocks called cache lines. Temporal locality: data used recently is likely to be used
again. Spatial locality: neighbouring data is likely to be used next. Contiguous arrays exploit both, and hardware
prefetchers recognise the access pattern and load ahead.

**How do caches stay consistent between cores (cache coherence)?**
With the MESI protocol (Modified, Exclusive, Shared, Invalid; Intel uses MESIF and AMD MOESI). Before a core writes a line,
the copies in other cores are invalidated, and those cores fetch it again on their next access. This is why sharing written
data between threads costs tens of ns.

**AoS vs SoA (data-oriented design)?**
Array of Structs is `vector<Tick>`. Struct of Arrays is `vector<double> prices; vector<int> qtys; ...`. If a loop only needs
prices, SoA loads only prices: fewer cache lines, and SIMD-friendly. AoS is better when you always use every field of one
element together.

**What is branch prediction? How do you help it?**
The CPU guesses which way an `if` will go and runs ahead. A wrong guess costs about 15–20 cycles. Help it with predictable
data (for example sorted data), branchless code (conditional moves, arithmetic), `[[likely]]`/`[[unlikely]]` (C++20), and
by moving rare paths (errors) out of the hot loop.

**Why is a linked list slow even with O(1) insertion?**
Every node sits somewhere else in memory, so each step is a dependent load that may miss the cache (~100 ns). The CPU can't
prefetch or overlap those loads.

**Why avoid memory allocation in the hot path?**
`malloc` can take locks, make system calls, fragment memory, and take a variable amount of time (jitter). Pre-allocate
instead: object pools, arenas, ring buffers, `std::pmr` allocators, `reserve()`.

**Page faults, the TLB, huge pages?**
Virtual memory is mapped in 4 KB pages. The first touch of a page causes a page fault (µs). The TLB caches translations, and
missing it costs a page-table walk. Huge pages (2 MB or 1 GB) mean far fewer TLB misses. Pre-fault memory at start-up
(`assign`, `mlock`).

**What is NUMA?**
On machines with several sockets, each CPU has its own local memory. Accessing the other socket's memory is slower (about
1.5–2×). Keep a thread and its data on the same node (`numactl`, first-touch allocation).

**How do CPU frequency scaling and C-states affect latency?**
An idle core lowers its frequency or goes into a deep sleep state (C-state), and waking it takes microseconds. That's a big
reason why a *sleeping* consumer (MutexQueue) is slow and jittery. Low-latency servers set the "performance" governor and
disable deep C-states.

**What is kernel bypass?**
Letting a user-space program read the network card directly (DPDK, Solarflare OpenOnload/ef_vi, RDMA), without system calls
or kernel copies. It's combined with busy-polling on a dedicated core.

**How do you reduce jitter (tail latency) on Linux?**
`isolcpus`/`nohz_full` to keep the scheduler and timer ticks off your cores, IRQ affinity to move interrupts elsewhere,
thread pinning, disabling hyper-threading or keeping the sibling thread idle, `mlockall`, real-time priority, no swap, and
turning off transparent-huge-page compaction.

**How do you profile C++?**
Sampling profilers (Linux `perf`, Intel VTune, Windows Performance Analyzer) show where the time goes. Flame graphs show it
visually. `perf stat` counts cache misses and branch misses. Measure first, and optimise the proven hot spot.

**What are the common benchmarking mistakes?**
- Measuring a Debug build.
- No warm-up.
- The compiler removing code whose result is unused (use the result, or `benchmark::DoNotOptimize`).
- A clock too coarse for what you measure.
- Tiny samples, and reporting only the mean.
- Frequency scaling and noisy neighbours.
- Coordinated omission.
- Comparing runs on different machines or different CPU placements.

**What is coordinated omission?**
When a load generator waits for each response before sending the next request, it stops sending during a stall. The stall
is then recorded as *one* slow sample instead of the many that real users would have experienced, and the reported tail
looks far better than reality. The fix is to send on a fixed schedule and measure from the *intended* send time.

**What is tail latency amplification?**
With fan-out (one request needs N sub-requests), the slowest sub-request decides the total. At N = 100, 63% of requests hit
at least one p99 event. That's why systems are designed around p99 and p99.9, not the mean.

**What is SIMD?**
Single Instruction, Multiple Data: one instruction processes 4–16 numbers at once (SSE, AVX2, AVX-512). Compilers
auto-vectorise simple loops over contiguous arrays at `-O2`/`-O3`. Branches, pointer aliasing and non-contiguous data prevent it.

**Queueing basics: why does latency explode near 100% utilisation?**
For a simple M/M/1 queue, the average time in the system is W = 1 / (μ − λ) (μ = service rate, λ = arrival rate). As λ
approaches μ, W goes to infinity. Keep hot components well below saturation, and use bounded queues with backpressure.

**How do you log in a low-latency system?**
Never format strings or write files on the hot path. Push a small binary record into an SPSC queue, and let a background
thread format and write it. Avoid `std::endl`, which flushes on every line.

---

## 6. Live-coding exercises (with short solutions)

**1. A thread-safe counter: mutex vs atomic**
```cpp
struct MutexCounter {
    void add() { std::lock_guard<std::mutex> lock(m); ++n; }
    std::mutex m; long n = 0;
};
struct AtomicCounter {
    void add() { n.fetch_add(1, std::memory_order_relaxed); }  // relaxed: only the total matters
    std::atomic<long> n{0};
};
```

**2. A spinlock with `std::atomic_flag`**
```cpp
class SpinLock {
public:
    void lock() noexcept { while (flag_.test_and_set(std::memory_order_acquire)) cpu_relax(); }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }
private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};  // works with std::lock_guard<SpinLock> because it has lock()/unlock()
```

**3. Two threads print odd and even numbers alternately (1..10)**
```cpp
std::mutex m; std::condition_variable cv; int next = 1;
auto worker = [&](int parity) {
    while (true) {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [&] { return next > 10 || next % 2 == parity; });
        if (next > 10) return;
        std::cout << next++ << "\n";
        cv.notify_all();
    }
};
std::thread odd(worker, 1), even(worker, 0);
odd.join(); even.join();
```

**4. Write the SPSC ring buffer from memory.** See `spsc_queue.hpp`. Remember four things: the power-of-two mask, *fill
then publish* (release), *acquire then read*, and each counter written by only one thread.

**5. Moving average over the last N values of a stream, in O(1) per value**
```cpp
class MovingAverage {
public:
    explicit MovingAverage(std::size_t n) : window_(n, 0.0) {}
    double add(double x) {
        sum_ += x - window_[pos_];           // add the new value, remove the one it replaces
        window_[pos_] = x;
        pos_ = (pos_ + 1) % window_.size();
        count_ = std::min(count_ + 1, window_.size());
        return sum_ / static_cast<double>(count_);
    }
private:
    std::vector<double> window_;
    std::size_t pos_ = 0, count_ = 0;
    double sum_ = 0.0;
};
```

**6. An LRU cache (a very common general question)**
Use `std::list<std::pair<Key, Value>>` in recency order and an `std::unordered_map<Key, list::iterator>`. `get`: find it,
move the node to the front with `splice` (O(1)). `put`: insert at the front, and if the size is over capacity, erase the
back node and its map entry. Both operations are O(1).

**7. A simple thread pool (outline)**
N `std::thread` workers loop: `task = queue.pop(); if (!task) break; task();`. The queue is a MutexQueue of
`std::function<void()>`. `submit(f)` pushes a task. The destructor pushes N empty tasks (poison pills) and joins every worker.
To return results, wrap tasks in `std::packaged_task` and hand back the `future`.
