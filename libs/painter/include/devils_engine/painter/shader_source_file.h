#ifndef DEVILS_ENGINE_PAINTER_SHADER_SOURCE_FILE_H
#define DEVILS_ENGINE_PAINTER_SHADER_SOURCE_FILE_H

#include <cstddef>
#include <cstdint>
#include "demiurg/resource_base.h"

namespace devils_engine {
namespace painter {

// этот класс чисто загрузит текст шейдера и не будет его компилировать
// в таком виде использовать не нужно, потом сделать отдельные классы под шейдеры
// они будут уже возвращать шейдер модули

class shader_source_file : public demiurg::resource_interface {
public:
  std::string memory;

  shader_source_file();

  void load_cold(const utils::safe_handle_t &handle) override;
  void load_warm(const utils::safe_handle_t &handle) override;

  void unload_hot(const utils::safe_handle_t &handle) override;
  void unload_warm(const utils::safe_handle_t &handle) override;
};

}
}

#endif