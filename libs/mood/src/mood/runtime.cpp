#include <algorithm>
#include <string_view>
#include <vector>

#include <gtl/phmap.hpp>

#include "devils_engine/utils/core.h" // utils::warn
#include "runtime.h"

namespace devils_engine {
namespace mood {

std::span<const system::transition> find_with_fallback(const system& sys, const utils::id state, const utils::id event) {
  auto sp = sys.find_transitions(state, event);
  if (sp.empty()) {
    sp = sys.find_transitions(conv::any_state, event); // общий переход из любого состояния
  }
  return sp;
}

step_outcome step(const system& sys, const utils::id state, const utils::id event, const act::exec_context& ctx) {
  step_outcome out;
  const auto candidates = find_with_fallback(sys, state, event);
  out.candidates = static_cast<uint16_t>(candidates.size());
  if (candidates.empty()) {
    out.result = step_result::no_transition;
    return out;
  }

  // top-down: первый кандидат, ВСЕ гварды которого прошли. Порядок гарантирован system'ом.
  for (const auto& t : candidates) {
    if (t.is_valid(ctx)) {
      out.result = step_result::transitioned;
      out.next_state = t.next_hash; // invalid_id для чистого эффекта (on_entry/on_exit/без '=') — остаёмся
      out.taken = &t;
      return out;
    }
    ++out.blocked;
  }

  out.result = step_result::blocked; // кандидаты были, но гварды никого не пустили
  return out;
}

step_outcome step(const system& sys, const std::string_view& state, const std::string_view& event, const act::exec_context& ctx) {
  return step(sys, utils::string_hash(state), utils::string_hash(event), ctx);
}

// прогнать эффекты ПЕРВОГО перехода в группе, чьи гварды все прошли (on_exit/on_entry — это
// тоже группы переходов, со своими гвардами). Если валидного нет — ничего не делаем.
static void run_first_valid(const std::span<const system::transition> group, const act::exec_context& ctx) {
  for (const auto& t : group) {
    if (t.is_valid(ctx)) {
      t.process(ctx);
      break;
    }
  }
}

utils::id apply_transition(const system& sys, const utils::id cur_state, const system::transition& taken, const act::exec_context& ctx) {
  const bool changes_state = (taken.next_hash != utils::invalid_id);

  // покидаем состояние только при внешнем переходе (внутренний '/ effects' без '=' состояние не меняет)
  if (changes_state) {
    run_first_valid(sys.find_transitions(cur_state, conv::on_exit), ctx);
  }

  taken.process(ctx); // эффекты самого перехода ('/ actions')

  if (changes_state) {
    run_first_valid(sys.find_transitions(taken.next_hash, conv::on_entry), ctx);
  }

  return taken.next_hash; // invalid_id ⇒ вызывающий оставляет cur_state
}

utils::id settle(const system& sys, const utils::id cur_state, const utils::id event, const act::exec_context& ctx, const uint32_t max_idle_iters) {
  utils::id state = cur_state;

  // 1) обработать пришедшее событие
  {
    const auto o = step(sys, state, event, ctx);
    if (o.result == step_result::transitioned && o.taken != nullptr) {
      const auto next = apply_transition(sys, state, *o.taken, ctx);
      if (next != utils::invalid_id) {
        state = next;
      }
    }
  }

  // 2) досчитать idle (completion-transitions) до стабильного состояния, но не более max_idle_iters.
  //    Останов: нет перехода / внутренний переход (состояние не меняется) / само-петля.
  for (uint32_t i = 0; i < max_idle_iters; ++i) {
    const auto o = step(sys, state, conv::idle, ctx);
    if (o.result != step_result::transitioned || o.taken == nullptr) {
      break;
    }
    const auto next = apply_transition(sys, state, *o.taken, ctx);
    if (next == utils::invalid_id || next == state) {
      break;
    }
    state = next;
  }

  return state;
}

// --- валидация графа ---

// расстояние Левенштейна (две строки), две бегущие строки DP. Гоняется только на загрузке.
static size_t levenshtein(const std::string_view& a, const std::string_view& b) {
  const size_t n = a.size(), m = b.size();
  if (n == 0) {
    return m;
  }
  if (m == 0) {
    return n;
  }
  std::vector<size_t> prev(m + 1), cur(m + 1);
  for (size_t j = 0; j <= m; ++j) {
    prev[j] = j;
  }
  for (size_t i = 1; i <= n; ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= m; ++j) {
      const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, cur);
  }
  return prev[m];
}

// ближайший по правке кандидат из набора имён (для "did you mean"). "" если ничего близкого.
static std::string_view nearest(const std::string_view& name, const std::vector<std::string_view>& pool) {
  std::string_view best;
  size_t best_dist = SIZE_MAX;
  for (const auto& cand : pool) {
    if (cand == name) {
      continue;
    }
    const size_t d = levenshtein(name, cand);
    if (d < best_dist) {
      best_dist = d;
      best = cand;
    }
  }
  // подсказываем только если правда близко (опечатка), а не любое ближайшее.
  const size_t threshold = std::max<size_t>(2, name.size() / 3);
  return (best_dist != SIZE_MAX && best_dist <= threshold) ? best : std::string_view();
}

void validate(const system& sys) {
  const auto all = sys.transitions();

  // hash -> имя (для сообщений), множества "имеет исходящие" и "является целью".
  gtl::flat_hash_map<utils::id, std::string_view> name_of;
  gtl::flat_hash_set<utils::id> has_outgoing;   // встречается как current_state
  gtl::flat_hash_set<utils::id> is_target;      // встречается как next_state
  std::vector<std::string_view> outgoing_names; // пул имён-кандидатов (состояния с исходящими)
  std::vector<std::string_view> target_names;   // пул имён-кандидатов (целевые состояния)

  for (const auto& t : all) {
    name_of.emplace(t.current_hash, t.current_state);
    if (has_outgoing.emplace(t.current_hash).second) {
      outgoing_names.push_back(t.current_state);
    }
    if (t.next_hash != utils::invalid_id) {
      name_of.emplace(t.next_hash, t.next_state);
      if (is_target.emplace(t.next_hash).second) {
        target_names.push_back(t.next_state);
      }
    }
  }

  // тупиковые: цель перехода, у которой нет ни одного исходящего перехода. Может быть
  // легитимным терминальным состоянием — поэтому warning, а не error, + подсказка.
  for (const auto& target : is_target) {
    if (has_outgoing.contains(target)) {
      continue;
    }
    const std::string_view name = name_of[target];
    const auto did_you_mean = nearest(name, outgoing_names);
    if (!did_you_mean.empty()) {
      utils::warn("mood: state '{}' is a transition target but has no outgoing transitions (terminal state or typo? did you mean '{}'?)", name, did_you_mean);
    } else {
      utils::warn("mood: state '{}' is a transition target but has no outgoing transitions (terminal state?)", name);
    }
  }

  // недостижимые: состояние с исходящими переходами, которое никогда не является целью.
  // any_state — wildcard-конвенция, целью не бывает по определению, его пропускаем.
  for (const auto& src : has_outgoing) {
    if (src == conv::any_state) {
      continue;
    }
    if (is_target.contains(src)) {
      continue;
    }
    const std::string_view name = name_of[src];
    const auto did_you_mean = nearest(name, target_names);
    if (!did_you_mean.empty()) {
      utils::warn("mood: state '{}' is never reached as a transition target (initial state or typo? did you mean '{}'?)", name, did_you_mean);
    } else {
      utils::warn("mood: state '{}' is never reached as a transition target (initial state?)", name);
    }
  }
}

} // namespace mood
} // namespace devils_engine
