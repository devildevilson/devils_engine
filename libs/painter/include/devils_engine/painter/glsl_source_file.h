#ifndef DEVILS_ENGINE_PAINTER_GLSL_SOURCE_FILE_H
#define DEVILS_ENGINE_PAINTER_GLSL_SOURCE_FILE_H

// Demiurg resource that owns GLSL source and its assets-side prepared SPIR-V.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "devils_engine/demiurg/resource_base.h"

namespace devils_engine {
namespace demiurg {
class resource_system;
}
namespace painter {
class glsl_source_file : public demiurg::resource_interface {
public:
  using shader_definition = std::pair<std::string, std::string>;

  struct prepared_variant {
    uint32_t shader_kind = UINT32_MAX;
    std::vector<shader_definition> definitions;
    std::vector<uint32_t> spirv;
  };

  std::string memory;
  std::vector<uint32_t> spirv;
  uint32_t spirv_shader_kind;
  std::vector<prepared_variant> variants;

  glsl_source_file();

  bool prepared(uint32_t shader_kind) const noexcept;
  bool prepare_spirv(const demiurg::resource_system* reg, uint32_t shader_kind, std::string* error = nullptr);
  const std::vector<uint32_t>* prepared_spirv(uint32_t shader_kind, std::span<const shader_definition> definitions) const noexcept;
  bool prepare_spirv(const demiurg::resource_system* reg, uint32_t shader_kind, std::span<const shader_definition> definitions, std::string* error = nullptr);

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;

  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

protected:
};
} // namespace painter
} // namespace devils_engine

#endif
