#ifndef DEVILS_ENGINE_UTILS_SHARED_H
#define DEVILS_ENGINE_UTILS_SHARED_H

#ifdef __cplusplus

#include <cstdint>
#include <cstddef>
#include <glm/glm.hpp>

#define INLINE inline
#define INOUT(type) type&

namespace devils_engine {
namespace utils {
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

#define DEVILS_ENGINE_GPU_UINT_MAX 0xffffffffu
#define DEVILS_ENGINE_GPU_INT_MAX  0x7fffffff

const uint INVALID_GPU_INDEX = DEVILS_ENGINE_GPU_UINT_MAX;

const uint UI_DRAW_SOLID     = 0u;
const uint UI_DRAW_MSDF      = 1u;
const uint UI_DRAW_IMAGE     = 2u;
const uint UI_DRAW_COMPOSITE = 3u;
const uint UI_DRAW_COOLDOWN  = 4u;
const uint UI_DRAW_MIX       = 5u;

const uint TEX_INDEX_BITS    = 14u;
const uint TEX_INDEX_MASK    = (1u << TEX_INDEX_BITS) - 1u;
const uint TEX_MIRROR_U      = 1u << 14u;
const uint TEX_MIRROR_V      = 1u << 15u;
const uint TEX_TYPE_SHIFT    = 16u;
const uint TEX_TYPE_MASK     = 0xFu;
const uint TEX_SAMPLER_SHIFT = 20u;
const uint TEX_SAMPLER_MASK  = 0xFu;

const uint SAMPLER_LINEAR    = 0u;
const uint SAMPLER_NEAREST   = 1u;

INLINE bool valid_gpu_index(const uint index) {
  return index != INVALID_GPU_INDEX;
}

INLINE uint tex_pack(const uint type, const uint index, const bool mirror_u, const bool mirror_v, const uint sampler_id) {
  return (index & TEX_INDEX_MASK)
    | (mirror_u ? TEX_MIRROR_U : 0u)
    | (mirror_v ? TEX_MIRROR_V : 0u)
    | ((type & TEX_TYPE_MASK) << TEX_TYPE_SHIFT)
    | ((sampler_id & TEX_SAMPLER_MASK) << TEX_SAMPLER_SHIFT);
}

INLINE uint tex_index_of(const uint id) {
  return id & TEX_INDEX_MASK;
}

INLINE uint tex_type_of(const uint id) {
  return (id >> TEX_TYPE_SHIFT) & TEX_TYPE_MASK;
}

INLINE uint tex_sampler_of(const uint id) {
  return (id >> TEX_SAMPLER_SHIFT) & TEX_SAMPLER_MASK;
}

INLINE bool tex_mirror_u_of(const uint id) {
  return (id & TEX_MIRROR_U) != 0u;
}

INLINE bool tex_mirror_v_of(const uint id) {
  return (id & TEX_MIRROR_V) != 0u;
}

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

INLINE float median3(const float r, const float g, const float b) {
  return max(min(r, g), min(max(r, g), b));
}

INLINE uint prng(const uint prev) {
  uint z = prev + 0x6D2B79F5u;
  z = (z ^ z >> 15) * (1u | z);
  z ^= z + (z ^ z >> 7) * (61u | z);
  return z ^ z >> 14;
}

INLINE uint rotl(const uint x, int k) {
  return (x << k) | (x >> (32 - k));
}

// по идее должно давать хорошие результаты для чисел не 0
INLINE uint prng2(const uint s0, const uint s1) {
  const uint s1_tmp = s1 ^ s0;
  const uint new_s0 = rotl(s0, 26) ^ s1_tmp ^ (s1_tmp << 9);
  return rotl(new_s0 * 0x9E3779BBu, 5) * 5u;
}

INLINE float prng_normalize(const uint state) {
  // float32 - 1 бит знак, 8 бит экспонента и 23 мантисса
  const uint float_mask = 0x7fu << 23;
  const uint float_val = float_mask | (state >> 9); // зачем двигать? первые несколько бит обладают плохой энтропией
  return uintBitsToFloat(float_val) - float(1.0);
}

// формула безье (2 порядок): vec = (1-t)*(1-t)*p1+2*(1-t)*t*p2+t*t*p3, где
// p1 - начало линии, p2 - контрольная точка, p3 - конец линии,
// t - переменная от 0 до 1 обозначающая участок линии безье
// нужно выбрать подходящую степень разбиения и нарисовать кривую
INLINE vec4 quadratic_bezier(const vec4 p1, const vec4 p2, const vec4 p3, const float t) {
  const float inv_t = float(1.0) - t;
  return inv_t*inv_t*p1 + float(2.0)*inv_t*t*p2 + t*t*p3;
}

// две контрольных точки, 0 <= t <= 1
INLINE vec4 cubic_bezier(const vec4 p1, const vec4 p2, const vec4 p3, const vec4 p4, const float t) {
  const float inv_t = float(1.0) - t;
  return inv_t*inv_t*inv_t*p1 + float(3.0)*inv_t*inv_t*t*p2 + float(3.0)*inv_t*t*t*p3 + t*t*t*p4;
}

#ifdef __cplusplus
}
}
}
#endif

#endif
