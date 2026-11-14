#ifndef FRONTIER_ONLINE_CORE_APP_CONFIG_RESOURCE_H
#define FRONTIER_ONLINE_CORE_APP_CONFIG_RESOURCE_H

#include <devils_engine/demiurg/resource_base.h>

#include "config.h"

namespace frontier_online {
namespace core {

using namespace devils_engine;

// Один из engine/project/debug documents как demiurg-ресурс. Живёт в ОТДЕЛЬНОМ (движковом,
// незаменяемом) resource_system — папка resources/engine/config/ — изолированном от
// игрового реестра модов (см. demiurg 1a, Q2). CPU-only (warm_and_hot_same):
//   load_cold читает свой документ; apply_to переносит только принадлежащие ему секции.
// Заменяет прежний сырой file_io в config.cpp::load_app_config.
class app_config_resource : public demiurg::resource_interface {
public:
  app_config_resource();

  const app_config& config() const noexcept {
    return cfg;
  }

  void apply_to(app_config& target) const;

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

private:
  app_config cfg;
};

} // namespace core
} // namespace frontier_online

#endif
