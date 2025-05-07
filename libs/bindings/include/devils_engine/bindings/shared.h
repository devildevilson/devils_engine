#ifndef DEVILS_ENGINE_BINDINGS_SHARED_H
#define DEVILS_ENGINE_BINDINGS_SHARED_H

#include <cstdint>
#include <cstddef>
#include <glm/glm.hpp>


#ifdef __cplusplus

#define INLINE inline
#define INOUT(type) type&

namespace devils_engine {
namespace bindings {
namespace shared {
using uint = uint32_t;
using vec4 = glm::vec4;
using vec3 = glm::vec3;
using vec2 = glm::vec2;

using glm::floatBitsToUint;
using glm::uintBitsToFloat;
using glm::abs;
using glm::dot;
using glm::min;
using glm::max;
using glm::unpackUnorm4x8;
using glm::packUnorm4x8;

#else

#define INLINE
#define INOUT(type) inout type

#endif

#define DEVILS_ENGINE_GPU_UINT_MAX 0xffffffff
#define DEVILS_ENGINE_GPU_INT_MAX  0x7fffffff

struct color_t {
  uint container;
  
#ifdef __cplusplus
  inline color_t() : container(0) {}
  inline color_t(const uint container) : container(container) {}
#endif
};

INLINE color_t make_color(const float r, const float g, const float b, const float a) {
  return color_t(packUnorm4x8(vec4(r, g, b, a)));
}

INLINE color_t make_color(const vec4 col) {
  return color_t(packUnorm4x8(col));
}

INLINE vec4 get_color(const color_t col) {
  return unpackUnorm4x8(col.container);
}

INLINE uint prng(const uint prev) {
  uint z = prev + 0x6D2B79F5;
  z = (z ^ z >> 15) * (1 | z);
  z ^= z + (z ^ z >> 7) * (61 | z);
  return z ^ z >> 14;
}

INLINE uint rotl(const uint x, int k) {
  return (x << k) | (x >> (32 - k));
}

// по идее должно давать хорошие результаты для чисел не 0
INLINE uint prng2(const uint s0, const uint s1) {
  const uint s1_tmp = s1 ^ s0;
  const uint new_s0 = rotl(s0, 26) ^ s1_tmp ^ (s1_tmp << 9);
  return rotl(new_s0 * 0x9E3779BB, 5) * 5;
}

INLINE float prng_normalize(const uint state) {
  // float32 - 1 бит знак, 8 бит экспонента и 23 мантисса
  const uint float_mask = 0x7f << 23;
  const uint float_val = float_mask | (state >> 9); // зачем двигать? первые несколько бит обладают плохой энтропией
  return uintBitsToFloat(float_val) - 1.0f;
}

// формула безье (2 порядок): vec = (1-t)*(1-t)*p1+2*(1-t)*t*p2+t*t*p3, где
// p1 - начало линии, p2 - контрольная точка, p3 - конец линии,
// t - переменная от 0 до 1 обозначающая участок линии безье
// нужно выбрать подходящую степень разбиения и нарисовать кривую
INLINE vec4 quadratic_bezier(const vec4 p1, const vec4 p2, const vec4 p3, const float t) {
  const float inv_t = 1.0f - t;
  return inv_t*inv_t*p1 + 2*inv_t*t*p2 + t*t*p3;
}

// две контрольных точки, 0 <= t <= 1
INLINE vec4 cubic_bezier(const vec4 p1, const vec4 p2, const vec4 p3, const vec4 p4, const float t) {
  const float inv_t = 1.0f - t;
  return inv_t*inv_t*inv_t*p1 + 3*inv_t*inv_t*t*p2 + 3*inv_t*t*t*p3 + t*t*t*p4;
}

#ifdef __cplusplus
}
}
}
#endif

#endif