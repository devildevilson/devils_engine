#include "render_config_source.h"

#include "devils_engine/demiurg/module_interface.h"

namespace devils_engine {
namespace painter {

render_config_source::render_config_source() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void render_config_source::load_cold(const utils::safe_handle_t&) {
  text = is_list_entry() && !list_section.empty() ? list_section : module->load_text(path);
}

void render_config_source::load_warm(const utils::safe_handle_t&) {}
void render_config_source::unload_hot(const utils::safe_handle_t&) {}
void render_config_source::unload_warm(const utils::safe_handle_t&) { text.clear(); text.shrink_to_fit(); }

}
}
