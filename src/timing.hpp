// Copyright (c) 2026 Jean-Louis Leroy
// Distributed under the Boost Software License, Version 1.0.

#ifndef OMB_TIMING_HPP
#define OMB_TIMING_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <numeric>
#include <vector>

#include <sched.h>
#include <x86intrin.h>

namespace omb {

// ---------------------------------------------------------------------------
// Serialization and the timed region.
//
// On Zen (and on all AMD parts since Zen 2) LFENCE is dispatch-serializing by
// default, so `lfence; rdtsc` ... `rdtscp; lfence` brackets the region tightly
// without needing CPUID. We deliberately avoid CPUID: this machine runs under
// WSL2/Hyper-V, where CPUID traps to the hypervisor and costs thousands of
// cycles, which would swamp a single dispatch.
// ---------------------------------------------------------------------------

// The timestamp is captured as the raw `edx:eax` pair and only assembled into a
// 64-bit value *after* the second reading. This matters more than it looks.
//
// With the `__rdtsc()` intrinsic, the shift-and-or that builds the 64-bit value
// has to sit between the two readings -- `rdtscp` overwrites `eax`/`edx`, so the
// first timestamp must be consumed before then -- and each compiler schedules it
// where it likes. gcc emitted `shl`/`mov`/`or` immediately after the second
// `lfence`, inside the measured window; clang emitted them before that `lfence`,
// and in one function sank the `or` past the work. Three instructions either
// way, but placed differently per compiler and, worse, differently between a
// variant and the baseline it is subtracted from -- so the arithmetic did not
// cancel in `disp`, and the cross-compiler columns carried a scheduling artifact
// in the denominator.
//
// Keeping `lo`/`hi` as separate 32-bit locals leaves the compiler nothing to
// schedule: the only thing it must do before `rdtscp` clobbers the registers is
// move them out, and the arithmetic happens after both readings. See README,
// "Why the timestamp is assembled afterwards".
//
// The "memory" clobbers are *compiler* barriers, distinct from the lfences.
// `lfence` is only an instruction; nothing about it stops the optimizer hoisting
// a load above it. Every one of these dispatches loads from a static global (the
// hash factors, the vptr table base, the method's slot), all loop-invariant and
// all prime candidates for hoisting out of the timed region.

struct tsc_reading {
    std::uint32_t lo;
    std::uint32_t hi;
};

inline auto tsc_start() -> tsc_reading {
    std::uint32_t lo, hi;
    asm volatile("lfence\n\trdtsc\n\tlfence"
                 : "=a"(lo), "=d"(hi)
                 :
                 : "memory");
    return {lo, hi};
}

inline auto tsc_stop() -> tsc_reading {
    std::uint32_t lo, hi;
    // rdtscp also writes the processor id to ecx.
    asm volatile("rdtscp\n\tlfence"
                 : "=a"(lo), "=d"(hi)
                 :
                 : "ecx", "memory");
    return {lo, hi};
}

inline auto elapsed(tsc_reading start, tsc_reading stop) -> std::uint64_t {
    auto t0 = (static_cast<std::uint64_t>(start.hi) << 32) | start.lo;
    auto t1 = (static_cast<std::uint64_t>(stop.hi) << 32) | stop.lo;
    return t1 - t0;
}

// Keep a value in a register and pretend it is both read and written, so the
// optimizer cannot constant-fold across it, hoist it out of the timed region,
// or devirtualize through it.
template<class T>
inline void clobber(T& value) {
    asm volatile("" : "+r"(value) : : "memory");
}

template<class T>
inline void consume(const T& value) {
    asm volatile("" : : "r"(value) : "memory");
}

// ---------------------------------------------------------------------------
// Cache state
// ---------------------------------------------------------------------------

enum class cache_mode { warm, clflush, sweep };

inline auto cache_mode_name(cache_mode m) -> const char* {
    switch (m) {
    case cache_mode::warm:
        return "warm";
    case cache_mode::clflush:
        return "clflush";
    case cache_mode::sweep:
        return "sweep";
    }
    return "?";
}

// Flush [p, p + size) from every cache level.
//
// clflushopt is weakly ordered with respect to other flushes; the caller
// issues one mfence after the last range (see scrubber::apply).
inline void flush_range(const void* p, std::size_t size) {
    if (p == nullptr) {
        return;
    }

    auto addr = reinterpret_cast<std::uintptr_t>(p);
    auto begin = addr & ~std::uintptr_t(63);
    auto end = addr + size;

    for (auto line = begin; line < end; line += 64) {
        _mm_clflushopt(reinterpret_cast<void*>(line));
    }
}

// A pointer/size pair to be flushed before each timed call.
struct region {
    const void* addr = nullptr;
    std::size_t size = 0;
};

// Evicts the caches before each timed dispatch.
//
// `sweep` writes one byte per 64-byte line over a buffer sized at 2x L3. Plain
// stores, not _mm_stream_*: non-temporal stores bypass the cache hierarchy and
// would evict nothing. Sequential rather than pointer-chasing because sweeping
// is bandwidth-bound (~1.6 ms for 64 MiB) whereas 1M dependent DRAM loads would
// cost ~80 ms per repetition and make the run intractable.
class scrubber {
  public:
    explicit scrubber(std::size_t sweep_bytes) : buffer_(sweep_bytes, 1) {
    }

