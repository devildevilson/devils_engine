#ifndef DEVILS_ENGINE_ORIGINATOR_VALUE_NOISE_H
#define DEVILS_ENGINE_ORIGINATOR_VALUE_NOISE_H

#include <array>
#include <cmath>
#include <cstdint>

#include "devils_engine/utils/prng.h"

namespace devils_engine::originator {

// Непрерывный 3D value noise с ключом мирового узла решётки. Период и смещение seed —
// параметры словаря форм, поэтому разные миры используют один и тот же общий алгоритм.
inline double value_noise(const uint64_t seed, const std::array<double, 3>& p, const double period) {
  const double x = p[0] / period, y = p[1] / period, z = p[2] / period;
  const int64_t ix = int64_t(std::floor(x)), iy = int64_t(std::floor(y)), iz = int64_t(std::floor(z));
  const auto smooth = [](const double t) { return t * t * (3.0 - 2.0 * t); };
  const double fx = smooth(x - double(ix)), fy = smooth(y - double(iy)), fz = smooth(z - double(iz));
  const auto sample = [&](const int64_t dx, const int64_t dy, const int64_t dz) {
    const auto hash = utils::mix(seed, uint64_t(ix + dx), uint64_t(iy + dy), uint64_t(iz + dz));
    return 2.0 * utils::prng_normalize(hash) - 1.0;
  };
  const auto lerp = [](const double a, const double b, const double t) { return a + (b - a) * t; };
  const double bottom = lerp(lerp(sample(0, 0, 0), sample(1, 0, 0), fx),
                             lerp(sample(0, 1, 0), sample(1, 1, 0), fx), fy);
  const double top = lerp(lerp(sample(0, 0, 1), sample(1, 0, 1), fx),
                          lerp(sample(0, 1, 1), sample(1, 1, 1), fx), fy);
  return lerp(bottom, top, fz);
}

} // namespace devils_engine::originator

#endif
