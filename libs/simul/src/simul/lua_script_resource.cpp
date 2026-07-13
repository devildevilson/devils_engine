#include <algorithm>

#include <devils_engine/demiurg/module_interface.h>

#include "devils_engine/simul/lua_script_resource.h"

namespace devils_engine {
namespace simul {

lua_script_resource::lua_script_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void lua_script_resource::ensure_text_loaded() {
  if (!text.empty()) {
    return;
  }

  if (is_list_entry()) {
    if (!list_section.empty()) {
      text = list_section;
      return;
    }

    if (list_offset != SIZE_MAX) {
      const std::string full = module->load_text(path);
      if (list_offset < full.size()) {
        text = full.substr(list_offset, std::min(list_size, full.size() - list_offset));
      }
      return;
    }
  }

  text = module->load_text(path);
}

void lua_script_resource::drop_text() {
  text.clear();
  text.shrink_to_fit();
  list_section.clear();
  list_section.shrink_to_fit();
}

void lua_script_resource::load_cold(const utils::safe_handle_t&) {
  ensure_text_loaded();
}

void lua_script_resource::load_warm(const utils::safe_handle_t&) {}
void lua_script_resource::unload_hot(const utils::safe_handle_t&) {}
void lua_script_resource::unload_warm(const utils::safe_handle_t&) {
  drop_text();
}

} // namespace simul
} // namespace devils_engine
