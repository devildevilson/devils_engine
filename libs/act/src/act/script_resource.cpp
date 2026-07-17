#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h> // utils::warn / utils::error
#include <tavl/deserialize.h>

#include <utility>

#include "devils_engine/act/script_resource.h"

namespace devils_engine {
namespace act {

// Тот же tavl-parse-идиом, что simul::parse_config_resource (пока локальная копия — DRY позже).
static void parse_script_config(demiurg::resource_interface& resource, script_config& config) {
  const std::string content = resource.is_list_entry() && !resource.list_section.empty()
                                ? resource.list_section
                                : resource.module->load_text(resource.path);

  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  config = script_config{};
  tavl::deserialize(parser, ctx, config);
  if (ctx.diagnostics.empty()) {
    return;
  }

  utils::warn("script resource '{}': {} tavl diagnostics", resource.id, ctx.diagnostics.size());
  for (const auto& d : ctx.diagnostics) {
    utils::warn("  tavl diagnostic '{}' at {}:{} field '{}'",
                tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
  }
}

script_resource::script_resource(const script_compiler* compiler) : compiler_(compiler) {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void script_resource::load_cold(const utils::safe_handle_t&) {
  if (compiler_ == nullptr) {
    utils::error{}("script resource '{}': act::script_compiler was not injected", id);
  }

  script_config cfg;
  parse_script_config(*this, cfg);

  if (cfg.expr.empty()) {
    utils::error{}("script resource '{}': expr is empty", id);
  }

  auto compiled = compiler_->compile(id, cfg.ret, cfg.scope, cfg.expr);
  program_ = std::move(compiled.program);
  category_ = compiled.type;
}

void script_resource::load_warm(const utils::safe_handle_t&) {}
void script_resource::unload_hot(const utils::safe_handle_t&) {}
void script_resource::unload_warm(const utils::safe_handle_t&) {
  program_ = devils_script::container{};
}

} // namespace act
} // namespace devils_engine
