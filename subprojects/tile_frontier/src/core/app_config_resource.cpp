#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h>
#include <tavl/deserialize.h>

#include "app_config_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

app_config_resource::app_config_resource() {
  // CPU-only ресурс: warm→hot не нужен (данные готовы после парсинга на load_cold).
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void app_config_resource::load_cold(const utils::safe_handle_t&) {
  const std::string content = module->load_text(path);

  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  cfg = app_config{}; // дефолты для полей, которых нет в файле
  tavl::deserialize(parser, ctx, cfg);

  if (!ctx.diagnostics.empty()) {
    utils::warn("app_config_resource '{}': {} tavl diagnostics", id, ctx.diagnostics.size());
    for (const auto& d : ctx.diagnostics) {
      utils::warn("  tavl diagnostic '{}' at {}:{} field '{}'",
                  tavl::to_string(d.error.type), source_line(static_cast<uint32_t>(d.error.span.line)), d.error.span.column, d.field);
    }
  }

  DE_LOG(catalogue::log_domain::resource, flow, "app_config_resource '{}': parsed (window {}x{}, render config '{}')",
         id, cfg.window.width, cfg.window.height, cfg.render.config_folder);
}

void app_config_resource::load_warm(const utils::safe_handle_t&) {}
void app_config_resource::unload_hot(const utils::safe_handle_t&) {}
void app_config_resource::unload_warm(const utils::safe_handle_t&) {
  cfg = app_config{};
}

} // namespace core
} // namespace tile_frontier
