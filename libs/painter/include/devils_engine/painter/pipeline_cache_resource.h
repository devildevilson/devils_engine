#ifndef DEVILS_ENGINE_PAINTER_PIPELINE_CACHE_RESOURCE_H
#define DEVILS_ENGINE_PAINTER_PIPELINE_CACHE_RESOURCE_H

#include <cstdint>
#include <vector>
#include "devils_engine/demiurg/resource_base.h"

namespace devils_engine {
namespace painter {

// Блоб VkPipelineCache как demiurg-ресурс (Фаза 2: конфиг painter на рельсах demiurg).
// CPU-only, бинарь. Живёт в ОТДЕЛЬНОМ demiurg-модуле над writable cache-папкой (НЕ бандлится
// CMake). Раздаёт кэш на load; запись (dump) идёт на диск напрямую через
// graphics_base::dump_cache_on_disk — round-trip естественный (модуль ре-сканится каждый init).
class pipeline_cache_resource : public demiurg::resource_interface {
public:
  std::vector<uint8_t> memory;

  pipeline_cache_resource();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
};

}
}

#endif
