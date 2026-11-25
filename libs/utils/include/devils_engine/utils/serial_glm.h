#ifndef DEVILS_ENGINE_UTILS_SERIAL_GLM_H
#define DEVILS_ENGINE_UTILS_SERIAL_GLM_H

#include <string_view>

#include <glm/glm.hpp>

#include "serialization.h"

// Канонические адаптеры кодека для float-векторов glm. Имена ("glm.vec2f"...) входят в layout-хеш и
// fingerprint схем (снимки ECS, компонентные образы сети): менять их — менять байты и отпечатки у
// всех потребителей. Значение пишется покомпонентно f32, без padding.

namespace devils_engine::utils::serial {

template <>
struct adapter<glm::vec2> {
  static constexpr std::string_view name = "glm.vec2f";
  static void write(writer& w, const glm::vec2& v) {
    w.f32(v.x);
    w.f32(v.y);
  }
  static void read(reader& r, glm::vec2& v) {
    v.x = r.f32();
    v.y = r.f32();
  }
};

template <>
struct adapter<glm::vec3> {
  static constexpr std::string_view name = "glm.vec3f";
  static void write(writer& w, const glm::vec3& v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
  }
  static void read(reader& r, glm::vec3& v) {
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
  }
};

template <>
struct adapter<glm::vec4> {
  static constexpr std::string_view name = "glm.vec4f";
  static void write(writer& w, const glm::vec4& v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
    w.f32(v.w);
  }
  static void read(reader& r, glm::vec4& v) {
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    v.w = r.f32();
  }
};

} // namespace devils_engine::utils::serial

#endif
