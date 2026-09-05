#ifndef DEVILS_ENGINE_UTILS_FLOAT_BITS_H
#define DEVILS_ENGINE_UTILS_FLOAT_BITS_H

#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace devils_engine::utils {

// Use fieldwise, never on a padded aggregate. Preserves signed zero and NaN
// payload identity; normalization, if wanted, belongs before serialization.
struct float_bits_equal {
  template <std::floating_point T>
    requires(sizeof(T) == 4 || sizeof(T) == 8) && std::numeric_limits<T>::is_iec559
  constexpr bool operator()(const T a, const T b) const noexcept {
    using bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    return std::bit_cast<bits>(a) == std::bit_cast<bits>(b);
  }
};

} // namespace devils_engine::utils

#endif
