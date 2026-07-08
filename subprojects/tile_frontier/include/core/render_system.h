#ifndef TILE_FRONTIER_CORE_RENDER_SYSTEM_H
#define TILE_FRONTIER_CORE_RENDER_SYSTEM_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/render_config.h>
#include <devils_engine/simul/systems.h>

namespace devils_engine { namespace demiurg { class resource_system; } }

namespace tile_frontier {
namespace core {

using namespace devils_engine;

struct render_simulation_init;
struct broker;

using render_simulation_config = devils_engine::simul::render_system_config;

class render_simulation : public devils_engine::simul::render_system<broker> {
public:
  render_simulation(const size_t frame_time, render_simulation_config config) noexcept;
  ~render_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  // Единый broker всех каналов (runtime владеет). Задаётся до старта потока; заодно триггерит
  // попытку сборки графа.
  void set_broker(struct broker* b);
private:
  std::unique_ptr<render_simulation_init> container;
};

}
}

#endif
