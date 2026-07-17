#ifndef DEVILS_ENGINE_SIMUL_INTERACTION_COMMIT_H
#define DEVILS_ENGINE_SIMUL_INTERACTION_COMMIT_H

#include <cstdint>

#include "devils_engine/act/exec_context.h"
#include "devils_engine/act/function.h"     // act::effect_function, invoke
#include "devils_engine/act/interaction.h"  // act::interaction (дескриптор)
#include "devils_engine/act/registry.h"     // effect(), interaction_of()
#include "devils_engine/aesthetics/common.h"            // entityid_t
#include "devils_engine/aesthetics/interaction_arena.h" // won()
#include "devils_engine/catalogue/call_log.h"           // call_log, call_record

// commit_calls — ОБОБЩЁННАЯ commit-фаза deferred-call механизма взаимодействий (ROADMAP п.16). Движковое
// generic-ядро: исполнить записанные вызовы (call_log) в детерминированном порядке и на каждый —
//   1) резолвить эффект по fn_id в act::registry (нет эффекта → пропуск);
//   2) если эффект тегирован взаимодействием (дескриптор), поставить цель в scope[target_scope] и
//      ГЕЙТить вызов через interaction_arena.won (победитель elect + «intent бьёт grab»); прочие эффекты
//      применяются безусловно;
//   3) вызвать эффект у прошедших гейт.
// Проектная специфика — через ХУКИ (движок не знает ни про компоненты, ни про FSM/звук проекта):
//   skip(rec, id) -> bool          : пропустить вызов ЦЕЛИКОМ (напр. схваченную сущность), до эффекта и after;
//   make_ctx(rec, id) -> exec_context : собрать контекст (проект владеет seed/scratch/world-seam);
//   after(rec, id, const ctx&, ran): пост-обработка (FSM-переход, звук…), вызывается и когда эффект не
//                                     применился (ran==false) — напр. FSM всё равно получает событие.
// Так движок владеет dispatch + дескриптор-гейтом + invoke, а проект — своей спецификой. Порядок dispatch
// детерминирован (индекс инициатора), фаза однопоточная (commit) — cross-entity мутации безопасны.

namespace devils_engine {
namespace simul {

template <typename Skip, typename MakeCtx, typename After>
void commit_calls(const catalogue::call_log& log, const act::registry& registry,
                  const aesthetics::interaction_arena& arena,
                  Skip&& skip, MakeCtx&& make_ctx, After&& after) {
  log.dispatch([&](const catalogue::call_record& rec) {
    const auto id = aesthetics::entityid_t(rec.primary);
    if (skip(rec, id)) {
      return;
    }
    const act::effect_function* effect = registry.effect(rec.fn);
    if (effect == nullptr) {
      return;
    }
    act::exec_context ctx = make_ctx(rec, id);
    bool ran = true;
    if (const act::interaction* desc = registry.interaction_of(rec.fn); desc != nullptr) {
      ctx.scope[desc->target_scope] = act::entity_id{rec.target};
      ctx.scope_count = uint32_t(desc->target_scope) + 1; // цель = последний участник scope
      ran = arena.won(rec.fn, aesthetics::entityid_t(rec.target), id);
    }
    if (ran) {
      effect->invoke(ctx);
    }
    after(rec, id, ctx, ran);
  });
}

} // namespace simul
} // namespace devils_engine

#endif
