#ifndef TILE_FRONTIER_CORE_RENDER_SYSTEM_H
#define TILE_FRONTIER_CORE_RENDER_SYSTEM_H

#include <cstddef>
#include <memory>
#include <string>

#include <devils_engine/simul/interface.h>

#include "actors.h"

namespace tile_frontier {
namespace core {

struct render_simulation_init;

// Параметры запуска рендера. Заполняются из app_config в simulation::init()
// и фиксируются на время жизни render_simulation.
struct render_simulation_config {
  std::string render_config_folder;
  std::string pipeline_cache_path;
  std::string graph_name = "graphics1";
  bool create_vulkan_on_init = true;
  bool headless = false;
};

class render_simulation : public simul::advancer {
public:
  render_simulation(const size_t frame_time, render_simulation_config config) noexcept;
  ~render_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  graphics_actor* get_actor();
private:
  std::unique_ptr<render_simulation_init> container;
  graphics_actor actor;
};

}
}

#endif
