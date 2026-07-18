#ifndef DEVILS_ENGINE_ACUMEN_SYSTEM_H
#define DEVILS_ENGINE_ACUMEN_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "astar.h"
#include "cache.h"
#include "common.h"

namespace devils_engine {
namespace act {
class registry;
} // namespace act

namespace acumen {
// Входные параметры решения (свёрнуты из плоского списка аргументов decide). Только ВХОД —
// переиспользуемы между вызовами (выход out передаётся отдельным аргументом, он у каждого
// вызова свой). scratch ОБЯЗАТЕЛЕН (переиспользуемый A*-контейнер на поток исполнения). cache
// ОПЦИОНАЛЕН: nullptr ⇒ без мемоизации (всегда живой A*); иначе hit ⇒ копия плана, miss ⇒ поиск + insert.
struct decide_params {
  state start;                                     // стартовое состояние (значащие биты → ключ)
  scoped_state goal;                               // целевое состояние
  uint64_t goal_id = 0;                            // дешёвый идентификатор цели от вызывающего
  astar<astar_data>::container* scratch = nullptr; // обязателен
  solution_cache* cache = nullptr;                 // опционально (nullptr игнорируется)
};

class system {
public:
  // registry — общий реестр act: метрики резолвятся как предикаты, действия как эффекты
  // (по их name). Резолв одноразовый в конструкторе → горячий путь без lookup'ов.
  // НЕ noexcept: кидает utils::error на отсутствующей/не той категории функции (ошибка загрузки).
  system(const act::registry* registry, std::vector<state_metric> metrics, std::vector<goal> goals, std::vector<action> actions);
  ~system() noexcept = default;
  system(const system& copy) noexcept = default;
  system(system&& move) noexcept = default;
  system& operator=(const system& copy) noexcept = default;
  system& operator=(system&& move) noexcept = default;

  std::span<const state_metric> get_metrics() const noexcept;
  std::span<const goal> get_goals() const noexcept;
  std::span<const action> get_actions() const noexcept;

  state compute_state(const act::exec_context& ctx) const;

  // Объединение масок всех действий/целей — биты, способные повлиять на план. Бит вне
  // этой маски в ключ мемоизации не входит. Считается один раз в конструкторе.
  const state& relevant_mask() const noexcept;

  // Ключ мемоизации для (старт-состояние, тождество цели). goal_id — дешёвый идентификатор
  // цели от вызывающего (одинаковая цель ⇒ одинаковый id; разные цели ⇒ разные id).
  plan_key make_key(const state& start, uint64_t goal_id) const noexcept;

  // Тождество системы в ключе мемоизации: ОБЯЗАТЕЛЬНО уникально, когда один solution_cache
  // обслуживает несколько систем (per-entity мозги) — план хранит индексы действий ЭТОЙ системы.
  // registry::add ставит соль = system_id (хеш имени) автоматически; 0 (дефолт) корректен только
  // для единственной системы на кеш.
  void set_cache_salt(const uint64_t salt) noexcept {
    cache_salt_ = salt;
  }

  // Единственная точка входа решения. С cache: hit ⇒ копирует кешированный план; miss ⇒ живой
  // A* в params.scratch и кладёт в cache (если план ≤ max_plan). Без cache (params.cache == nullptr)
  // — просто живой A*. out — буфер вызывающего под план в порядке исполнения (напр. std::array на
  // стеке), отдельный аргумент (у каждого вызова свой). Возвращает ПОЛНУЮ длину плана; out
  // заполняется до min(out.size(), max_plan).
  size_t decide(const decide_params& params, std::span<const action*> out) const;

private:
  std::vector<state_metric> metrics;
  std::vector<goal> goals;
  std::vector<action> actions;
  state relevant_mask_;                 // ∪ масок действий/целей (значащие биты)
  std::vector<uint16_t> relevant_bits_; // индексы set-бит relevant_mask_ (для проекции ключа)
  uint64_t cache_salt_ = 0;             // тождество системы в plan_key (см. set_cache_salt)
};

class planner final : public astar<astar_data>::interface {
public:
  using float_t = astar<astar_data>::float_t;
  using container = astar<astar_data>::container;

  planner(const class system* system) noexcept;
  float_t neighbor_cost(const astar_data&, const astar_data&, const void*) const override;
  float_t goal_cost(const astar_data&, const astar_data&, const void*) const override;
  bool is_same(const astar_data&, const astar_data&, const void*) const override;
  void fill_successors(container*, const astar_data&, const void*) const override;

private:
  const class system* system;
};

} // namespace acumen
} // namespace devils_engine

#endif
