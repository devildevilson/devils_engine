#ifndef TILE_FRONTIER_CORE_ACTOR_SNAPSHOT_H
#define TILE_FRONTIER_CORE_ACTOR_SNAPSHOT_H

// Включение снапшотов для боевых компонентов tile_frontier:
//  - адаптеры для НЕ-агрегатных glm-типов (aesthetics не зависит от glm),
//  - регистрация всех ECS-компонентов в реестр сериализации.
// rgba8_color/entityid_t сериализуются сами (плоский агрегат / uint32).

#include <glm/glm.hpp>

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/aesthetics/sink.h>

#include "core/actor_simulation.h" // определения компонентов

// --- адаптеры glm (ЛИСТ: canon = name, сериализация = write/read; reflect их не трогает) ---
namespace devils_engine {
namespace aesthetics {
namespace serial {

template <> struct adapter<glm::vec2> {
  static constexpr std::string_view name = "glm.vec2f";
  static void write(writer& w, const glm::vec2& v) { w.f32(v.x); w.f32(v.y); }
  static void read(reader& r, glm::vec2& v) { v.x = r.f32(); v.y = r.f32(); }
};
template <> struct adapter<glm::vec3> {
  static constexpr std::string_view name = "glm.vec3f";
  static void write(writer& w, const glm::vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
  static void read(reader& r, glm::vec3& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); }
};
template <> struct adapter<glm::vec4> {
  static constexpr std::string_view name = "glm.vec4f";
  static void write(writer& w, const glm::vec4& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); w.f32(v.w); }
  static void read(reader& r, glm::vec4& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); v.w = r.f32(); }
};

} // namespace serial
} // namespace aesthetics
} // namespace devils_engine

// --- регистрация компонентов (позиционно, без имён на проводе; схему стережёт fingerprint) ---
using namespace tile_frontier::core;

SERIALIZABLE_COMPONENT(actor_position)
SERIALIZABLE_COMPONENT(actor_velocity)
SERIALIZABLE_COMPONENT(actor_brain)
SERIALIZABLE_COMPONENT(actor_visual)
SERIALIZABLE_COMPONENT(actor_perception)
SERIALIZABLE_COMPONENT(actor_cognition)
SERIALIZABLE_COMPONENT(stats)
SERIALIZABLE_COMPONENT(actor_state)
SERIALIZABLE_COMPONENT(actor_eating)
SERIALIZABLE_COMPONENT(actor_grabbed)
SERIALIZABLE_COMPONENT(food_item)
SERIALIZABLE_COMPONENT(obstacle)

#endif
