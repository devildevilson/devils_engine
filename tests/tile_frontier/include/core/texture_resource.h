#ifndef TILE_FRONTIER_CORE_TEXTURE_RESOURCE_H
#define TILE_FRONTIER_CORE_TEXTURE_RESOURCE_H

#include <cstdint>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>

#include "gpu_load_context.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Текстура-ресурс под MVP: png с диска → декод stb (RGBA8) → GPU-текстура.
//   cold→warm (load_cold, поток ассетов): читает png через demiurg-модуль, декодит в RGBA;
//   warm→hot  (load_warm, поток рендера): заливает в assets_base texture storage, пишет
//             gpu_index = слот текстуры и освобождает CPU-копию.
// gpu_index = индекс texture_slot в assets_base; он же станет индексом в дескриптор-массиве.
class texture_resource : public demiurg::resource_interface {
public:
  static constexpr uint32_t invalid_gpu_index = ~uint32_t(0);

  std::vector<uint8_t> memory; // декодированные RGBA-байты (width*height*4)
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t gpu_index = invalid_gpu_index;

  texture_resource();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
};

}
}

#endif
