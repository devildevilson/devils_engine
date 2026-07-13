#ifndef DEVILS_ENGINE_ACT_EXEC_CONTEXT_H
#define DEVILS_ENGINE_ACT_EXEC_CONTEXT_H

#include <cstdint>
#include "devils_engine/utils/prng.h" // utils::mix (counter-based)
#include "common.h"

namespace devils_engine {
namespace act {

struct effect_sink; // effect_sink.h
class world;        // forward: aesthetics::world или его срез
struct execution_scratch;

// exec_context — ИММУТАБЕЛЬНЫЙ игровой контекст вызова. Свои поля не меняются между
// вызовами; течёт по ССЫЛКЕ в invoke, НИКОГДА не глобал (глобал = молчаливые гонки под
// актор-моделью). Mutable VM/call state лежит отдельно в per-worker execution_scratch.
//
// PRNG-СОСТОЯНИЕ ВНУТРИ НЕ ДЕРЖИМ: immutable-входы (seed/entity/tick) дают внешние
// системы (текущий кадр/тик + entity + сид), а каждый вызов задаёт `purpose` ЯВНО.
// Это детерминированнее авто-счётчика обращений: не зависит ни от порядка, ни от числа
// draw'ов — добавил/убрал один draw, остальные не сдвинулись.
struct exec_context {
  static constexpr uint32_t max_scope = 8;

  // scope: фикс-стек сущностей без аллокаций. [0]=this, [1]=target, [2]=контейнер...
  entity_id scope[max_scope];
  uint32_t  scope_count = 0;

  const world* w = nullptr; // только чтение; мутация мира — кодом эффекта через sink

  // immutable-входы RNG (из внешних систем). НЕТ мутируемого счётчика.
  uint64_t rng_seed = 0, rng_entity = 0, rng_tick = 0;
  uint64_t random(const uint64_t purpose) const noexcept {
    return utils::mix(rng_seed, rng_entity, rng_tick, purpose);
  }

  effect_sink* sink = nullptr; // nullptr ⇒ dry-run
  bool dry_run() const noexcept { return sink == nullptr; }

  execution_scratch* scratch = nullptr;

  entity_id primary()   const noexcept { return scope_count > 0 ? scope[0] : entity_id{}; }
  entity_id secondary() const noexcept { return scope_count > 1 ? scope[1] : entity_id{}; }
};

}
}

#endif
