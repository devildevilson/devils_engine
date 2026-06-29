#ifndef DEVILS_ENGINE_ACUMEN_SYSTEM_H
#define DEVILS_ENGINE_ACUMEN_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <span>

#include "common.h"
#include "astar.h"

namespace devils_engine {
namespace act { class registry; } // резолв функций метрик/действий в конструкторе system

namespace acumen {
struct astar_data {
  scoped_state state;
  double weight;
  const struct action* action;
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
private:
  std::vector<state_metric> metrics;
  std::vector<goal> goals;
  std::vector<action> actions;
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

// Пишет план (действия цепочки решения в порядке исполнения) в `out` БЕЗ аллокаций — буфер даёт
// вызывающий (напр. std::array на стеке). Возвращает ПОЛНУЮ длину плана: если она > out.size(),
// в out лежит только префикс (план обрезан — увеличь буфер). 0 — решения нет ЛИБО start уже
// удовлетворяет цели (действий не нужно). План НЕ включает стартовый узел и ВКЛЮЧАЕТ цель.
size_t find_solution(const system* sys, astar<astar_data>::container* c, const state& start, const scoped_state& goal_state, std::span<const action*> out);

}
}

#endif