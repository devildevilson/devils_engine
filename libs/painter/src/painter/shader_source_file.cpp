#include "devils_engine/demiurg/module_interface.h"
#include "shader_source_file.h"

namespace devils_engine {
namespace painter {

shader_source_file::shader_source_file() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, true);
}

void shader_source_file::load_cold(const utils::safe_handle_t&) {
  module->load_binary(path, memory); // SPIR-V — бинарь, читаем напрямую в vector<uint8_t>
}

void shader_source_file::load_warm(const utils::safe_handle_t&) {}
void shader_source_file::unload_hot(const utils::safe_handle_t&) {}
void shader_source_file::unload_warm(const utils::safe_handle_t&) {
  memory.clear();
  memory.shrink_to_fit();
}

} // namespace painter
} // namespace devils_engine
