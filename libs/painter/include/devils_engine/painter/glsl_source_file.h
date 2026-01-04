#ifndef DEVILS_ENGINE_PAINTER_GLSL_SOURCE_FILE_H
#define DEVILS_ENGINE_PAINTER_GLSL_SOURCE_FILE_H

#include <cstdint>
#include <cstddef>
#include "devils_engine/demiurg/resource_base.h"

namespace devils_engine {
namespace painter {
class glsl_source_file : public demiurg::resource_interface {
public:
  std::string memory;

  glsl_source_file();

  void load_cold(const utils::safe_handle_t &handle) override;
  void load_warm(const utils::safe_handle_t &handle) override;

  void unload_hot(const utils::safe_handle_t &handle) override;
  void unload_warm(const utils::safe_handle_t &handle) override;
protected:

};
}
}

#endif