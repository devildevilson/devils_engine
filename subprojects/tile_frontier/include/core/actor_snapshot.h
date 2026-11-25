#ifndef TILE_FRONTIER_CORE_ACTOR_SNAPSHOT_H
#define TILE_FRONTIER_CORE_ACTOR_SNAPSHOT_H

// Включение снапшотов для боевых компонентов tile_frontier:
//  - адаптеры для НЕ-агрегатных glm-типов — движковые (utils/serial_glm.h; aesthetics не зависит от glm),
//  - регистрация всех ECS-компонентов в реестр сериализации.
// rgba8_color/entityid_t сериализуются сами (плоский агрегат / uint32).

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/aesthetics/sink.h>
#include <devils_engine/utils/serial_glm.h>
#include <glm/glm.hpp>

#include "core/actor_simulation.h" // определения компонентов

// --- регистрация компонентов (позиционно, без имён на проводе; схему стережёт fingerprint) ---
using namespace tile_frontier::core;

SERIALIZABLE_COMPONENT(actor_position)
SERIALIZABLE_COMPONENT(spawn_point)
SERIALIZABLE_COMPONENT(player_controller)
SERIALIZABLE_COMPONENT(actor_velocity)
SERIALIZABLE_COMPONENT(actor_brain)
SERIALIZABLE_COMPONENT(actor_visual)
SERIALIZABLE_COMPONENT(actor_perception)
SERIALIZABLE_COMPONENT(actor_cognition)
SERIALIZABLE_COMPONENT(stats)
SERIALIZABLE_COMPONENT(actor_state)
SERIALIZABLE_COMPONENT(goap_ref)
SERIALIZABLE_COMPONENT(fsm_ref)
SERIALIZABLE_COMPONENT(actor_eating)
SERIALIZABLE_COMPONENT(actor_grabbed)
SERIALIZABLE_COMPONENT(flag_set)
SERIALIZABLE_COMPONENT(food_item)
SERIALIZABLE_COMPONENT(obstacle)

#endif
