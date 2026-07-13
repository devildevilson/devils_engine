#ifndef DEVILS_ENGINE_SIMUL_RENDER_CONFIG_H
#define DEVILS_ENGINE_SIMUL_RENDER_CONFIG_H

// Project-facing configuration values consumed by the standard render runtime.

#include <cstdint>
#include <string>

#include <vulkan/vulkan_core.h>

namespace devils_engine {
namespace demiurg {
class resource_system;
}

namespace simul {

struct render_system_config {
  const demiurg::resource_system* engine_registry = nullptr;
  std::string render_config_prefix;
  std::string shader_config_prefix;

  const demiurg::resource_system* cache_registry = nullptr;
  std::string pipeline_cache_id;
  std::string pipeline_cache_path;

  std::string graph_name = "graphics1";
  std::string menu_graph_name;
  std::string texture_descriptor_name = "textures";
  std::string app_name = "devils_app";
  std::string engine_name = "devils_engine";
  uint32_t app_version = VK_MAKE_VERSION(0, 1, 1);
  uint32_t engine_version = VK_MAKE_VERSION(0, 1, 1);
  bool create_vulkan_on_init = true;
  bool headless = false;
};

} // namespace simul
} // namespace devils_engine

#endif
