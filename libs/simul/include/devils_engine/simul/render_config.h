#ifndef DEVILS_ENGINE_SIMUL_RENDER_CONFIG_H
#define DEVILS_ENGINE_SIMUL_RENDER_CONFIG_H

#include <string>

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
  bool create_vulkan_on_init = true;
  bool headless = false;
};

}
}

#endif
