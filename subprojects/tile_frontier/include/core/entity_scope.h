#ifndef TILE_FRONTIER_CORE_ENTITY_SCOPE_H
#define TILE_FRONTIER_CORE_ENTITY_SCOPE_H

#include <cstdint>

#include <devils_engine/act/exec_context.h> // act::exec_context (.w, primary())
#include <devils_script/context.h>          // devils_script::context::set_arg

// Root-скоуп геймплейных скриптов devils_script для tile_frontier. Пара {world, entityid} по
// значению (≤16 байт, trivially destructible, с .valid()) — как handle<person> в примерах ds:
// нативка-аксессор (напр. hunger(entity_scope)) разыменовывает её и читает компонент из мира.
// Здесь же — seed: засев root-скоупа в ds::context из act::exec_context (reinterpret-seam над
// ctx.w, тот же, что world_of в actor_simulation.cpp). act остаётся ECS-agnostic — знание про
// aesthetics::world живёт в проекте.

namespace devils_engine { namespace aesthetics { class world; } }

namespace tile_frontier {
namespace core {

struct entity_scope {
  const devils_engine::aesthetics::world* w = nullptr;
  uint32_t id = UINT32_MAX;
  // Лениво: не трогаем world (валидность сущности проверяет сама нативка через get<T> -> nullptr).
  // Так предикат не КИДАЕТ на «мёртвом» скоупе, а ведёт себя как нативная версия (нет компонента -> 0).
  bool valid() const noexcept { return w != nullptr && id != UINT32_MAX; }
};

// seed_fn для act::script_function: set_arg(0, root) перед process(). Root = primary()-сущность
// текущего exec_context + указатель на мир (распакованный из непрозрачного act::world*).
inline void seed_entity_scope(const devils_engine::act::exec_context& ctx, devils_script::context* vm) {
  vm->set_arg(0, entity_scope{
    reinterpret_cast<const devils_engine::aesthetics::world*>(ctx.w),
    uint32_t(ctx.primary().id)
  });
}

}
}

#endif
