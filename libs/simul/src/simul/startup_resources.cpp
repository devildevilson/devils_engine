#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/simul/startup_resources.h>
#include <devils_engine/utils/core.h>
#include <tavl/deserialize.h>

namespace devils_engine {
namespace simul {
namespace {

template <typename Config>
void parse_config_resource(demiurg::resource_interface& resource, Config& config) {
  const std::string content = resource.is_list_entry() && !resource.list_section.empty()
                                ? resource.list_section
                                : resource.module->load_text(resource.path);

  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  config = Config{};
  tavl::deserialize(parser, ctx, config);
  if (ctx.diagnostics.empty()) {
    return;
  }

  utils::warn("startup resource '{}': {} tavl diagnostics", resource.id, ctx.diagnostics.size());
  for (const auto& d : ctx.diagnostics) {
    utils::warn(
      "  tavl diagnostic '{}' at {}:{} field '{}'",
      tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
  }
}

void configure_cpu_resource(demiurg::resource_interface& resource) {
  resource.set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  resource.set_flag(demiurg::resource_flags::binary, false);
}

} // namespace

startup_entry_resource::startup_entry_resource() {
  configure_cpu_resource(*this);
}

void startup_entry_resource::load_cold(const utils::safe_handle_t&) {
  parse_config_resource(*this, config_);
}

void startup_entry_resource::load_warm(const utils::safe_handle_t&) {}
void startup_entry_resource::unload_hot(const utils::safe_handle_t&) {}
void startup_entry_resource::unload_warm(const utils::safe_handle_t&) {
  config_ = {};
}

runtime_state_resource::runtime_state_resource() {
  configure_cpu_resource(*this);
}

void runtime_state_resource::load_cold(const utils::safe_handle_t&) {
  parse_config_resource(*this, config_);
}

void runtime_state_resource::load_warm(const utils::safe_handle_t&) {}
void runtime_state_resource::unload_hot(const utils::safe_handle_t&) {}
void runtime_state_resource::unload_warm(const utils::safe_handle_t&) {
  config_ = {};
}

} // namespace simul
} // namespace devils_engine
