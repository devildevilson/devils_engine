#include "system.h"

#include "utils/core.h"
#include "utils/string-utils.hpp"
#include "utils/patterns.h"

#include <algorithm>
#include <cctype>
#include <array>

namespace devils_engine {
namespace mood {

int32_t system::transition::is_valid(void* userdata) const {
  int32_t ret = 1;
  for (size_t i = 0; i < guards.size() && !guards[i].empty() && bool(ret); ++i) {
    const auto r = (*guards_ptr[i])(userdata);
    if (r < 0) utils::error{}("Something went wrong while computing transition guard '{}' in line '{}'", guards[i], full_line);
    ret = int32_t(bool(ret) && bool(r));
  }
  return ret;
}

int32_t system::transition::on_exit(void* userdata) const {
  for (const auto &t : current_state_on_exit) {
    const bool ret = t.is_valid(userdata);
    if (ret) { t.process(userdata); break; }
  }

  return 0;
}

int32_t system::transition::process(void* userdata) const {
  for (size_t i = 0; i < actions.size() && !actions[i].empty(); ++i) {
    const auto ret = (*actions_ptr[i])(userdata);
    if (ret != 0) utils::error{}("Something went wrong while computing transition action '{}' in line '{}'", actions[i], full_line);
  }

  return 0;
}

int32_t system::transition::on_entry(void* userdata) const {
  for (const auto& t : next_state_on_entry) {
    const bool ret = t.is_valid(userdata);
    if (ret) { t.process(userdata); break; }
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

static bool find_transition_f(const system::transition &obj, const std::pair<std::string_view, std::string_view>& value) {
  std::less<std::string_view> l;
  if (obj.current_state == value.first) return l(obj.event, value.second);
  return l(obj.current_state, value.first);
}

system::system(const struct table* table, std::vector<std::string> lines) noexcept : table(table), m_memory(std::move(lines)) {
  for (const auto& line : m_memory) {
    auto t = parse_line(line);

    // нужно проверить повторы? как? повтор это если
    // current_state, event, guards совпадают
    {
      const auto test = find_transitions(t.current_state, t.event);
      for (const auto &another : test) {
        const auto another_end = std::find(another.guards.begin(), another.guards.end(), std::string_view());
        bool equals = !t.guards[0].empty();
        for (size_t i = 0; i < t.guards.size() && !t.guards[i].empty(); ++i) {
          equals = equals && (std::find(another.guards.begin(), another_end, t.guards[i]) != another_end);
        }

        if (equals) utils::error{}("Found line with same state + event + guards as another\nLine1: {}\nLine2: {}", line, another.full_line);
      }
    }

    const auto p = std::make_pair(t.current_state, t.event);
    auto itr = std::lower_bound(m_transitions.begin(), m_transitions.end(), p, &find_transition_f);
    auto end = itr;
    for (; end != m_transitions.end() && end->current_state == t.current_state && end->event == t.event; ++end) {}
    m_transitions.insert(end, std::move(t));
  }

  for (auto &t : m_transitions) {
    t.current_state_on_exit = find_transitions(t.current_state, "on_exit");
    t.next_state_on_entry = find_transitions(t.next_state, "on_entry");

    for (size_t i = 0; i < t.guards.size() && !t.guards[i].empty(); ++i) {
      const auto& name = t.guards[i];
      auto itr = table->guards.find(name);
      if (itr != table->guards.end()) t.guards_ptr[i] = &itr->second;
      else { 
        for (const auto& [name, fn] : table->guards) {
          utils::print(name, " ");
        }
        utils::error{}("Could not find guard function '{}'", name); 
      }
    }

    for (size_t i = 0; i < t.actions.size() && !t.actions[i].empty(); ++i) {
      const auto& name = t.actions[i];
      auto itr = table->actions.find(name);
      if (itr != table->actions.end()) t.actions_ptr[i] = &itr->second;
      else utils::error{}("Could not find action function '{}'", name);
    }
  }
}

std::span<const system::transition> system::find_transitions(const std::string_view& current_state, const std::string_view& event) const {
  const auto p = std::make_pair(current_state, event);
  auto itr = std::lower_bound(m_transitions.begin(), m_transitions.end(), p, &find_transition_f);

  auto beg = itr;
  auto end = itr;

  if (beg != m_transitions.begin()) { // у msvc есть какая то странная проверка выхода за пределы итераторов
    auto tmp = beg - 1;
    while (beg != m_transitions.begin() && tmp->current_state == current_state && tmp->event == event) {
      beg = tmp;
      if (tmp != m_transitions.begin()) tmp -= 1;
    }
  }

  for (; end != m_transitions.end() && end->current_state == current_state && end->event == event; ++end) {}

  return std::span(beg, end);
}

std::span<const system::transition> system::transitions() const { return m_transitions; }

}
}