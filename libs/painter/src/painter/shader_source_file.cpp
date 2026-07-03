#include "shader_source_file.h"

#include "devils_engine/demiurg/module_interface.h"

namespace devils_engine {
namespace painter {

shader_source_file::shader_source_file() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, true);
}

void shader_source_file::load_cold(const utils::safe_handle_t&) {
  // SPIR-V — бинарь: читаем как байты (load_text мог бы исказить не-UTF8 содержимое).
  std::vector<char> tmp;
  module->load_binary(path, tmp);
  memory.assign(tmp.begin(), tmp.end());
}

void shader_source_file::load_warm(const utils::safe_handle_t&) {}
void shader_source_file::unload_hot(const utils::safe_handle_t&) {}
void shader_source_file::unload_warm(const utils::safe_handle_t&) { memory.clear(); memory.shrink_to_fit(); }

}
}
