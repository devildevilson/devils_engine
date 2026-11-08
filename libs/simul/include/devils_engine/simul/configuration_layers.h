#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <reflect>
#include <tavl/deserialize.h>
#include "devils_engine/utils/error.h"

namespace devils_engine::simul {
// Runtime may keep a composed view; authored documents have disjoint ownership.
inline constexpr std::array<std::string_view, 5> engine_configuration_sections{
  "window", "simulation", "render", "sound", "ui"};
inline constexpr std::array<std::string_view, 2> debug_configuration_sections{"metrics", "logging"};

inline bool configuration_section_allowed(std::span<const std::string_view> sections, std::string_view name) {
  return std::find(sections.begin(), sections.end(), name) != sections.end();
}

// Validate the entire event stream before deserialization: unknown root sections must not
// silently migrate across owners just because TAVL normally skips unknown aggregate fields.
inline void validate_configuration_sections(std::string_view text, std::string_view label,
                                            std::span<const std::string_view> sections) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();
  size_t depth = 0;
  bool saw_section = false;
  for (;;) {
    const auto [event, error] = parser.poll_event();
    if (!error.no_error()) utils::error{}("configuration '{}': {} at {}:{}", label,
      tavl::to_string(error.type), error.span.line, error.span.column);
    if (event.type == tavl::event_type::eof) break;
    if (depth == 0 && !saw_section && (event.type == tavl::event_type::object_begin || event.type == tavl::event_type::array_begin || event.type == tavl::event_type::tuple_begin))
      utils::error{}("configuration '{}': expected named top-level sections without an outer block", label);
    if (event.type == tavl::event_type::got_row_identifier && depth == 0) {
      saw_section = true;
      const auto name = parser.to_string(event.token);
      if (!configuration_section_allowed(sections, name)) utils::error{}(
        "configuration '{}': section '{}' is not owned by this document", label, name);
    }
    if (event.type == tavl::event_type::object_begin || event.type == tavl::event_type::array_begin || event.type == tavl::event_type::tuple_begin) ++depth;
    if (event.type == tavl::event_type::object_end || event.type == tavl::event_type::array_end || event.type == tavl::event_type::tuple_end) --depth;
  }
}

template <typename Settings>
void apply_configuration_sections(Settings& target, const Settings& source,
                                  std::span<const std::string_view> sections) {
  reflect::for_each([&](auto index) {
    if (configuration_section_allowed(sections, reflect::member_name<index>(source)))
      reflect::get<index>(target) = reflect::get<index>(source);
  }, source);
}

template <typename Settings>
void deserialize_configuration_layer(std::string_view text, std::string_view label,
                                     std::span<const std::string_view> sections, Settings& out) {
  validate_configuration_sections(text, label, sections);
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();
  Settings candidate = out;
  tavl::ct_context context;
  tavl::deserialize(parser, context, candidate);
  for (const auto& d : context.diagnostics) {
    if (d.error.type == tavl::error_type::warn_missing_field) continue;
    utils::error{}("configuration '{}': {} at {}:{}, field '{}'", label,
      tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
  }
  apply_configuration_sections(out, candidate, sections);
}

template <typename Settings>
struct runtime_debug_settings {
  decltype(std::declval<Settings>().metrics) metrics;
  decltype(std::declval<Settings>().logging) logging;
};
} // namespace devils_engine::simul
