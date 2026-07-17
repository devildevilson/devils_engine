#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/utils/core.h> // utils::warn / utils::error
#include <devils_script/system.h>
#include <tavl/parser.h>

#include "entity_scope.h" // root-скоуп parse<bool, entity_scope>
#include "goap_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

namespace {

using ev_t = tavl::event_type;

// Идентификатор строки/ключа: в object-like блоках (root, {}) tavl даёт got_row_identifier, в
// data-driven ([]) — got_token. Принимаем оба, чтобы парсер не зависел от режима блока.
bool is_ident(const ev_t t) noexcept {
  return t == ev_t::got_token || t == ev_t::got_row_identifier;
}

// Пропустить события до открытия блока нужного типа (array/object). Возвращает false на eof.
bool enter_block(tavl::parser& p, const ev_t begin) {
  for (;;) {
    const auto e = p.peek();
    if (e.type == begin) {
      p.poll_event();
      return true;
    }
    if (e.type == ev_t::eof || e.type == ev_t::not_enought_data) {
      p.poll_event();
      return false;
    }
    p.poll_event();
  }
}

// Прочитать [ k0, !k1, not k2, ... ] в список строк, склеивая префиксы "!"/"not " с ключом.
std::vector<std::string> read_key_list(tavl::parser& p) {
  std::vector<std::string> out;
  if (!enter_block(p, ev_t::array_begin)) {
    return out;
  }
  std::string prefix;
  for (;;) {
    const auto e = p.peek();
    if (e.type == ev_t::array_end || e.type == ev_t::eof) {
      p.poll_event();
      break;
    }
    if (!is_ident(e.type)) {
      p.poll_event();
      continue;
    } // row_begin/row_end/comma
    const auto [tev, terr] = p.poll_event();
    const std::string tok = p.to_string(tev.token);
    if (tok == "!") {
      prefix = "!";
      continue;
    }
    if (tok == "not") {
      prefix = "not ";
      continue;
    }
    out.push_back(prefix + tok);
    prefix.clear();
  }
  return out;
}

// metrics = [ key = <ds expr>, ... ] — co-parse: снимаем key + '=', остаток строки доедает ds.
void parse_metrics(const devils_script::system& sys, tavl::parser& p, goap_config& cfg) {
  if (!enter_block(p, ev_t::array_begin)) {
    return;
  }
  for (;;) {
    const auto e = p.peek();
    if (e.type == ev_t::array_end || e.type == ev_t::eof) {
      p.poll_event();
      break;
    }
    if (!is_ident(e.type)) {
      p.poll_event();
      continue;
    } // row_begin/row_end
    const auto [kev, kerr] = p.poll_event();
    goap_metric m;
    m.key = p.to_string(kev.token);
    p.poll_event(); // '='
    devils_script::system::parse_context ctx;
    sys.parse<bool, entity_scope>(m.key, p, ctx, m.program); // ds доедает выражение до row_end
    cfg.metrics.push_back(std::move(m));
  }
}

// Разобрать один объект { name = X  requirements = [...]  next_state = [...]  weight_state = [...] }
// или { name  requirements  goal } (для целей). Неизвестные поля пропускаются.
template <typename Consume>
void parse_object(tavl::parser& p, const Consume& consume_field) {
  if (!enter_block(p, ev_t::object_begin)) {
    return;
  }
  for (;;) {
    const auto e = p.peek();
    if (e.type == ev_t::object_end || e.type == ev_t::eof) {
      p.poll_event();
      break;
    }
    if (!is_ident(e.type)) {
      p.poll_event();
      continue;
    } // row_begin/row_end
    const auto [fev, ferr] = p.poll_event();
    const std::string field = p.to_string(fev.token);
    p.poll_event(); // '='
    consume_field(field);
  }
}

void parse_actions(const devils_script::system& sys, tavl::parser& p,
                   std::vector<goap_action_config>& out) {
  if (!enter_block(p, ev_t::array_begin)) {
    return;
  }
  for (;;) {
    const auto e = p.peek();
    if (e.type == ev_t::array_end || e.type == ev_t::eof) {
      p.poll_event();
      break;
    }
    if (e.type != ev_t::object_begin) {
      p.poll_event();
      continue;
    } // разделители между объектами
    goap_action_config a;
    parse_object(p, [&](const std::string& field) {
      if (field == "name") {
        const auto [v, err] = p.poll_event();
        a.name = p.to_string(v.token);
      } else if (field == "effect") {
        devils_script::system::parse_context ctx;
        sys.parse<void, entity_scope>(a.name.empty() ? "goap.action.effect" : a.name,
                                      p, ctx, a.effect_program);
        a.has_effect_program = true;
      } else if (field == "requirements") {
        a.requirements = read_key_list(p);
      } else if (field == "next_state") {
        a.next_state = read_key_list(p);
      } else if (field == "weight_state") {
        a.weight_state = read_key_list(p);
      }
    });
    out.push_back(std::move(a));
  }
}

void parse_goals(tavl::parser& p, std::vector<goap_goal_config>& out) {
  if (!enter_block(p, ev_t::array_begin)) {
    return;
  }
  for (;;) {
    const auto e = p.peek();
    if (e.type == ev_t::array_end || e.type == ev_t::eof) {
      p.poll_event();
      break;
    }
    if (e.type != ev_t::object_begin) {
      p.poll_event();
      continue;
    }
    goap_goal_config g;
    parse_object(p, [&](const std::string& field) {
      if (field == "name") {
        const auto [v, err] = p.poll_event();
        g.name = p.to_string(v.token);
      } else if (field == "requirements") {
        g.requirements = read_key_list(p);
      } else if (field == "goal") {
        g.goal = read_key_list(p);
      }
    });
    out.push_back(std::move(g));
  }
}

// Верхний уровень: строки `section = [ ... ]` (metrics/actions/goals), диспетчер по имени секции.
void parse_goap(const devils_script::system& sys, tavl::parser& p, goap_config& cfg) {
  for (;;) {
    const auto e = p.peek();
    if (e.type == ev_t::eof || e.type == ev_t::not_enought_data) {
      p.poll_event();
      break;
    }
    if (!is_ident(e.type)) {
      p.poll_event();
      continue;
    } // row_begin/row_end
    const auto [sev, serr] = p.poll_event();
    const std::string section = p.to_string(sev.token);
    p.poll_event(); // '='
    if (section == "base") {
      const auto [v, err] = p.poll_event();
      cfg.base = p.to_string(v.token);
    } else if (section == "metrics") {
      parse_metrics(sys, p, cfg);
    } else if (section == "actions") {
      parse_actions(sys, p, cfg.actions);
    } else if (section == "goals") {
      parse_goals(p, cfg.goals);
    } else if (section == "disable_metrics") {
      cfg.disable_metrics = read_key_list(p);
    } else if (section == "disable_actions") {
      cfg.disable_actions = read_key_list(p);
    } else if (section == "disable_goals") {
      cfg.disable_goals = read_key_list(p);
    }
    // неизвестная секция: значение (массив) будет пропущено обычным ходом цикла
  }
}

} // namespace

