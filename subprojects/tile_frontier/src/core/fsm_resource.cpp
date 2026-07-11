#include "fsm_resource.h"

#include <tavl/deserialize.h>

#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h> // utils::warn

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Тот же tavl-parse-идиом, что simul::parse_config_resource / script_resource (DRY позже).
static void parse_fsm_config(demiurg::resource_interface& resource, fsm_config& config) {
  const std::string content = resource.is_list_entry() && !resource.list_section.empty()
    ? resource.list_section
    : resource.module->load_text(resource.path);

  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  config = fsm_config{};
  tavl::deserialize(parser, ctx, config);
  if (ctx.diagnostics.empty()) return;

  utils::warn("fsm resource '{}': {} tavl diagnostics", resource.id, ctx.diagnostics.size());
  for (const auto& d : ctx.diagnostics) {
    utils::warn("  tavl diagnostic '{}' at {}:{} field '{}'",
      tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
  }
}

fsm_resource::fsm_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void fsm_resource::load_cold(const utils::safe_handle_t&) {
  parse_fsm_config(*this, config_);
  if (config_.transitions.empty()) utils::warn("fsm resource '{}': пустой список transitions", id);
}

void fsm_resource::load_warm(const utils::safe_handle_t&) {}
void fsm_resource::unload_hot(const utils::safe_handle_t&) {}
void fsm_resource::unload_warm(const utils::safe_handle_t&) { config_ = {}; }

}
}
