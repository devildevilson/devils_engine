#include "devils_engine/demiurg/module_interface.h"
#include "render_config_source.h"

namespace devils_engine {
namespace painter {

render_config_source::render_config_source() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void render_config_source::ensure_text_loaded() {
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

void render_config_source::drop_text() {
  text.clear();
  text.shrink_to_fit();
  list_section.clear();
  list_section.shrink_to_fit();
}

void render_config_source::load_cold(const utils::safe_handle_t&) {
  ensure_text_loaded();
}

void render_config_source::load_warm(const utils::safe_handle_t&) {}
void render_config_source::unload_hot(const utils::safe_handle_t&) {}
void render_config_source::unload_warm(const utils::safe_handle_t&) {
  drop_text();
}

} // namespace painter
} // namespace devils_engine
