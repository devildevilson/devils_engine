#include "glsl_source_file.h"

#include "demiurg/module_interface.h"

namespace devils_engine {
namespace painter {
glsl_source_file::glsl_source_file() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

// супер простой класс в котором мы просто ждем когда нас положат в какой нибудь шейдер
void glsl_source_file::load_cold(const utils::safe_handle_t &) {
  memory = module->load_text(path);
}

void glsl_source_file::load_warm(const utils::safe_handle_t &) {}
void glsl_source_file::unload_hot(const utils::safe_handle_t &) {}
void glsl_source_file::unload_warm(const utils::safe_handle_t&) { memory.clear(); memory.shrink_to_fit(); }
}
}