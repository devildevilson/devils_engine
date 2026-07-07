#ifndef TILE_FRONTIER_CORE_APP_CONFIG_RESOURCE_H
#define TILE_FRONTIER_CORE_APP_CONFIG_RESOURCE_H

#include <devils_engine/demiurg/resource_base.h>

#include "config.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Движковый конфиг приложения как demiurg-ресурс. Живёт в ОТДЕЛЬНОМ (движковом,
// незаменяемом) resource_system — папка resources/engine/config/ — изолированном от
// игрового реестра модов (см. demiurg 1a, Q2). CPU-only (warm_and_hot_same):
//   cold→warm (load_cold): читает текст через demiurg-модуль и парсит tavl в app_config.
// Заменяет прежний сырой file_io в config.cpp::load_app_config.
class app_config_resource : public demiurg::resource_interface {
public:
  app_config_resource();

  const app_config& config() const noexcept { return cfg; }

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

private:
  app_config cfg;
};

}
}

#endif
