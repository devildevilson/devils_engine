#include "devils_engine/demiurg/module_interface.h"
#include "glsl_source_file.h"
#include "shader_crafter.h"

namespace devils_engine {
namespace painter {
glsl_source_file::glsl_source_file() : spirv_shader_kind(UINT32_MAX) {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

bool glsl_source_file::prepared(const uint32_t shader_kind) const noexcept {
  return spirv_shader_kind == shader_kind && !spirv.empty();
}

bool glsl_source_file::prepare_spirv(const demiurg::resource_system* reg, const uint32_t shader_kind, std::string* error) {
  if (prepared(shader_kind)) {
    return true;
  }

  if (memory.empty()) {
    load(utils::safe_handle_t{});
  }

  shader_crafter sc(reg);
  sc.set_optimization(true);
  sc.set_shader_entry_point("main");
  sc.set_shader_type(shader_kind);
  auto out = sc.compile(std::string(id), memory);
  if (out.empty()) {
    if (error != nullptr) {
      *error = sc.err_msg();
    }
    return false;
  }

  spirv = std::move(out);
  spirv_shader_kind = shader_kind;
  return true;
}

// супер простой класс в котором мы просто ждем когда нас положат в какой нибудь шейдер
void glsl_source_file::load_cold(const utils::safe_handle_t&) {
  memory = module->load_text(path);
}

void glsl_source_file::load_warm(const utils::safe_handle_t&) {}
void glsl_source_file::unload_hot(const utils::safe_handle_t&) {}
void glsl_source_file::unload_warm(const utils::safe_handle_t&) {
  memory.clear();
  memory.shrink_to_fit();
  spirv.clear();
  spirv.shrink_to_fit();
  spirv_shader_kind = UINT32_MAX;
}
} // namespace painter
} // namespace devils_engine
