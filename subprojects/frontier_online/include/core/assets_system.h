#ifndef FRONTIER_ONLINE_CORE_ASSETS_SYSTEM_H
#define FRONTIER_ONLINE_CORE_ASSETS_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <devils_engine/simul/standard_assets_system.h>

#include "script_environment.h" // project devils_script system + entity_scope compiler adapter
#include "terrain.h"            // источник земли: чанк = функция (зерно, ключ)

namespace devils_engine {
namespace demiurg {
class resource_system;
}
} // namespace devils_engine

namespace frontier_online {
namespace core {

struct broker;
struct command_load_chunk;

// Тонкая обёртка над actor-паттерном: вся логика менеджмента загрузки — в
// demiurg::resource_loader (внутри container). assets владеет реестром ресурсов
// (resource_system + module_system), строит его в init() и далее только читает.
// Локальные переходы cold↔warm делает сам, GPU-переходы warm↔hot форвардит рендеру.
class assets_simulation : public devils_engine::simul::standard_assets_system<broker> {
public:
  assets_simulation(const size_t frame_time) noexcept;

  // Отпечаток мира последнего открытого генератора: зерно + размер чанка + тексты всех его
  // документов. Это и есть то, что поедет на рукопожатии вместо самой земли.
  uint64_t world_fingerprint() const noexcept;

protected:
  void update_project(const size_t time, ::frontier_online::core::broker& br) override;
  // Registers owner-level gameplay resources plus project world descriptors and injects the
  // project script compiler into act/acumen resources.
  void register_project_resource_types(devils_engine::demiurg::resource_system& resources) override;

private:
  // Источник под мир, названный запросом. Живёт на ассет-потоке и только на нём.
  const terrain_source* terrain_for(const command_load_chunk& request);

  script_environment script_env_; // owns ds::system and the parse-time root-scope adapter
  std::unique_ptr<terrain_source> terrain_;
  std::string terrain_id_;
};

} // namespace core
} // namespace frontier_online

#endif
