#ifndef DEVILS_ENGINE_PAINTER_SHADER_COMPILER_STATE_H
#define DEVILS_ENGINE_PAINTER_SHADER_COMPILER_STATE_H

#include <memory>
#include <string>

#include <shaderc/shaderc.hpp>

#include "shader_compiler.h"

// Внутренняя часть `shader_compiler`: сам объект shaderc и дисковый кэш. Держится в отдельной
// внутренней шапке, потому что её видят ДВОЕ — сам компилятор и `shader_crafter`, который через него
// компилирует, — а публичной шапке знать про shaderc незачем.

namespace devils_engine {
namespace painter {

struct shader_compiler::state {
  // ЛЕНИВО: пока ни одной настоящей компиляции не случилось, состояние glslang не поднимается. На
  // этом и держится обещание дискового кэша — тёплый прогон не платит 90 мс инициализации.
  std::unique_ptr<shaderc::Compiler> compiler;
  std::string cache_dir;
  size_t hits = 0;
  size_t compilations = 0;
  // Жалоба на непригодный каталог кэша — ОДНА на компилятор: каталог сам не исправится, а строка на
  // каждый шейдер заглушила бы весь остальной вывод.
  bool cache_complained = false;

  shaderc::Compiler& glslang() {
    if (compiler == nullptr) {
      compiler = std::make_unique<shaderc::Compiler>();
    }
    return *compiler;
  }
};

} // namespace painter
} // namespace devils_engine

#endif
