#include "script_resource.h"

#include <tavl/deserialize.h>

#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h> // utils::warn / utils::error
#include <devils_script/system.h>

#include "entity_scope.h" // root-скоуп parse<bool, entity_scope>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Тот же tavl-parse-идиом, что simul::parse_config_resource (пока локальная копия — DRY позже).
static void parse_script_config(demiurg::resource_interface& resource, actor_script_config& config) {
  const std::string content = resource.is_list_entry() && !resource.list_section.empty()
    ? resource.list_section
    : resource.module->load_text(resource.path);

  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  config = actor_script_config{};
  tavl::deserialize(parser, ctx, config);
  if (ctx.diagnostics.empty()) return;

  utils::warn("script resource '{}': {} tavl diagnostics", resource.id, ctx.diagnostics.size());
  for (const auto& d : ctx.diagnostics) {
    utils::warn("  tavl diagnostic '{}' at {}:{} field '{}'",
      tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
  }
}

script_resource::script_resource(devils_script::system* sys) : sys_(sys) {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void script_resource::load_cold(const utils::safe_handle_t&) {
  if (sys_ == nullptr) utils::error{}("script resource '{}': devils_script::system не впрыснут", id);

  actor_script_config cfg;
  parse_script_config(*this, cfg);

  if (cfg.expr.empty()) utils::error{}("script resource '{}': пустой expr", id);

  // Срез: единственная поддержанная комбинация (ret,scope). Общий диспетчер — шаг 4.
  if (cfg.ret == "bool") {
    program_ = sys_->parse<bool, entity_scope>(id, cfg.expr);
    category_ = act::category::predicate;
  } else {
    utils::error{}("script resource '{}': ret '{}' пока не поддержан (только \"bool\")", id, cfg.ret);
  }
}

void script_resource::load_warm(const utils::safe_handle_t&) {}
void script_resource::unload_hot(const utils::safe_handle_t&) {}
void script_resource::unload_warm(const utils::safe_handle_t&) { program_ = devils_script::container{}; }

}
}
