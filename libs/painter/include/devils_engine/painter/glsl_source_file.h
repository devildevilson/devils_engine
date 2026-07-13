#ifndef DEVILS_ENGINE_PAINTER_GLSL_SOURCE_FILE_H
#define DEVILS_ENGINE_PAINTER_GLSL_SOURCE_FILE_H

// Demiurg resource that owns GLSL source and its assets-side prepared SPIR-V.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "devils_engine/demiurg/resource_base.h"

namespace devils_engine {
namespace demiurg {
class resource_system;
}
namespace painter {
class glsl_source_file : public demiurg::resource_interface {
public:
  std::string memory;
  std::vector<uint32_t> spirv;
  uint32_t spirv_shader_kind;

  glsl_source_file();

  bool prepared(uint32_t shader_kind) const noexcept;
  bool prepare_spirv(const demiurg::resource_system* reg, uint32_t shader_kind, std::string* error = nullptr);

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;

  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

protected:
};
} // namespace painter
} // namespace devils_engine

#endif
