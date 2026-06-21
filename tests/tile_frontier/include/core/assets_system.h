#ifndef TILE_FRONTIER_CORE_ASSETS_SYSTEM_H
#define TILE_FRONTIER_CORE_ASSETS_SYSTEM_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/interface.h>

#include "actors.h"

namespace devils_engine { namespace demiurg { class resource_system; } }

namespace tile_frontier {
namespace core {

struct assets_simulation_init;

// Тонкая обёртка над actor-паттерном: вся логика менеджмента загрузки — в
// demiurg::resource_loader (внутри container). assets владеет реестром ресурсов
// (resource_system + module_system), строит его в init() и далее только читает.
// Локальные переходы cold↔warm делает сам, GPU-переходы warm↔hot форвардит рендеру.
class assets_simulation : public simul::advancer {
public:
  assets_simulation(const size_t frame_time) noexcept;
  ~assets_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  assets_actor* get_actor();

  // Реестр ресурсов: строится в init() (на главном потоке), потом стабилен и читается main'ом.
  demiurg::resource_system* resources();

  // Куда форвардить GPU-переходы (warm↔hot). Задаётся из simulation::init после создания рендера.
  void set_render_actor(graphics_actor* gactor);
private:
  std::unique_ptr<assets_simulation_init> container;
  assets_actor actor;
};

}
}

#endif
