#include "system.h"

#include <array>
#include <algorithm>
#include <cassert>

#include "devils_engine/utils/core.h"      // utils::error
#include "devils_engine/utils/string_id.h" // utils::string_hash
#include "devils_engine/act/registry.h"    // act::registry — резолв предикатов/эффектов

#if __cplusplus == 202306L
const char* abc = "sfasfsaddsada";
#endif

namespace devils_engine {
namespace acumen {
// Низкоуровневый alloc-free A*-примитив: пишет план (действия цепочки в порядке исполнения) в
// out, возвращает ПОЛНУЮ длину (> out.size() ⇒ в out лежит префикс). 0 — решения нет ЛИБО start
// уже удовлетворяет цели. План НЕ включает стартовый узел и ВКЛЮЧАЕТ цель. Внутренний (file-local):
// единственная публичная точка входа — system::decide (мемоизация + cap), он зовёт его на miss/без кеша.
static size_t find_solution(const system* sys, astar<astar_data>::container* c, const state& start, const scoped_state& goal_state, std::span<const action*> out);

state_metric::state_metric() noexcept : weight(1.0) {}
state_metric::state_metric(std::string name_) noexcept : name(std::move(name_)), weight(1.0) {}
state_metric::state_metric(std::string name_, const double weight_) noexcept : name(std::move(name_)), weight(weight_) {}
bool state_metric::compute(const act::exec_context& ctx) const { return compute_func ? compute_func->invoke(ctx) : false; }

scoped_state::scoped_state(const state& handle) noexcept : handle(handle) { mask.flip(); }
scoped_state::scoped_state(const state& handle, const state& mask) noexcept : handle(handle), mask(mask) {}
void scoped_state::set(const size_t& index, const bool val) noexcept { mask.set(index, true); handle.set(index, val); }
void scoped_state::unset(const size_t& index) noexcept { mask.set(index, false); }
bool scoped_state::get(const size_t& index) const noexcept { return mask.test(index) && handle.test(index); }
state scoped_state::compute() const noexcept { return mask & handle; }

bool scoped_state::check(const state& s) const noexcept { return (s & mask) == compute(); }
state scoped_state::apply(const state& s) const noexcept { return (s & (~mask)) | compute(); }
std::tuple<size_t, size_t> scoped_state::count_similars(const state& s) const noexcept {
  size_t count = 0;
  size_t equals = 0; // double
  for (size_t i = 0; i < s.size(); ++i) {
    const size_t k = size_t(mask.test(i));
    count += k * 1;
    equals += k * size_t(handle.test(i) == s.test(i));
  }

  return std::make_tuple(equals, count);
}

double scoped_state::compute_variance_norm(const state& s) const noexcept {
  const auto [equals, count] = count_similars(s);
  return count == 0 ? 0.0 : (1.0 - double(equals) / double(count));
}

action::action() noexcept {}
action::action(std::string name_, scoped_state requirements_, scoped_state next_state_, scoped_state weight_state_) noexcept :
  name(std::move(name_)), requirements(std::move(requirements_)), next_state(std::move(next_state_)), weight_state(std::move(weight_state_))
{
}

double action::compute_weight(const state& current_state) const noexcept {
  if (weight_state.mask.none()) return 0.5;

  size_t count = 0;
  size_t pos = 0;
  for (size_t i = 0; i < weight_state.handle.size(); ++i) {
    const size_t k = size_t(weight_state.mask.test(i));
    count += k * 1;
    pos += k * size_t(weight_state.handle.test(i) == current_state.test(i));
  }

  return count == 0 ? 0.5 : (1.0 - (double(pos) / double(count)));
}

system::system(const act::registry* registry, std::vector<state_metric> metrics_, std::vector<goal> goals_, std::vector<action> actions_)
  : metrics(std::move(metrics_)), goals(std::move(goals_)), actions(std::move(actions_))
{
  // резолв функций общего реестра act по именам (как в mood) — кэшируем типизированные
  // указатели здесь, на фазе сборки, чтобы lookup ушёл из горячего цикла A*.
  for (auto& m : metrics) {
    const auto* fn = registry->predicate(utils::string_hash(m.name));
    if (fn != nullptr) m.compute_func = fn;
    else utils::error{}("acumen: state_metric '{}' has no matching predicate function in act::registry", m.name);
  }

  for (auto& a : actions) {
    if (a.name.empty()) continue; // действие без эффекта — чисто символический переход состояния
    const auto* fn = registry->effect(utils::string_hash(a.name));
    if (fn != nullptr) a.effect = fn;
    else utils::error{}("acumen: action '{}' has no matching effect function in act::registry", a.name);
  }

  // значащие биты = всё, что влияет на план: маски требований/эффектов/весов действий +
  // маски требований/целей целей. Бит вне этого объединения на решение не влияет → в ключ
  // мемоизации не входит (см. cache.h). Считаем один раз здесь.
  relevant_mask_.reset();
  for (const auto& a : actions) {
    relevant_mask_ |= a.requirements.mask;
    relevant_mask_ |= a.next_state.mask;
    relevant_mask_ |= a.weight_state.mask;
  }
  for (const auto& g : goals) {
    relevant_mask_ |= g.requirements.mask;
    relevant_mask_ |= g.goal.mask;
  }
  relevant_bits_.clear();
  for (size_t i = 0; i < relevant_mask_.size(); ++i) {
    if (relevant_mask_.test(i)) relevant_bits_.push_back(uint16_t(i));
  }
}

std::span<const state_metric> system::get_metrics() const noexcept { return std::span(metrics); }
std::span<const goal> system::get_goals() const noexcept { return std::span(goals); }
std::span<const action> system::get_actions() const noexcept { return std::span(actions); }

// да было бы неплохо придумать быстрый расчет метрик
state system::compute_state(const act::exec_context& ctx) const {
  state s;
  for (size_t i = 0; i < metrics.size(); ++i) {
    s.set(i, metrics[i].compute(ctx));
  }
  return s;
}

const state& system::relevant_mask() const noexcept { return relevant_mask_; }

plan_key system::make_key(const state& start, const uint64_t goal_id) const noexcept {
  // проецируем значащие биты в плотные младшие позиции — ключ зависит лишь от того, что
  // влияет на план. relevant_bits_.size() <= STATE_SIZE ⇒ b/64 < state_words (без OOB).
  plan_key k;
  k.goal_id = goal_id;
  size_t b = 0;
  for (const uint16_t idx : relevant_bits_) {
    if (start.test(idx)) k.bits[b >> 6] |= (uint64_t(1) << (b & 63u));
    ++b;
  }
  return k;
}

size_t system::decide(const decide_params& params, std::span<const action*> out) const {
  assert(params.scratch != nullptr && "acumen::decide: scratch (A* container) is required");
  // контроль соундности кеша: биты цели должны покрываться значащей маской, иначе ключ их
  // не учтёт и кеш вернёт неверный план. Дёшево гасим в дебаге (в релизе цель фиксирована).
  assert((params.goal.mask & ~relevant_mask_).none() &&
         "acumen::decide: goal mask has bits outside relevant_mask (cache key would be unsound)");

  const action* const base = get_actions().data();
  const bool use_cache = params.cache != nullptr;

  // hit (только с кешем): ключ зависит лишь от значащих бит старта + тождества цели.
  plan_key key;
  if (use_cache) {
    key = make_key(params.start, params.goal_id);
    if (const cached_plan* hit = params.cache->find(key)) {
      const size_t n = std::min(size_t(hit->length), out.size());
      for (size_t i = 0; i < n; ++i) out[i] = base + hit->actions[i];
      return hit->full_length;
    }
  }

  // miss / без кеша: живой поиск в локальный буфер (cap = max_plan), оттуда — и в кеш, и в out.
  std::array<const action*, max_plan> tmp{};
  const size_t full = find_solution(this, params.scratch, params.start, params.goal, std::span<const action*>(tmp));

  const size_t copy = std::min({ full, out.size(), max_plan });
  for (size_t i = 0; i < copy; ++i) out[i] = tmp[i];

  if (use_cache && full <= max_plan) { // длиннее cap не представимо — не кешируем (редкий случай)
    cached_plan plan;
    plan.length = uint8_t(full);
    plan.full_length = uint8_t(std::min<size_t>(full, 255));
    for (size_t i = 0; i < full; ++i) plan.actions[i] = uint16_t(tmp[i] - base);
    params.cache->insert(key, plan);
  }
  return full;
}

planner::planner(const class system* system) noexcept : system(system) {}

planner::float_t planner::neighbor_cost(const astar_data& a, const astar_data& b, const void*) const {
  return b.state.compute_variance_norm(a.state.compute()) + b.weight;
}

// как это расчитать? важно чтобы вес не переплюнул число тут... может просто на 10 умножить?
// мы скорее всего сможем тем или иным образом свести числа к нормализованному числу
// например вес действия это уже от 0 до 1
// варианс тоже по идее должен быть таковым или нет?
// по идее с такими функциями как сейчас neighbor_cost никогда не будет больше 2

planner::float_t planner::goal_cost(const astar_data& a, const astar_data& b, const void*) const {
  const auto [equals, count] = b.state.count_similars(a.state.compute());
  //return (1.0 - double(equals) / double(count)) * 2.0;
  return (count - equals) * 2.0;
}

bool planner::is_same(const astar_data& a, const astar_data& b, const void*) const {
  return b.state.check(a.state.compute());
}

// на счет стоимости действий https://gamedev.stackexchange.com/questions/45321/estimating-costs-in-a-goap-system
void planner::fill_successors(container* c, const astar_data& a, const void*) const {
  const auto state_for_check = a.state.compute();

  for (const auto& action : system->get_actions()) {
    if (!action.requirements.check(state_for_check)) continue;

    const double w = action.compute_weight(state_for_check);
    if (w < DEVILS_ENGINE_ACUMEN_EPSILON) continue;
    const auto state_for_next = action.next_state.apply(state_for_check);
    c->add_successor(astar_data{ state_for_next, w, &action });
  }
}

static size_t find_solution(const system* sys, astar<astar_data>::container* c, const state& start, const scoped_state& goal_state, std::span<const action*> out) {
  planner p(sys);
  astar<astar_data>::algorithm a(c, &p, astar_data{ scoped_state(start), 0.0, nullptr }, astar_data{ goal_state, 0.0, nullptr }, nullptr);

  auto state = astar<astar_data>::state::searching;
  while (state == astar<astar_data>::state::searching) {
    state = a.step();
  }

  if (state != astar<astar_data>::state::succeeded) { return 0; }

  // План — действия узлов цепочки решения в порядке исполнения. Стартовый узел действия НЕ несёт
  // (его data.action == nullptr, это лишь исходное состояние), поэтому идём со start->child и
  // ВКЛЮЧАЕМ goal-узел — в нём лежит действие, достигшее цели. Пишем в out до его размера, но
  // считаем ПОЛНУЮ длину (вызывающий увидит обрезание по count > out.size()). Если start уже
  // удовлетворяет цели, start->child == nullptr -> count 0 (пустой план).
  size_t count = 0;
  for (auto node = a.solution_raw()->child; node != nullptr; node = node->child) {
    if (count < out.size()) out[count] = node->data.action;
    ++count;
    if (node == a.goal_node()) break;
  }

  // Узлы решения (start..goal) после успеха ещё висят в node_pool (free_unused чистит лишь
  // тупиковые из open/closed). Возвращаем их в пул — блоки остаются, контейнер ПЕРЕИСПОЛЬЗУЕМ
  // для следующего поиска без перевыделения (один container на поток исполнения). out уже
  // скопирован, а data.action указывает в system->actions, не в узлы — освобождение безопасно.
  // (Путь неудачи сюда не доходит — free_all отработал в step(); см. ранний return выше.)
  a.free_solution();
  return count;
}

}
}
