#pragma once

#include <cstdint>
#include <atomic>
#include <ctime>
#include <x86intrin.h>

#include <hw/utility/Format.hpp>

namespace hw::utility {

using Timestamp = int64_t;  // Nanoseconds since Epoch
using CPUCycles = int64_t;

class SystemClockTSC {
  struct CalibrationData {
    std::atomic<uint64_t> seq{0};
    double nsPerCycle{0.0};
    CPUCycles baseTsc{0};
    Timestamp baseNs{0};
  };

public:
  SystemClockTSC() {
    calibrate();
  }

  /**
   * Public TSC Access: useful for cycles-based latency measurements.
   * __rdtscp is a serializing instruction.
   */
  static inline CPUCycles tsc() noexcept {
    unsigned int aux;
    return static_cast<CPUCycles>(__rdtscp(&aux));
  }

  /**
   * Hot-path: High-speed timestamp acquisition.
   * Complexity: ~10ns.
   */
  inline Timestamp now() const noexcept {
    uint64_t s1, s2;
    double factor;
    CPUCycles bTsc;
    Timestamp bNs;

    do {
      // acquire fence ensures the data reads (factor, bTsc, bNs)
      // are not hoisted above the sequence check.
      s1 = _data.seq.load(std::memory_order_acquire);

      if (s1 & 1) [[unlikely]] {
          continue;
      }

      factor = _data.nsPerCycle;
      bTsc = _data.baseTsc;
      bNs = _data.baseNs;

      // acquire fence ensures the data reads are not reordered
      // below the final sequence check.
      s2 = _data.seq.load(std::memory_order_acquire);
    } while (s1 != s2);

    return bNs + static_cast<Timestamp>(static_cast<double>(tsc() - bTsc) * factor);
  }

  /**
   * Background path: Calibration.
   * To be called by a dedicated "slow" thread (e.g., every 100ms).
   */
  void calibrate() noexcept {
    uint64_t s = _data.seq.load(std::memory_order_relaxed);
    _data.seq.store(s + 1, std::memory_order_release); // Start Write (Odd)

    // 1. Anchor to Realtime (Wall Clock)
    Timestamp anchorNs = now_(CLOCK_REALTIME);
    CPUCycles anchorTsc = tsc();

    // 2. Measure elapsed hardware time using Monotonic Raw (pure frequency)
    Timestamp startMono = now_(CLOCK_MONOTONIC_RAW);
    CPUCycles startTsc = anchorTsc;

    // Wait 10ms for a stable frequency sample
    Timestamp target = startMono + 10'000'000;
    while(now_(CLOCK_MONOTONIC_RAW) < target) {
        _mm_pause(); // Hint to CPU that we are in a busy loop
    }

    Timestamp endMono = now_(CLOCK_MONOTONIC_RAW);
    CPUCycles endTsc = tsc();

    // Prevent division by zero if rdtsc is broken or too fast
    if (endTsc > startTsc) [[likely]] {
        _data.nsPerCycle = static_cast<double>(endMono - startMono) / static_cast<double>(endTsc - startTsc);
    }

    _data.baseTsc = anchorTsc;
    _data.baseNs = anchorNs;

    _data.seq.store(s + 2, std::memory_order_release); // End Write (Even)
  }

private:
  static inline Timestamp now_(clockid_t clk_id) noexcept {
    timespec ts;
    clock_gettime(clk_id, &ts);
    return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
  }

  alignas(64) CalibrationData _data;
};

}
