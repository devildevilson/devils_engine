#include "devils_engine/originator/common.h"

#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>

namespace devils_engine {
namespace originator {

namespace {
struct base_traits {
  std::string_view name;
  size_t component_byte_size;
  field_kind::values kind;
};

constexpr base_traits base_table[field_base::count] = {
#define X(name_, bytes_, kind_) {#name_, bytes_, field_kind::kind_},
  DEVILS_ENGINE_ORIGINATOR_FIELD_BASE_LIST
#undef X
};

constexpr std::string_view kind_names[field_kind::count] = {"floating", "unsigned_integer", "signed_integer", "normalized"};

constexpr std::string_view aperture_names[aperture::count] = {"pointwise", "gather", "scatter", "reduce", "sequential"};
constexpr std::string_view binding_names[binding_mode::count] = {"read", "write"};

// f16 <-> f32. Своя реализация, а не зависимость: нужны ровно две функции, обе на уровне битов.
float half_to_float(const uint16_t half) noexcept {
  const uint32_t sign = uint32_t(half & 0x8000u) << 16;
  const uint32_t exponent = (half >> 10) & 0x1fu;
  const uint32_t mantissa = half & 0x3ffu;

  if (exponent == 0) {
    if (mantissa == 0) {
      return std::bit_cast<float>(sign);
    }
    // Субнормаль: нормализуем сдвигом, компенсируя экспоненту.
    uint32_t shift = 0;
    uint32_t m = mantissa;
    while ((m & 0x400u) == 0) {
      m <<= 1;
      ++shift;
    }
    m &= 0x3ffu;
    const uint32_t e = 127 - 15 - shift + 1;
    return std::bit_cast<float>(sign | (e << 23) | (m << 13));
  }

  if (exponent == 0x1fu) {
    return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
  }

  return std::bit_cast<float>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
}

uint16_t float_to_half(const float value) noexcept {
  const uint32_t bits = std::bit_cast<uint32_t>(value);
  const uint16_t sign = uint16_t((bits >> 16) & 0x8000u);
  const int32_t exponent = int32_t((bits >> 23) & 0xffu) - 127 + 15;
  const uint32_t mantissa = bits & 0x7fffffu;

  if (((bits >> 23) & 0xffu) == 0xffu) {
    return uint16_t(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
  }

  if (exponent >= 0x1f) {
    return uint16_t(sign | 0x7c00u); // переполнение -> бесконечность
  }

  if (exponent <= 0) {
    if (exponent < -10) {
      return sign; // слишком мало даже для субнормали
    }
    const uint32_t m = (mantissa | 0x800000u) >> uint32_t(1 - exponent + 13);
    return uint16_t(sign | m);
  }

  // Округление к ближайшему чётному по отбрасываемым 13 битам.
  const uint32_t rounded = mantissa + 0x0fffu + ((mantissa >> 13) & 1u);
  if ((rounded & 0x800000u) != 0) {
    return uint16_t(sign | uint16_t((exponent + 1) << 10));
  }
  return uint16_t(sign | uint16_t(exponent << 10) | uint16_t(rounded >> 13));
}
} // namespace

bool field_type::valid() const noexcept {
  return base < field_base::count && components >= 1 && components <= max_field_components;
}

size_t field_type::component_byte_size() const noexcept {
  return base < field_base::count ? base_table[base].component_byte_size : 0;
}

size_t field_type::byte_size() const noexcept {
  return component_byte_size() * components;
}

size_t field_type::alignment() const noexcept {
  return component_byte_size();
}

field_kind::values field_type::kind() const noexcept {
  return base < field_base::count ? base_table[base].kind : field_kind::count;
}

field_type parse_field_type(const std::string_view& str) noexcept {
  // Буквы -> род, цифры -> число компонент. Ровно как parse_layout в painter.
  size_t split = 0;
  while (split < str.size() && (std::isalpha(static_cast<unsigned char>(str[split])) != 0)) {
    ++split;
  }

  if (split == 0 || split == str.size()) {
    return field_type{};
  }

  const auto letters = str.substr(0, split);
  const auto digits = str.substr(split);

  uint32_t components = 0;
  for (const char c : digits) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
      return field_type{};
    }
    components = components * 10 + uint32_t(c - '0');
    if (components > max_field_components) {
      return field_type{};
    }
  }

  for (size_t i = 0; i < field_base::count; ++i) {
    if (base_table[i].name == letters) {
      return field_type{field_base::values(i), components};
    }
  }

  return field_type{};
}

std::string_view to_string(const field_base::values base) noexcept {
  return base < field_base::count ? base_table[base].name : std::string_view("invalid");
}

std::string_view to_string(const field_kind::values kind) noexcept {
  return kind < field_kind::count ? kind_names[kind] : std::string_view("invalid");
}

std::string_view to_string(const aperture::values value) noexcept {
  return value < aperture::count ? aperture_names[value] : std::string_view("invalid");
}

std::string_view to_string(const binding_mode::values value) noexcept {
  return value < binding_mode::count ? binding_names[value] : std::string_view("invalid");
}

bool is_parallel(const aperture::values value) noexcept {
  return value == aperture::pointwise || value == aperture::gather || value == aperture::reduce;
}

double load_component(const void* ptr, const field_base::values base) noexcept {
  switch (base) {
    case field_base::v: {
      float value = 0.0f;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value);
    }
    case field_base::sf: {
      uint16_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(half_to_float(value));
    }
    case field_base::ui: {
      uint32_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value);
    }
    case field_base::us: {
      uint16_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value);
    }
    case field_base::ub: {
      uint8_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value);
    }
    case field_base::i: {
      int32_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value);
    }
    case field_base::is: {
      int16_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value);
    }
    case field_base::ib: {
      int8_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value);
    }
    case field_base::c: {
      uint8_t value = 0;
      std::memcpy(&value, ptr, sizeof(value));
      return double(value) / 255.0;
    }
    default: return 0.0;
  }
}

void store_component(void* ptr, const field_base::values base, const double value) noexcept {
  switch (base) {
    case field_base::v: {
      const float v = float(value);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::sf: {
      const uint16_t v = float_to_half(float(value));
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::ui: {
      const double clamped = value < 0.0 ? 0.0 : (value > 4294967295.0 ? 4294967295.0 : value);
      const uint32_t v = uint32_t(clamped);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::us: {
      const double clamped = value < 0.0 ? 0.0 : (value > 65535.0 ? 65535.0 : value);
      const uint16_t v = uint16_t(clamped);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::ub: {
      const double clamped = value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value);
      const uint8_t v = uint8_t(clamped);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::i: {
      const double clamped = value < -2147483648.0 ? -2147483648.0 : (value > 2147483647.0 ? 2147483647.0 : value);
      const int32_t v = int32_t(clamped);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::is: {
      const double clamped = value < -32768.0 ? -32768.0 : (value > 32767.0 ? 32767.0 : value);
      const int16_t v = int16_t(clamped);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::ib: {
      const double clamped = value < -128.0 ? -128.0 : (value > 127.0 ? 127.0 : value);
      const int8_t v = int8_t(clamped);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    case field_base::c: {
      const double clamped = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
      const uint8_t v = uint8_t(clamped * 255.0 + 0.5);
      std::memcpy(ptr, &v, sizeof(v));
      return;
    }
    default: return;
  }
}

} // namespace originator
} // namespace devils_engine