    void apply(cache_mode mode, const region* regions, std::size_t count) {
        switch (mode) {
        case cache_mode::warm:
            return;

        case cache_mode::clflush:
            for (std::size_t i = 0; i != count; ++i) {
                flush_range(regions[i].addr, regions[i].size);
            }
            _mm_mfence();
            return;

        case cache_mode::sweep: {
            auto* p = buffer_.data();
            auto n = buffer_.size();

            for (std::size_t i = 0; i < n; i += 64) {
                p[i] = static_cast<unsigned char>(i);
            }

            // Defeat dead-store elimination over the whole loop.
            consume(p);
            _mm_mfence();
            return;
        }
        }
    }

    auto sweep_bytes() const -> std::size_t {
        return buffer_.size();
    }

  private:
    std::vector<unsigned char> buffer_;
};

// ---------------------------------------------------------------------------
// TSC calibration and CPU pinning
// ---------------------------------------------------------------------------

// Ticks per second, measured against CLOCK_MONOTONIC. The TSC on this part is
// invariant (constant_tsc, nonstop_tsc, tsc_reliable) but ticks at the nominal
// base frequency, so these are *reference* cycles, not core cycles.
inline auto calibrate_tsc_hz(double seconds = 0.2) -> double {
    timespec t0{}, t1{};

    clock_gettime(CLOCK_MONOTONIC, &t0);
    auto c0 = tsc_start();

    // int64 throughout: on -m32, `long` is 32 bits and a stall of ~2.15 s
    // during the spin (SIGSTOP, hypervisor descheduling) made the multiply
    // overflow -- UB, and an absurd calibrated frequency.
    const auto target = static_cast<std::int64_t>(seconds * 1e9);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        auto so_far = static_cast<std::int64_t>(t1.tv_sec - t0.tv_sec) *
                1000000000 +
            (t1.tv_nsec - t0.tv_nsec);

        if (so_far >= target) {
            break;
        }
    }

    auto c1 = tsc_stop();
    auto ns = static_cast<double>(
        static_cast<std::int64_t>(t1.tv_sec - t0.tv_sec) * 1000000000 +
        (t1.tv_nsec - t0.tv_nsec));

    return static_cast<double>(elapsed(c0, c1)) * 1e9 / ns;
}

inline auto pin_to_cpu(int cpu) -> bool {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct stats {
    std::uint64_t min = 0;
    std::uint64_t median = 0;
    std::uint64_t p90 = 0;
    double mean = 0;   // trimmed
    double stddev = 0; // of the trimmed set
    double se = 0;     // standard error of `mean`
    std::size_t samples = 0;
};

// The mean is trimmed of its top 5%: those samples are scheduler preemptions
// and interrupts, not dispatches, and a single one of them moves the mean by
// more than the quantity being measured.
//
// The mean, not the median, is the statistic to read. An lfence-bracketed rdtsc
// pair costs 25 or 50 cycles on this machine, bimodally (see README), so any
// single measurement is quantized far above the cost of a warm dispatch. The
// proportion of samples landing in each mode does shift with the work done, so
// the mean resolves well below one tick, while the median cannot.
inline auto summarize(std::vector<std::uint64_t> samples) -> stats {
    stats s;

    if (samples.empty()) {
        return s;
    }

    std::sort(samples.begin(), samples.end());

    s.samples = samples.size();
    s.min = samples.front();
    s.median = samples[samples.size() / 2];
    s.p90 = samples[(samples.size() * 90) / 100];

    auto keep = samples.size() - samples.size() / 20;

    if (keep == 0) {
        keep = samples.size();
    }

    auto total = std::accumulate(samples.begin(), samples.begin() + keep, 0.0);
    s.mean = total / static_cast<double>(keep);

    double sq = 0;

    for (std::size_t i = 0; i != keep; ++i) {
        auto d = static_cast<double>(samples[i]) - s.mean;
        sq += d * d;
    }

    s.stddev = std::sqrt(sq / static_cast<double>(keep));
    s.se = s.stddev / std::sqrt(static_cast<double>(keep));

    return s;
}

// Cost of the empty timed region: the lfence/rdtsc/lfence ... rdtscp/lfence
// bracket itself. Reported so the reader can see the noise floor.
inline auto measure_tsc_floor(std::size_t reps = 20000) -> stats {
    std::vector<std::uint64_t> samples;
    samples.reserve(reps);

    for (std::size_t i = 0; i != reps; ++i) {
        auto t0 = tsc_start();
        auto t1 = tsc_stop();
        samples.push_back(elapsed(t0, t1));
    }

    return summarize(std::move(samples));
}

} // namespace omb

#endif
