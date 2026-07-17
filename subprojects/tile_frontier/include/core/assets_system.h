#ifndef TILE_FRONTIER_CORE_ASSETS_SYSTEM_H
#define TILE_FRONTIER_CORE_ASSETS_SYSTEM_H

#include <cstddef>

#include <devils_engine/simul/standard_assets_system.h>

#include "script_environment.h" // project devils_script system + entity_scope compiler adapter

namespace devils_engine {
namespace demiurg {
class resource_system;
}
} // namespace devils_engine

namespace tile_frontier {
namespace core {

struct broker;

// Тонкая обёртка над actor-паттерном: вся логика менеджмента загрузки — в
// demiurg::resource_loader (внутри container). assets владеет реестром ресурсов
// (resource_system + module_system), строит его в init() и далее только читает.
// Локальные переходы cold↔warm делает сам, GPU-переходы warm↔hot форвардит рендеру.
class assets_simulation : public devils_engine::simul::standard_assets_system<broker> {
public:
  assets_simulation(const size_t frame_time) noexcept;

protected:
  void update_project(const size_t time, ::tile_frontier::core::broker& br) override;
  // Registers owner-level gameplay resources plus project world descriptors and injects the
  // project script compiler into act/acumen resources.
  void register_project_resource_types(devils_engine::demiurg::resource_system& resources) override;

private:
  script_environment script_env_; // owns ds::system and the parse-time root-scope adapter
};

} // namespace core
} // namespace tile_frontier

#endif
