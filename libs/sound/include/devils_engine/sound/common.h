#ifndef DEVILS_ENGINE_SOUND_COMMON_H
#define DEVILS_ENGINE_SOUND_COMMON_H

#include <cstdint>
#include <cstddef>
#include <climits>

namespace devils_engine {
namespace sound {

enum class format {
  unknown, u8, s16, s24, s32, f32
};

constexpr format bits_per_sample_to_format(const size_t bits, const bool isfloat = true) noexcept {
  switch (bits) {
    case 8 : return format::u8;
    case 16: return format::s16;
    case 24: return format::s24;
    case 32: return isfloat ? format::f32 : format::s32;
  }

  return format::unknown;
}

constexpr size_t format_to_bytes(const enum format format) noexcept {
  size_t sample_size = 0;
  switch (format) {
    case format::f32: sample_size = sizeof(float); break;
    case format::s16: sample_size = sizeof(int16_t); break;
    case format::s24: sample_size = sizeof(int16_t) + sizeof(int8_t); break;
    case format::s32: sample_size = sizeof(int32_t); break;
    case format::u8 : sample_size = sizeof(uint8_t); break;
    case format::unknown: break;
  }

  return sample_size;
}

constexpr size_t format_to_bits(const enum format format) noexcept {
  return format_to_bytes(format) * CHAR_BIT;
}

constexpr size_t pcm_samples_to_bytes(const size_t sample_rate, const uint32_t channels, const enum format format) noexcept {
  return sample_rate * channels * format_to_bytes(format);
}

constexpr size_t pcm_frame_to_bytes(const uint32_t channels, const enum format format) noexcept {
  return channels * format_to_bytes(format);
}

constexpr double bytes_to_seconds(const size_t bytes, const size_t sample_rate, const uint32_t channels, const enum format format) noexcept {
  return double(bytes) / double(pcm_samples_to_bytes(sample_rate, channels, format));
}

constexpr size_t seconds_to_bytes(const double seconds, const size_t sample_rate, const uint32_t channels, const enum format format) noexcept {
  return seconds * pcm_samples_to_bytes(sample_rate, channels, format);
}

constexpr size_t second_to_pcm_samples(const size_t sample_rate, const uint32_t channels) noexcept {
  return sample_rate * channels;
}

constexpr size_t bytes_to_pcm_frames(const size_t bytes, const uint32_t channels, const enum format format) noexcept {
  return bytes / pcm_frame_to_bytes(channels, format);
}

struct vec3 {
  float x,y,z;

  vec3() noexcept;
  vec3(const float x, const float y, const float z) noexcept;

  // ???
};

vec3 operator-(const vec3 &a, const vec3 &b) noexcept;
float dot2(const vec3 &a, const vec3 &b) noexcept;
float distance2(const vec3 &a, const vec3 &b) noexcept;
vec3 normalize(const vec3 &a) noexcept;

enum class type {
  music,
  talk,
  talk_pos,
  ui_effect,
  sfx,

  count
};

double compute_base_priority(const enum type type) noexcept; // [0,1]

#define SOUND_SYSTEM_EXTENSION_LIST \
  X(mp3)  \
  X(flac) \
  X(wav)  \
  X(ogg)  \
  X(pcm)  \

enum class data_type {
#define X(name) name,
  SOUND_SYSTEM_EXTENSION_LIST
#undef X

  undefined
};

static std::string_view type_to_string(const sound::data_type type);

}
}

#endif