#include "prefab_resource.h"

#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h> // utils::warn

namespace tile_frontier {
namespace core {

using namespace devils_engine;

prefab_resource::prefab_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void prefab_resource::load_cold(const utils::safe_handle_t&) {
  // list-секция (`//---`) отдаётся демиургом в list_section, иначе читаем весь файл.
  text_ = is_list_entry() && !list_section.empty() ? list_section : module->load_text(path);

  // Имя для spawn: из поля `name` list-секции (list_name), иначе базовое имя файла (хвост id).
  if (!list_name.empty()) {
    name_ = list_name;
  } else {
    const std::string_view sid = id;
    const auto slash = sid.rfind('/');
    name_ = std::string(slash == std::string_view::npos ? sid : sid.substr(slash + 1));
  }

  if (text_.empty()) utils::warn("prefab resource '{}': пустой текст префаба", id);
}

void prefab_resource::load_warm(const utils::safe_handle_t&) {}
void prefab_resource::unload_hot(const utils::safe_handle_t&) {}
void prefab_resource::unload_warm(const utils::safe_handle_t&) { text_.clear(); name_.clear(); }

}
}
