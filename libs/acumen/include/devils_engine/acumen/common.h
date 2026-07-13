#ifndef DEVILS_ENGINE_ACUMEN_COMMON_H
#define DEVILS_ENGINE_ACUMEN_COMMON_H

#include <cstddef>
#include <cstdint>
#include <bitset>
#include <string>

#include "devils_engine/act/function.h"      // act::predicate_function / act::effect_function
#include "devils_engine/act/exec_context.h"  // act::exec_context

#ifndef DEVILS_ENGINE_ACUMEN_STATE_SIZE
#  define DEVILS_ENGINE_ACUMEN_STATE_SIZE 256
#endif

#define DEVILS_ENGINE_ACUMEN_EPSILON 0.0000001

// Максимальная длина кешируемого плана (мемоизация). Пока константа в хедере; при нужде
// навесим define сборщика. Планы длиннее НЕ кешируются (decide вернёт полную длину, но в
// out отдаст не больше max_plan действий — для таких зови find_solution напрямую).
#ifndef DEVILS_ENGINE_ACUMEN_MAX_PLAN
#  define DEVILS_ENGINE_ACUMEN_MAX_PLAN 8
#endif

namespace devils_engine {
namespace acumen {
// динамический сет?
using state = std::bitset<DEVILS_ENGINE_ACUMEN_STATE_SIZE>;

inline constexpr size_t max_plan = DEVILS_ENGINE_ACUMEN_MAX_PLAN;
// число 64-битных слов под проекцию значащих бит состояния в ключ мемоизации.
inline constexpr size_t state_words = (DEVILS_ENGINE_ACUMEN_STATE_SIZE + 63) / 64;

// метрика состояния = один бит GOAP-стейта = ПРЕДИКАТ над сущностью из общего реестра act.
// `name` — имя предиката в act::registry; `compute_func` РЕЗОЛВИТ system при сборке (lookup
// уходит из горячего пути compute_state). Предикаты чистые — планировщик A* зовёт их свободно.
// Бит в `state` для метрики — это её ИНДЕКС в массиве метрик системы (плотная нумерация;
// compute_state ставит s[i] = metrics[i].compute(ctx)).
struct state_metric {
  std::string name;
  double weight = 1.0; // вес наверное нужен только для того чтобы сортировать goal
  const act::predicate_function* compute_func = nullptr; // резолвится system'ом из act::registry по name

  state_metric() noexcept;
  state_metric(std::string name) noexcept;
  state_metric(std::string name, const double weight) noexcept;
  bool compute(const act::exec_context& ctx) const;
};

struct scoped_state {
  state handle;
  state mask;

  scoped_state() noexcept = default;
  scoped_state(const state& handle) noexcept;
  scoped_state(const state& handle, const state& mask) noexcept;
  void set(const size_t& index, const bool val) noexcept;
  void unset(const size_t& index) noexcept;
  bool get(const size_t& index) const noexcept;
  state compute() const noexcept;

  bool check(const state& s) const noexcept;
  state apply(const state& s) const noexcept;
  std::tuple<size_t, size_t> count_similars(const state& s) const noexcept;
  double compute_variance_norm(const state& s) const noexcept;
};

struct goal {
  std::string name;
  scoped_state requirements; // validity of the state
  scoped_state goal;         //  цель достигнута если гоал - часть стейта
};

struct action {
  std::string name;
  scoped_state requirements; // must be valid thru action? bold statement
  scoped_state next_state;   // action successfully end when next_state becomes part of the state
  scoped_state weight_state; // priority
  // эффект действия из общего реестра act, РЕЗОЛВИТСЯ system по `name` (пустое name => нет
  // эффекта, чисто символический переход). ВАЖНО: в планировании (A*) НЕ исполняется — план
  // лишь выбирает действия; эффект применяется отдельной apply-фазой (через intent).
  const act::effect_function* effect = nullptr;

  action() noexcept;
  action(std::string name, scoped_state requirements, scoped_state next_state, scoped_state weight_state) noexcept;
  double compute_weight(const state& current_state) const noexcept;
};
}
}

#endif
