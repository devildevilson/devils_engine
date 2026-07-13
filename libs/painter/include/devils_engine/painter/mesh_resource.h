#ifndef DEVILS_ENGINE_PAINTER_MESH_RESOURCE_H
#define DEVILS_ENGINE_PAINTER_MESH_RESOURCE_H

#include <cstdint>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>
#include <devils_engine/utils/shared.h>

namespace devils_engine {
namespace painter {

// Простой mesh-ресурс: сырые вершинные байты с диска -> GPU-буфер (assets_base).
//   cold->warm (load_cold, поток ассетов): читает байты через demiurg-модуль;
//   warm->hot  (load_warm, поток рендера): заливает в GPU, пишет gpu_index, освобождает CPU-копию.
class mesh_resource : public demiurg::resource_interface {
public:
  static constexpr uint32_t invalid_gpu_index = utils::shared::invalid_gpu_index;
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

} // namespace painter
} // namespace devils_engine

#endif
