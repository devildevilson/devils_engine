#ifndef TILE_FRONTIER_CORE_MESH_RESOURCE_H
#define TILE_FRONTIER_CORE_MESH_RESOURCE_H

#include <cstdint>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>

namespace devils_engine { namespace painter { class assets_base; class graphics_base; } }

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Контекст GPU-стороны: поток рендера передаёт его в load_warm/unload_hot через handle
// (см. resource_loader — warm↔hot исполняет рендер, а не поток ассетов).
struct gpu_load_context {
  painter::assets_base* assets;
  painter::graphics_base* base;
};

// Простой mesh-ресурс под MVP: сырые вершинные байты с диска → GPU-буфер.
//   cold→warm (load_cold, поток ассетов): читает байты через demiurg-модуль;
//   warm→hot  (load_warm, поток рендера): заливает в GPU, пишет gpu_index и
//             освобождает CPU-копию (память больше не нужна — она на GPU);
//   unload_*  : обратные переходы.
// Полный контракт структуры GPU-ресурса — позже; пока локально в tile_frontier.
class mesh_resource : public demiurg::resource_interface {
public:
  static constexpr uint32_t invalid_gpu_index = ~uint32_t(0);
  static constexpr uint32_t vertex_stride = 16; // float3 + uint32 rgba (как у тест-треугольника)

  std::vector<uint8_t> memory;
  uint32_t gpu_index = invalid_gpu_index;
  uint32_t vertex_count = 0;

  mesh_resource();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
};

}
}

#endif
