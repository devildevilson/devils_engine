#ifndef TILE_FRONTIER_CORE_SOUND_SYSTEM_H
#define TILE_FRONTIER_CORE_SOUND_SYSTEM_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/interface.h>

#include "actors.h"

namespace tile_frontier {
namespace core {

struct sound_simulation_init;
struct broker;

// звук делает что?
class sound_simulation : public simul::advancer {
public:
  sound_simulation(const size_t frame_time) noexcept;
  ~sound_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  sound_actor* get_actor();
  // Единый broker всех каналов (main владеет). Задаётся до старта потока.
  void set_broker(broker* b);
private:
  std::unique_ptr<sound_simulation_init> container;
  sound_actor actor;
};

}
}

#endif