namespace {
template <typename T, typename Key>
void erase_named(std::vector<T>& values, const std::vector<std::string>& disabled, const Key& key) {
  values.erase(std::remove_if(values.begin(), values.end(), [&](const T& value) {
                 return std::find(disabled.begin(), disabled.end(), key(value)) != disabled.end();
               }),
               values.end());
}

template <typename T, typename Key>
void overlay_named(std::vector<T>& values, const std::vector<T>& overlay, const Key& key) {
  for (const auto& value : overlay) {
    const auto it = std::find_if(values.begin(), values.end(), [&](const T& old) {
      return key(old) == key(value);
    });
    if (it != values.end()) {
      *it = value;
    } else {
      values.push_back(value);
    }
  }
}

std::string normalize_goap_id(std::string id) {
  if (!id.starts_with("goap/")) {
    id = "goap/" + id;
  }
  return id;
}

goap_config resolve_impl(demiurg::resource_system& resources, const std::string& id,
                         std::vector<std::string>& stack) {
  if (std::find(stack.begin(), stack.end(), id) != stack.end()) {
    utils::error{}("GOAP config inheritance cycle at '{}'", id);
  }
  auto* resource = resources.get<goap_resource>(id);
  if (resource == nullptr) {
    utils::error{}("GOAP config '{}' not found", id);
  }
  while (!resource->usable()) {
    resource->load(utils::safe_handle_t{});
  }

  stack.push_back(id);
  goap_config out;
  if (!resource->config().base.empty()) {
    out = resolve_impl(resources, normalize_goap_id(resource->config().base), stack);
  }
  out = merge_goap_config(out, resource->config());
  stack.pop_back();
  return out;
}
} // namespace

goap_config merge_goap_config(const goap_config& base, const goap_config& derived) {
  goap_config out = base;
  out.base.clear();
  erase_named(out.metrics, derived.disable_metrics, [](const goap_metric& v) -> const std::string& {
    return v.key;
  });
  erase_named(out.actions, derived.disable_actions, [](const goap_action_config& v) -> const std::string& {
    return v.name;
  });
  erase_named(out.goals, derived.disable_goals, [](const goap_goal_config& v) -> const std::string& {
    return v.name;
  });
  overlay_named(out.metrics, derived.metrics, [](const goap_metric& v) -> const std::string& {
    return v.key;
  });
  overlay_named(out.actions, derived.actions, [](const goap_action_config& v) -> const std::string& {
    return v.name;
  });
  overlay_named(out.goals, derived.goals, [](const goap_goal_config& v) -> const std::string& {
    return v.name;
  });
  out.disable_metrics.clear();
  out.disable_actions.clear();
  out.disable_goals.clear();
  return out;
}

goap_config resolve_goap_config(demiurg::resource_system& resources, const std::string_view id) {
  std::vector<std::string> stack;
  return resolve_impl(resources, normalize_goap_id(std::string(id)), stack);
}

goap_resource::goap_resource(devils_script::system* sys) : sys_(sys) {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void goap_resource::load_cold(const utils::safe_handle_t&) {
  if (sys_ == nullptr) {
    utils::error{}("goap resource '{}': devils_script::system was not injected", id);
  }

  const std::string content = is_list_entry() && !list_section.empty()
                                ? list_section
                                : module->load_text(path);

  tavl::parser p;
  sys_->configure_parser(p); // ds-операторы (=, математика) в tavl-парсер — для выражений метрик
  p.flush(content);
  p.finish();

  config_ = goap_config{};
  parse_goap(*sys_, p, config_);
  if (config_.metrics.empty() && config_.base.empty()) {
    utils::warn("goap resource '{}': neither a base nor any metrics were provided", id);
  }
}

void goap_resource::load_warm(const utils::safe_handle_t&) {}
void goap_resource::unload_hot(const utils::safe_handle_t&) {}
void goap_resource::unload_warm(const utils::safe_handle_t&) {
  config_ = goap_config{};
}

} // namespace core
} // namespace tile_frontier
