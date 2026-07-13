#ifndef DEVILS_ENGINE_UTILS_SHARED_H
#define DEVILS_ENGINE_UTILS_SHARED_H

#ifdef __cplusplus

#  include <cstddef>
#  include <cstdint>

#  include <glm/glm.hpp>

#  define INLINE inline
#  define INOUT(type) type&

namespace devils_engine {
namespace utils {
namespace shared {
using uint = uint32_t;
using vec4 = glm::vec4;
using vec3 = glm::vec3;
using vec2 = glm::vec2;

using glm::abs;
using glm::dot;
using glm::floatBitsToUint;
using glm::max;
using glm::min;
using glm::packUnorm4x8;
using glm::uintBitsToFloat;
using glm::unpackUnorm4x8;

#else

#  define INLINE
#  define INOUT(type) inout type

#endif

#define DEVILS_ENGINE_GPU_UINT_MAX 0xffffffffu
#define DEVILS_ENGINE_GPU_INT_MAX 0x7fffffff

const uint invalid_gpu_index = DEVILS_ENGINE_GPU_UINT_MAX;

const uint ui_draw_solid = 0u;
const uint ui_draw_msdf = 1u;
const uint ui_draw_image = 2u;
const uint ui_draw_composite = 3u;
const uint ui_draw_cooldown = 4u;
const uint ui_draw_mix = 5u;

const uint tex_index_bits = 14u;
const uint tex_index_mask = (1u << tex_index_bits) - 1u;
const uint tex_mirror_u = 1u << 14u;
const uint tex_mirror_v = 1u << 15u;
const uint tex_type_shift = 16u;
const uint tex_type_mask = 0xFu;
const uint tex_sampler_shift = 20u;
const uint tex_sampler_mask = 0xFu;

const uint sampler_linear = 0u;
const uint sampler_nearest = 1u;

INLINE bool valid_gpu_index(const uint index) {
  return index != invalid_gpu_index;
}

INLINE uint tex_pack(const uint type, const uint index, const bool mirror_u, const bool mirror_v, const uint sampler_id) {
  return (index & tex_index_mask) | (mirror_u ? tex_mirror_u : 0u) | (mirror_v ? tex_mirror_v : 0u) | ((type & tex_type_mask) << tex_type_shift) | ((sampler_id & tex_sampler_mask) << tex_sampler_shift);
}

INLINE uint tex_index_of(const uint id) {
  return id & tex_index_mask;
}

INLINE uint tex_type_of(const uint id) {
  return (id >> tex_type_shift) & tex_type_mask;
}

INLINE uint tex_sampler_of(const uint id) {
  return (id >> tex_sampler_shift) & tex_sampler_mask;
}

INLINE bool tex_mirror_u_of(const uint id) {
  return (id & tex_mirror_u) != 0u;
}

INLINE bool tex_mirror_v_of(const uint id) {
  return (id & tex_mirror_v) != 0u;
}

struct color_t {
  uint container;

#ifdef __cplusplus
  color_t() : container(0) {}
  color_t(const uint container) : container(container) {}
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
  return inv_t * inv_t * p1 + float(2.0) * inv_t * t * p2 + t * t * p3;
}

// две контрольных точки, 0 <= t <= 1
INLINE vec4 cubic_bezier(const vec4 p1, const vec4 p2, const vec4 p3, const vec4 p4, const float t) {
  const float inv_t = float(1.0) - t;
  return inv_t * inv_t * inv_t * p1 + float(3.0) * inv_t * inv_t * t * p2 + float(3.0) * inv_t * t * t * p3 + t * t * t * p4;
}

#ifdef __cplusplus
}
}
}
#endif

#endif
