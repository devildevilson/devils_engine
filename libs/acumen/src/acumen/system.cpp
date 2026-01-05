#include "system.h"

#if __cplusplus == 202306L
const char* abc = "sfasfsaddsada";
#endif

namespace devils_engine {
namespace acumen {
state_metric::state_metric() noexcept : weight(1.0) {}
state_metric::state_metric(std::string name_, func_t f_) noexcept : name(std::move(name_)), weight(1.0), compute_func(std::move(f_)) {}
state_metric::state_metric(std::string name_, const double weight_, func_t f_) noexcept : name(std::move(name_)), weight(weight_), compute_func(std::move(f_)) {}
bool state_metric::compute(const void* p) const { return compute_func ? compute_func(p) : false; }

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
action::action(std::string name_, scoped_state requirements_, scoped_state next_state_, scoped_state weight_state_, func_t f_) noexcept :
  name(std::move(name_)), requirements(std::move(requirements_)), next_state(std::move(next_state_)), weight_state(std::move(weight_state_)), action_func(std::move(f_))
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

system::system(std::vector<state_metric> metrics_, std::vector<goal> goals_, std::vector<action> actions_) noexcept : metrics(std::move(metrics_)), goals(std::move(goals_)), actions(std::move(actions_)) {}

std::span<const state_metric> system::get_metrics() const noexcept { return std::span(metrics); }
std::span<const goal> system::get_goals() const noexcept { return std::span(goals); }
std::span<const action> system::get_actions() const noexcept { return std::span(actions); }

// да было бы неплохо придумать быстрый расчет метрик
state system::compute_state(const void* p) const {
  state s;
  for (size_t i = 0; i < metrics.size(); ++i) {
    s.set(i, metrics[i].compute(p));
  }
  return s;
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

std::vector<const action*> find_solution(const system* sys, astar<astar_data>::container* c, const state& start, const scoped_state& goal_state) {
  planner p(sys);
  astar<astar_data>::algorithm a(c, &p, astar_data{ scoped_state(start), 0.0, nullptr }, astar_data{ goal_state, 0.0, nullptr }, nullptr);

  auto state = astar<astar_data>::state::searching;
  while (state == astar<astar_data>::state::searching) {
    state = a.step();
  }

  if (state != astar<astar_data>::state::succeeded) { return std::vector<const action*>(); }

  if (p.is_same(a.solution_raw()->data, a.goal_node()->data, nullptr)) return { a.solution_raw()->data.action };

  std::vector<const action*> arr;
  for (auto node = a.solution_raw(); node != a.goal_node(); node = node->child) {
    arr.push_back(node->data.action);
  }

  return arr;
}

}
}