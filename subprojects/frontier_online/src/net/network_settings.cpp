#include "net/network_settings.h"

#include <fstream>
#include <iterator>
#include <string>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/platform.h>
#include <devils_engine/utils/root.h>
#include <tavl/deserialize.h>

namespace frontier_online::net {

namespace utils = devils_engine::utils;

network_settings load_network_settings() {
  const auto project = utils::root::open(utils::project_folder());
  if (!project) {
    utils::error{}("frontier_online network settings: project folder '{}' is not usable: {}",
                   utils::project_folder(), utils::to_string(project.error()));
  }
  const auto path = project->resolve(network_settings_file);
  if (!path) {
    utils::error{}("frontier_online network settings: '{}' does not resolve: {}", network_settings_file,
                   utils::to_string(path.error()));
  }
  std::ifstream input(*path, std::ios::binary);
  if (!input) return network_settings{}; // файла нет — умолчания

  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) utils::error{}("frontier_online network settings: '{}' could not be read", *path);
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();
  tavl::ct_context context;
  network_settings settings;
  tavl::deserialize(parser, context, settings);
  // Опущенное поле — не ошибка: файл переопределяет только то, что в нём написано. (Неизвестное
  // поле tavl не сообщает вовсе — опечатка в имени ключа останется незамеченной.)
  std::erase_if(context.diagnostics, [](const auto& diagnostic) {
    return diagnostic.error.type == tavl::error_type::warn_missing_field;
  });
  if (!context.diagnostics.empty()) {
    const auto& first = context.diagnostics.front();
    utils::error{}("frontier_online network settings '{}': {} tavl diagnostics, first '{}' at {}:{} "
                   "field '{}'", *path, context.diagnostics.size(), tavl::to_string(first.error.type),
                   first.error.span.line + 1, first.error.span.column + 1, first.field);
  }
  if (!reconnect_policy_of(settings).valid()) {
    utils::error{}("frontier_online network settings '{}': reconnect policy is inconsistent (suspect {} "
                   "ms, lost {} ms, backoff {}..{} ms, {} attempts)", *path, settings.reconnect.suspect_ms,
                   settings.reconnect.lost_ms, settings.reconnect.first_backoff_ms,
                   settings.reconnect.max_backoff_ms, settings.reconnect.max_attempts);
  }
  if (settings.authority.hold_ms == 0 || settings.authority.ticket_ms < 2 * uint64_t(settings.authority.hold_ms)) {
    utils::error{}("frontier_online network settings '{}': reconnect hold {} ms and ticket {} ms are "
                   "inconsistent (hold > 0, ticket >= 2 * hold: a ticket renewed at half its life must "
                   "outlive a hold)", *path, settings.authority.hold_ms, settings.authority.ticket_ms);
  }
  return settings;
}

} // namespace frontier_online::net
