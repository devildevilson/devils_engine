#include "system.h"

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/string-utils.hpp"
#include "devils_engine/utils/string_id.h" // utils::string_hash для резолва имён в act::registry
#include "devils_engine/utils/prng.h"      // utils::mix — ключ хеш-индекса (state,event)
#include "devils_engine/utils/patterns.h"

#include <algorithm>
#include <cctype>
#include <array>

namespace devils_engine {
namespace mood {

int32_t system::transition::is_valid(const act::exec_context& ctx) const {
  // guard = act::predicate_function (bool). Канала ошибки <0 больше нет — ошибки внутри
  // функции кидаются через utils::error самим бэкендом, наружу выходит чистый предикат.
  int32_t ret = 1;
  for (size_t i = 0; i < guards.size() && !guards[i].empty() && bool(ret); ++i) {
    const bool r = guards_ptr[i]->invoke(ctx);
    ret = int32_t(bool(ret) && r);
  }
  return ret;
}

int32_t system::transition::process(const act::exec_context& ctx) const {
  // action = act::effect_function (void): эффект мутирует мир только через ctx (world/sink).
  for (size_t i = 0; i < actions.size() && !actions[i].empty(); ++i) {
    actions_ptr[i]->invoke(ctx);
  }

  return 0;
}

static constexpr bool is_valid_char(const char c) { 
  return (c >= '0' && c <= '9') || 
         (c >= 'a' && c >= 'z') || 
         (c >= 'A' && c >= 'Z') || 
         (c == '_'); 
}

static bool is_ident_start(const char c) {
  return std::isalpha(c) || c == '_';
}

static bool is_ident_char(const char c) {
  return std::isalnum(c) || c == '_';
}

enum class token_type {
  identifier,
  plus,       // +
  lbracket,   // [
  rbracket,   // ]
  slash,      // /
  equals,     // =
  comma,      // ,
  end
};

struct token {
  token_type type;
  std::string_view token;
};

static size_t parse_line_to_tokens(const std::string_view& str, token* tokens, const size_t max_tokens) {
  size_t count = 0;
  size_t i = 0;
  while (i < str.size()) {
    if (count >= max_tokens) return SIZE_MAX;

    if (utils::string::is_whitespace(str[i])) { ++i; continue; }

    if (is_ident_start(str[i])) {
      const size_t start = i;
      while (i < str.size() && is_ident_char(str[i])) { ++i; }

      tokens[count] = { token_type::identifier, str.substr(start, i - start) };
      count += 1;
      continue;
    }

    switch (str[i]) {
      case '+': tokens[count] = { token_type::plus, std::string_view(&str[i], 1) }; count += 1; break;
      case '[': tokens[count] = { token_type::lbracket, std::string_view(&str[i], 1) }; count += 1; break;
      case ']': tokens[count] = { token_type::rbracket, std::string_view(&str[i], 1) }; count += 1; break;
      case '/': tokens[count] = { token_type::slash, std::string_view(&str[i], 1) }; count += 1; break;
      case '=': tokens[count] = { token_type::equals, std::string_view(&str[i], 1) }; count += 1; break;
      case ',': tokens[count] = { token_type::comma, std::string_view(&str[i], 1) }; count += 1; break;
      default: return SIZE_MAX;
    }

    ++i;
  }

  tokens[count] = { token_type::end, std::string_view() }; count += 1;

  return count;
}

static system::transition parse_line(const std::string_view &line) {
  system::transition t;
  t.full_line = utils::string::trim(line);

  auto cur_line = t.full_line;

  //utils::println(cur_line);

  std::array<token, 64> token_arr;
  const size_t token_count = parse_line_to_tokens(cur_line, token_arr.data(), token_arr.size());
  if (token_count == SIZE_MAX) utils::error{}("Could not parse line '{}': unexpected symbol", cur_line);

  /*for (size_t i = 0; i < token_count; ++i) {
    utils::print(token_arr[i].token);
  }

  utils::println();*/

  struct token_next_struct {
    std::span<const token> tokens;
    size_t current_token;
    token_next_struct(const token* tokens, const size_t max_tokens) noexcept : tokens(tokens, max_tokens), current_token(0) {}
    token next() { const size_t index = current_token; current_token += 1; return tokens[index]; }
  };

  token_next_struct tok(token_arr.data(), token_count);

  auto cur = tok.next();

  if (cur.type != token_type::identifier) {
    utils::error{}("Could not parse line '{}': expected identifer as first token", cur_line);
  }

  t.current_state = cur.token;

  cur = tok.next();
  if (cur.type == token_type::plus) {
    cur = tok.next();
    if (cur.type != token_type::identifier) { utils::error{}("Could not parse line '{}': expected identifer after plus sign", cur_line); }
    t.event = cur.token;
    cur = tok.next();
  }

  if (cur.type == token_type::lbracket) {
    cur = tok.next();
    size_t guards_size = 0;
    while (cur.type != token_type::rbracket && cur.type != token_type::end) {
      if (cur.type != token_type::identifier) { utils::error{}("Could not parse line '{}': expected identifer within brackets", cur_line); }
      t.guards[guards_size] = cur.token;
      guards_size += 1;
      cur = tok.next();
      if (cur.type == token_type::comma) cur = tok.next();
    }

    if (cur.type == token_type::rbracket) cur = tok.next();
  }

  if (cur.type == token_type::slash) {
    cur = tok.next();
    size_t actions_count = 0;
    while (cur.type != token_type::equals && cur.type != token_type::end) {
      if (cur.type != token_type::identifier) { utils::error{}("Could not parse line '{}': expected identifer after slash", cur_line); }
      t.actions[actions_count] = cur.token;
      actions_count += 1;
      cur = tok.next();
      if (cur.type == token_type::comma) cur = tok.next();
    }
  }
  
  if (cur.type == token_type::equals) {
    cur = tok.next();
    if (cur.type != token_type::identifier) { utils::error{}("Could not parse line '{}': expected identifer after equals", cur_line); }
    t.next_state = cur.token;
    cur = tok.next();
  }

  if (cur.type != token_type::end) {
    utils::error{}("Could not parse line '{}': expected end of sequence", cur_line);
  }

  /*utils::print(t.current_state, "+", t.event, "[");
  for (size_t i = 0; i < t.guards.size() && !t.guards[i].empty(); ++i) { utils::print(t.guards[i], ", "); }
  utils::print("]/");
  for (size_t i = 0; i < t.actions.size() && !t.actions[i].empty(); ++i) { utils::print(t.actions[i], ", "); }
  utils::println("=", t.next_state);*/


  //size_t count = 0;
  //std::array<std::string_view, 4> parts;

  //const size_t plus_sign = cur_line.find('+');
  //if (plus_sign != std::string_view::npos) {
  //  parts[count] = utils::string::trim(cur_line.substr(0, plus_sign));
  //  count += 1;
  //  cur_line = utils::string::trim(cur_line.substr(plus_sign + 1));
  //}

  //const size_t div_sign = cur_line.find('/');
  //if (div_sign != std::string_view::npos) {
  //  parts[count] = utils::string::trim(cur_line.substr(0, div_sign));
  //  count += 1;
  //  cur_line = utils::string::trim(cur_line.substr(div_sign + 1));
  //}

  //const size_t eq_sign = cur_line.find('=');
  //if (eq_sign != std::string_view::npos) {
  //  parts[count] = utils::string::trim(cur_line.substr(0, eq_sign));
  //  count += 1;
  //  cur_line = utils::string::trim(cur_line.substr(eq_sign + 1));
  //}

  //parts[count] = cur_line;
  //count += 1;

  //t.current_state = parts[0];
  //count = 1;
  //if (plus_sign != std::string_view::npos) {
  //  const auto event_guard = parts[count];
  //  count += 1;

  //  const size_t bracket = event_guard.find('[');
  //  if (bracket != std::string_view::npos) {
  //    t.event = utils::string::trim(event_guard.substr(0, bracket));
  //    const size_t bracket_end = event_guard.find(']');
  //    if (bracket_end == std::string_view::npos) utils::error{}("Wrong bracketing in line '{}'", line);
  //    //const auto guards = utils::string::trim(event_guard.substr(bracket+1, bracket_end-bracket-1));
  //    const auto guards = utils::string::trim(utils::string::inside(event_guard, "[", "]"));
  //    auto sp = std::span(t.guards.data(), t.guards.size());
  //    const size_t count = utils::string::split(guards, ",", sp);
  //    if (count == SIZE_MAX) utils::error{}("Too much guards found while processing '{}' in line '{}'", guards, line);
  //    for (size_t i = 0; i < count; ++i) {
  //      sp[i] = utils::string::trim(sp[i]);
  //    }
  //  }
  //  else {
  //    t.event = event_guard;
  //  }
  //}

  //if (div_sign != std::string_view::npos) {
  //  const auto actions = parts[count];
  //  count += 1;

  //  auto sp = std::span(t.actions.data(), t.actions.size());
  //  const size_t count = utils::string::split(actions, ",", sp);
  //  if (count == SIZE_MAX) utils::error{}("Too much actions found while processing '{}' in line '{}'", actions, line);
  //  for (size_t i = 0; i < count; ++i) {
  //    sp[i] = utils::string::trim(sp[i]);
  //  }
  //}

  //if (eq_sign != std::string_view::npos) {
  //  t.next_state = parts[count];
  //  count += 1;
  //}

  // !std::all_of(t.current_state.begin(), t.current_state.end(), &is_valid_char)
  if (!std::all_of(t.current_state.begin(), t.current_state.end(), &is_valid_char)) {
    utils::error{}("Invalid current state string '{}' in line '{}'", t.current_state, line);
  }

  //!std::all_of(t.next_state.begin(), t.next_state.end(), &is_valid_char)
  if (!std::all_of(t.next_state.begin(), t.next_state.end(), &is_valid_char)) {
    utils::error{}("Invalid next state string '{}' in line '{}'", t.next_state, line);
  }

  //!std::all_of(t.event.begin(), t.event.end(), &is_valid_char)
  if (!std::all_of(t.event.begin(), t.event.end(), &is_valid_char)) {
    utils::error{}("Invalid event string '{}' in line '{}'", t.event, line);
  }

  for (size_t i = 0; i < t.guards.size() && !t.guards[i].empty(); ++i) {
    if (!std::all_of(t.guards[i].begin(), t.guards[i].end(), &is_valid_char)) {
      utils::error{}("Invalid guard string '{}' in line '{}'", t.guards[i], line);
    }
  }

  for (size_t i = 0; i < t.actions.size() && !t.actions[i].empty(); ++i) {
    if (!std::all_of(t.actions[i].begin(), t.actions[i].end(), &is_valid_char)) {
      utils::error{}("Invalid action string '{}' in line '{}'", t.actions[i], line);
    }
  }

  return t;
}

// ключ хеш-индекса: смешиваем хеши состояния и события. Коллизия 64-бит пренебрежима и в
// духе остального проекта (string_hash коллизии ловятся как ошибки на загрузке).
static inline uint64_t group_key(const utils::id state_hash, const utils::id event_hash) noexcept {
  return utils::mix(state_hash, event_hash);
}

system::system(const act::registry* registry, std::vector<std::string> lines) : registry(registry), m_memory(std::move(lines)) {
  // 1. распарсить все строки, предпосчитать хеши.
  m_transitions.reserve(m_memory.size());
  for (const auto& line : m_memory) {
    auto t = parse_line(line);
    t.current_hash = utils::string_hash(t.current_state);
    t.event_hash   = utils::string_hash(t.event);
    t.next_hash    = t.next_state.empty() ? utils::invalid_id : utils::string_hash(t.next_state);
    m_transitions.push_back(std::move(t));
  }

  // 2. сгруппировать (state, event) непрерывно. stable_sort СОХРАНЯЕТ порядок исходных строк
  //    внутри группы (равные ключи) — это и есть тот самый top-down порядок гвардов.
  std::stable_sort(m_transitions.begin(), m_transitions.end(), [](const transition& a, const transition& b) {
    if (a.current_hash != b.current_hash) return a.current_hash < b.current_hash;
    return a.event_hash < b.event_hash;
  });

  // 3. построить индекс диапазонов + проверка дублей внутри группы (одинаковый набор гвардов).
  for (size_t i = 0; i < m_transitions.size();) {
    size_t j = i;
    while (j < m_transitions.size() &&
           m_transitions[j].current_hash == m_transitions[i].current_hash &&
           m_transitions[j].event_hash == m_transitions[i].event_hash) { ++j; }

    m_index.emplace(group_key(m_transitions[i].current_hash, m_transitions[i].event_hash),
                    range{ static_cast<uint32_t>(i), static_cast<uint32_t>(j - i) });

    // дубль: набор гвардов перехода b является подмножеством гвардов более раннего a (и b
    // имеет хотя бы один гвард) — как в исходной проверке (b проверялся против уже вставленных).
    for (size_t b = i; b < j; ++b) {
      if (m_transitions[b].guards[0].empty()) continue;
      for (size_t a = i; a < b; ++a) {
        bool subset = true;
        for (size_t g = 0; g < m_transitions[b].guards.size() && !m_transitions[b].guards[g].empty(); ++g) {
          bool found = false;
          for (size_t h = 0; h < m_transitions[a].guards.size() && !m_transitions[a].guards[h].empty(); ++h) {
            if (m_transitions[a].guards[h] == m_transitions[b].guards[g]) { found = true; break; }
          }
          if (!found) { subset = false; break; }
        }
        if (subset) utils::error{}("Found line with same state + event + guards as another\nLine1: {}\nLine2: {}", m_transitions[b].full_line, m_transitions[a].full_line);
      }
    }

    i = j;
  }

  // 4. резолв имён guard/action в типизированные указатели общего реестра act.
  for (auto& t : m_transitions) {
    for (size_t i = 0; i < t.guards.size() && !t.guards[i].empty(); ++i) {
      const auto& name = t.guards[i];
      const auto* fn = registry->predicate(utils::string_hash(name));
      if (fn != nullptr) t.guards_ptr[i] = fn;
      else utils::error{}("Could not find guard (predicate) function '{}' in act::registry", name);
    }

    for (size_t i = 0; i < t.actions.size() && !t.actions[i].empty(); ++i) {
      const auto& name = t.actions[i];
      const auto* fn = registry->effect(utils::string_hash(name));
      if (fn != nullptr) t.actions_ptr[i] = fn;
      else utils::error{}("Could not find action (effect) function '{}' in act::registry", name);
    }
  }
}

std::span<const system::transition> system::find_transitions(const utils::id state_hash, const utils::id event_hash) const {
  const auto itr = m_index.find(group_key(state_hash, event_hash));
  if (itr == m_index.end()) return {};
  return std::span(m_transitions.data() + itr->second.offset, itr->second.count);
}

std::span<const system::transition> system::find_transitions(const std::string_view& current_state, const std::string_view& event) const {
  return find_transitions(utils::string_hash(current_state), utils::string_hash(event));
}

std::span<const system::transition> system::transitions() const { return m_transitions; }

}
}
