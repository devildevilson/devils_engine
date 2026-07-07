#ifndef TILE_FRONTIER_CORE_SOUND_SYSTEM_H
#define TILE_FRONTIER_CORE_SOUND_SYSTEM_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/systems.h>

namespace tile_frontier {
namespace core {

struct sound_simulation_init;
struct broker;

// звук делает что?
class sound_simulation : public devils_engine::simul::sound_system<broker> {
public:
  sound_simulation(const size_t frame_time) noexcept;
  ~sound_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  // Единый broker всех каналов (runtime владеет). Задаётся до старта потока.
  void set_broker(struct broker* b);
private:
  std::unique_ptr<sound_simulation_init> container;
};

}
}

#endif
