#include "devils_engine/utils/log.h"
#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/simul/configuration_layers.h>

#include "app_config_resource.h"

namespace frontier_online {
namespace core {

using namespace devils_engine;

app_config_resource::app_config_resource() {
  // CPU-only ресурс: warm→hot не нужен (данные готовы после парсинга на load_cold).
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

namespace {
std::span<const std::string_view> sections(std::string_view id) {
  static constexpr std::array<std::string_view, 1> project_sections{"time"};
  if (id == "config/engine") return simul::engine_configuration_sections;
  if (id == "config/debug") return simul::debug_configuration_sections;
  if (id == "config/project") return project_sections;
  utils::error{}("configuration '{}': expected config/engine, config/project or config/debug; migrate app.tavl", id);
  return {};
}
} // namespace

void app_config_resource::load_cold(const utils::safe_handle_t&) {
  app_config candidate{};
  simul::deserialize_configuration_layer(module->load_text(path), id, sections(id), candidate);
  cfg = std::move(candidate);
}

void app_config_resource::apply_to(app_config& target) const {
  simul::apply_configuration_sections(target, cfg, sections(id));
}

void app_config_resource::load_warm(const utils::safe_handle_t&) {}
void app_config_resource::unload_hot(const utils::safe_handle_t&) {}
void app_config_resource::unload_warm(const utils::safe_handle_t&) {
  cfg = app_config{};
}

} // namespace core
} // namespace frontier_online
