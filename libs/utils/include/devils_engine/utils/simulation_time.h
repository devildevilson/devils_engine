#ifndef DEVILS_ENGINE_UTILS_SIMULATION_TIME_H
#define DEVILS_ENGINE_UTILS_SIMULATION_TIME_H

#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace devils_engine::utils {

inline constexpr uint64_t microseconds_per_second = UINT64_C(1000000);

struct simulation_tick {
  uint64_t value = 0;
  constexpr auto operator<=>(const simulation_tick&) const noexcept = default;
};

struct simulation_duration {
  uint64_t ticks = 0;
  constexpr auto operator<=>(const simulation_duration&) const noexcept = default;
};

struct authored_duration {
  uint64_t microseconds = 0;
  constexpr auto operator<=>(const authored_duration&) const noexcept = default;
};

constexpr simulation_tick operator+(const simulation_tick tick,
                                    const simulation_duration duration) {
  if (duration.ticks > std::numeric_limits<uint64_t>::max() - tick.value) {
    throw std::overflow_error("simulation tick deadline overflow");
  }
  return {tick.value + duration.ticks};
}

// Session-wide fixed simulation rate. Authored durations stay in microseconds,
// but causal state stores only integer simulation ticks. Integral Hz keeps the
// portable conversion below independent of compiler-specific 128-bit types.
class simulation_rate {
public:
  constexpr explicit simulation_rate(const uint32_t ticks_per_second)
    : ticks_per_second_(ticks_per_second) {
    if (ticks_per_second == 0) {
      throw std::invalid_argument("simulation rate must be positive");
    }
  }

  constexpr uint32_t ticks_per_second() const noexcept {
    return ticks_per_second_;
  }

  // Deadlines use ceil: a positive authored duration never completes earlier
  // than requested. Zero remains the explicit immediate duration.
  constexpr simulation_duration to_ticks_ceil(const authored_duration duration) const {
    const uint64_t whole_seconds = duration.microseconds / microseconds_per_second;
    const uint64_t remainder = duration.microseconds % microseconds_per_second;
    if (whole_seconds > std::numeric_limits<uint64_t>::max() / ticks_per_second_) {
      throw std::overflow_error("authored duration does not fit simulation ticks");
    }

    const uint64_t whole_ticks = whole_seconds * ticks_per_second_;
    const uint64_t remainder_product = remainder * uint64_t(ticks_per_second_);
    const uint64_t partial_ticks =
      remainder_product / microseconds_per_second +
      uint64_t(remainder_product % microseconds_per_second != 0);
    if (partial_ticks > std::numeric_limits<uint64_t>::max() - whole_ticks) {
      throw std::overflow_error("authored duration does not fit simulation ticks");
    }
    return {whole_ticks + partial_ticks};
  }

  // Presentation/calendar code may derive nominal elapsed authored time from a
  // tick. Quotient/remainder avoids tick * 1'000'000 overflow in the common case.
  constexpr authored_duration to_microseconds_floor(const simulation_tick tick) const {
    const uint64_t whole_seconds = tick.value / ticks_per_second_;
    const uint64_t remainder = tick.value % ticks_per_second_;
    if (whole_seconds > std::numeric_limits<uint64_t>::max() / microseconds_per_second) {
      throw std::overflow_error("simulation tick does not fit authored microseconds");
    }
    const uint64_t whole_microseconds = whole_seconds * microseconds_per_second;
    const uint64_t partial_microseconds =
      (remainder * microseconds_per_second) / ticks_per_second_;
    if (partial_microseconds >
        std::numeric_limits<uint64_t>::max() - whole_microseconds) {
      throw std::overflow_error("simulation tick does not fit authored microseconds");
    }
    return {whole_microseconds + partial_microseconds};
  }

private:
  uint32_t ticks_per_second_;
};

constexpr simulation_tick deadline_after(const simulation_tick now,
                                         const authored_duration duration,
                                         const simulation_rate rate) {
  return now + rate.to_ticks_ceil(duration);
}

} // namespace devils_engine::utils

#endif
