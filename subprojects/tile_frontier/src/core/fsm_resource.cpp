#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/string-utils.hpp>
#include <tavl/parser.h>

#include <algorithm>
#include <string_view>

#include "fsm_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

namespace {

using ev_t = tavl::event_type;

struct parsed_event {
  tavl::event event;
  tavl::error error;
};

parsed_event poll(tavl::parser& parser, const demiurg::resource_interface& resource) {
  const auto [event, error] = parser.poll_event();
  if (error.is_critical()) {
    utils::error{}("fsm resource '{}': TAVL error '{}' at {}:{}",
                   resource.id, tavl::to_string(error.type), error.span.line + 1, error.span.column + 1);
  }
  return {event, error};
}

bool is_trivia(const ev_t type) {
  return type == ev_t::row_begin || type == ev_t::empty_row || type == ev_t::got_comment;
}

parsed_event next_transition_event(tavl::parser& parser, const demiurg::resource_interface& resource) {
  for (;;) {
    auto parsed = poll(parser, resource);
    if (!is_trivia(parsed.event.type)) {
      return parsed;
    }
  }
}

std::string token_text(tavl::parser& parser, const tavl::event& event) {
  return parser.to_string(event.token);
}

void expect_identifier(
  tavl::parser& parser,
  const demiurg::resource_interface& resource,
  const tavl::event& event,
  const std::string_view role,
  std::string& out) {
  if (event.type != ev_t::got_token || event.token.type != tavl::token_type::identifier) {
    utils::error{}("fsm resource '{}': expected {} identifier at {}:{}; transition rows must not be quoted",
                   resource.id, role, event.token.span.line + 1, event.token.span.column + 1);
  }
  out = token_text(parser, event);
}

std::vector<std::string> parse_identifier_block(
  tavl::parser& parser,
  const demiurg::resource_interface& resource,
  const ev_t end,
  const std::string_view role) {
  std::vector<std::string> out;
  for (;;) {
    auto parsed = poll(parser, resource);
    const auto& event = parsed.event;
    if (event.type == end) {
      return out;
    }
    if (is_trivia(event.type) || event.type == ev_t::row_end) {
      continue;
    }
    std::string name;
    expect_identifier(parser, resource, event, role, name);
    out.push_back(std::move(name));
  }
}

std::string source_row(const std::string_view content, const tavl::source_span span) {
  size_t begin = std::min(span.offset, content.size());
  while (begin > 0 && content[begin - 1] != '\n' && content[begin - 1] != '\r') {
    --begin;
  }
  size_t end = std::min(span.offset + span.size, content.size());
  while (end < content.size() && content[end] != '\n' && content[end] != '\r') {
    ++end;
  }
  return std::string(utils::string::trim(content.substr(begin, end - begin)));
}

mood::transition_config parse_transition(
  tavl::parser& parser,
  const demiurg::resource_interface& resource,
  const std::string_view content,
  const tavl::event& first) {
  mood::transition_config config;
  config.source = source_row(content, first.token.span);
  expect_identifier(parser, resource, first, "current state", config.current_state);

  auto parsed = next_transition_event(parser, resource);
  auto text = token_text(parser, parsed.event);
  if (parsed.event.type == ev_t::got_token && text == "+") {
    parsed = next_transition_event(parser, resource);
    expect_identifier(parser, resource, parsed.event, "event", config.event);
    parsed = next_transition_event(parser, resource);
    text = token_text(parser, parsed.event);
  }

  if (parsed.event.type == ev_t::array_begin) {
    config.guards = parse_identifier_block(parser, resource, ev_t::array_end, "guard");
    parsed = next_transition_event(parser, resource);
    text = token_text(parser, parsed.event);
  }

  if (parsed.event.type == ev_t::got_token && text == "/") {
    parsed = next_transition_event(parser, resource);
    if (parsed.event.type != ev_t::tuple_begin) {
      utils::error{}("fsm resource '{}': actions after '/' must be wrapped in parentheses at {}:{}",
                     resource.id, parsed.event.token.span.line + 1, parsed.event.token.span.column + 1);
    }
    config.actions = parse_identifier_block(parser, resource, ev_t::tuple_end, "action");
    parsed = next_transition_event(parser, resource);
    text = token_text(parser, parsed.event);
  }

  if (parsed.event.type == ev_t::got_token && text == "=") {
    parsed = next_transition_event(parser, resource);
    expect_identifier(parser, resource, parsed.event, "next state", config.next_state);
    parsed = next_transition_event(parser, resource);
  }

  if (parsed.event.type != ev_t::row_end) {
    utils::error{}("fsm resource '{}': unexpected token '{}' at {}:{} in transition '{}'",
                   resource.id, token_text(parser, parsed.event), parsed.event.token.span.line + 1,
                   parsed.event.token.span.column + 1, config.source);
  }
  return config;
}

void parse_fsm_config(demiurg::resource_interface& resource, fsm_config& config) {
  const std::string content = resource.is_list_entry() && !resource.list_section.empty()
                                ? resource.list_section
                                : resource.module->load_text(resource.path);

  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  config = fsm_config{};
  bool found_transitions = false;
  for (;;) {
    auto parsed = poll(parser, resource);
    const auto& event = parsed.event;
    if (event.type == ev_t::eof) {
      break;
    }
    if (event.type != ev_t::got_row_identifier || token_text(parser, event) != "transitions") {
      continue;
    }

    found_transitions = true;
    do {
      parsed = poll(parser, resource);
    } while (parsed.event.type != ev_t::array_begin && parsed.event.type != ev_t::eof);
    if (parsed.event.type != ev_t::array_begin) {
      utils::error{}("fsm resource '{}': transitions must be a TAVL array", resource.id);
    }

    for (;;) {
      parsed = poll(parser, resource);
      if (parsed.event.type == ev_t::array_end) {
        break;
      }
      if (is_trivia(parsed.event.type) || parsed.event.type == ev_t::row_end) {
        continue;
      }
      config.transitions.push_back(parse_transition(parser, resource, content, parsed.event));
    }
  }
  if (!found_transitions) {
    utils::error{}("fsm resource '{}': missing transitions array", resource.id);
  }
}

} // namespace

fsm_resource::fsm_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void fsm_resource::load_cold(const utils::safe_handle_t&) {
  parse_fsm_config(*this, config_);
  if (config_.transitions.empty()) {
    utils::warn("fsm resource '{}': transitions array is empty", id);
  }
}

void fsm_resource::load_warm(const utils::safe_handle_t&) {}
void fsm_resource::unload_hot(const utils::safe_handle_t&) {}
void fsm_resource::unload_warm(const utils::safe_handle_t&) {
  config_ = {};
}

} // namespace core
} // namespace tile_frontier
