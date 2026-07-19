#ifndef DEVILS_ENGINE_ACT_INTENT_H
#define DEVILS_ENGINE_ACT_INTENT_H

#include <cstdint>

#include "common.h"
#include "devils_engine/utils/string_id.h" // utils::id

namespace devils_engine {
namespace act {
// intent — ШОВ thinker→ECS. GOAP/FSM/скрипт НЕ мутируют мир напрямую: они выдают
// компактный intent базовых действий, который ECS-системы консумят в ДЕТЕРМИНИРОВАННОЙ
// apply-фазе (sort by actor.id, не "кто первый прислал"). Один тип сериализации вместо
// миллионов разбросанных мутаций (lockstep INPUT-канал catalogue). source_action несёт
// PROVENANCE — "почему" (какой GOAP-action породил), не только "что".
//
// intent_kind РАСШИРЯЕТСЯ базовыми глаголами по мере надобности; набор — это контракт,
// который потом тяжело менять, поэтому растёт осознанно.

enum class intent_kind : uint16_t {
  none,
  move_to,       // payload.target — точка назначения
  turn_to,       // payload.target — направление/цель поворота
  call_function, // payload.call.fn — fn_id геймплейной функции (эффекта)
  fsm_event,     // payload.fsm.event — событие для mood-FSM актора
  spawn_prefab,  // payload.spawn — prefab id + уже разрешённая world-space позиция
};

struct intent {
  intent_kind kind = intent_kind::none;
  entity_id actor;
  union {
    vec3 target;
    struct {
      utils::id fn;
      entity_id target; // вторичный участник для эффектов-взаимодействий (едет в scope[1]); {} если не нужен
    } call;
    struct {
      utils::id event;
    } fsm;
    struct {
      utils::id prefab;
      vec3 target;
    } spawn;
  } payload = {};
  utils::id source_action = 0; // provenance: GOAP action, породивший интенцию
};

} // namespace act
} // namespace devils_engine

#endif
