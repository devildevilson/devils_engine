#ifndef TILE_FRONTIER_CORE_ASSETS_SYSTEM_H
#define TILE_FRONTIER_CORE_ASSETS_SYSTEM_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/interface.h>

#include "actors.h"

namespace tile_frontier {
namespace core {

struct assets_simulation_init;

class assets_simulation : public simul::advancer {
public:
  assets_simulation(const size_t frame_time) noexcept;
  ~assets_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  assets_actor* get_actor();
private:
  std::unique_ptr<assets_simulation_init> container;
  assets_actor actor;
};

}
}

#endif
