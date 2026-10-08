#ifndef FRONTIER_ONLINE_CORE_ACTOR_SNAPSHOT_H
#define FRONTIER_ONLINE_CORE_ACTOR_SNAPSHOT_H

// Включение снапшотов для боевых компонентов frontier_online:
//  - регистрация всех ECS-компонентов в реестр сериализации.
// rgba8_color/entityid_t сериализуются сами (плоский агрегат / uint32).

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/aesthetics/sink.h>

#include "core/actor_simulation.h" // определения компонентов
#include "core/glm_serial.h"

// --- регистрация компонентов (позиционно, без имён на проводе; схему стережёт fingerprint) ---
using namespace frontier_online::core;

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
