#ifndef DEVILS_ENGINE_PAINTER_GPU_TEXTURE_RESOURCE_H
#define DEVILS_ENGINE_PAINTER_GPU_TEXTURE_RESOURCE_H

#include <cstdint>
#include <vector>

#include "devils_engine/demiurg/resource_base.h"

namespace devils_engine {
namespace painter {

// Базовый GPU-текстурный ресурс: RGBA-байты (memory) + размеры -> слот таблицы текстур assets_base.
// gpu_index = индекс texture_slot (он же индекс в дескриптор-массиве шейдера). Откуда берутся байты
// (png-декод / MSDF-генерация) — дело подкласса (texture_resource / visage::font_resource): база
// даёт только GPU-заливку. load_cold — no-op (байты наполняет подкласс/шаги). warm->hot (load_warm)
// исполняет поток рендера (не warm_and_hot_same). Всё, что нужно потребителю — gpu_index.
class gpu_texture_resource : public demiurg::resource_interface {
public:
  static constexpr uint32_t invalid_gpu_index = ~uint32_t(0);

  std::vector<uint8_t> memory; // RGBA8-байты (width*height*4)
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t gpu_index = invalid_gpu_index;

  gpu_texture_resource();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;
};

}
}

#endif
