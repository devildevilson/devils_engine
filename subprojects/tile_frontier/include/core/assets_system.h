#ifndef TILE_FRONTIER_CORE_ASSETS_SYSTEM_H
#define TILE_FRONTIER_CORE_ASSETS_SYSTEM_H

#include <cstddef>
#include <memory>

#include <devils_engine/simul/systems.h>

namespace devils_engine { namespace demiurg { class resource_system; } }

namespace tile_frontier {
namespace core {

struct assets_simulation_init;
struct broker;

// Тонкая обёртка над actor-паттерном: вся логика менеджмента загрузки — в
// demiurg::resource_loader (внутри container). assets владеет реестром ресурсов
// (resource_system + module_system), строит его в init() и далее только читает.
// Локальные переходы cold↔warm делает сам, GPU-переходы warm↔hot форвардит рендеру.
class assets_simulation : public devils_engine::simul::assets_system<broker> {
public:
  assets_simulation(const size_t frame_time) noexcept;
  ~assets_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  // Реестр ресурсов: строится в init() (на главном потоке), потом стабилен и читается main'ом.
  devils_engine::demiurg::resource_system* resources();

  // Единый broker всех каналов (runtime владеет). Задаётся до старта потока.
  void set_broker(struct broker* b);
private:
  std::unique_ptr<assets_simulation_init> container;
};

}
}

#endif
