#include "shader_compiler_state.h"

// Владение компилятором GLSL и его дисковым кэшем. Сама компиляция живёт в `shader_crafter`: там
// собираются опции, дефайны и includer, а здесь — только то, что переиспользуется между вызовами.

namespace devils_engine {
namespace painter {

shader_compiler::shader_compiler() : state_(std::make_unique<state>()) {}
shader_compiler::~shader_compiler() noexcept = default;
shader_compiler::shader_compiler(shader_compiler&&) noexcept = default;
shader_compiler& shader_compiler::operator=(shader_compiler&&) noexcept = default;

void shader_compiler::set_cache_directory(std::string path) {
  state_->cache_dir = std::move(path);
}

const std::string& shader_compiler::cache_directory() const noexcept {
  return state_->cache_dir;
}

bool shader_compiler::glslang_started() const noexcept {
  return state_->compiler != nullptr;
}

size_t shader_compiler::cache_hits() const noexcept {
  return state_->hits;
}

size_t shader_compiler::compilations() const noexcept {
  return state_->compilations;
}

} // namespace painter
} // namespace devils_engine
