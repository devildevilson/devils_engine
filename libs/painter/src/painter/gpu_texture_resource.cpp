#include <devils_engine/catalogue/logging.h>
#include "gpu_texture_resource.h"

#include <span>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>

#include "assets_base.h"
#include "gpu_load_context.h"

#include <vulkan/vulkan_core.h>

namespace devils_engine {
namespace painter {

gpu_texture_resource::gpu_texture_resource() {
  // GPU-ресурс: warm->hot делает рендер (не warm_and_hot_same).
  set_flag(demiurg::resource_flags::warm_and_hot_same, false);
  set_flag(demiurg::resource_flags::binary, true);
}

// База не знает, откуда байты — их наполняет подкласс (png-декод / MSDF-шаг).
void gpu_texture_resource::load_cold(const utils::safe_handle_t&) {}

void gpu_texture_resource::load_warm(const utils::safe_handle_t& handle) {
  auto* ctx = handle.get<gpu_load_context>();

  const auto h = ctx->assets->register_texture_storage(path);
  texture_create_info tci{};
  tci.extents = { width, height, 1 };
  tci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ctx->assets->create_texture_storage(h, tci);
  ctx->assets->populate_texture_storage(h, std::span<const uint8_t>(memory));
  ctx->assets->mark_ready_texture_slot(h);

  gpu_index = h;

  // На GPU — CPU-копия больше не нужна.
  memory.clear();
  memory.shrink_to_fit();

  DE_LOG(catalogue::log_domain::resource, flow, "gpu_texture_resource '{}': uploaded to GPU, gpu_index(slot)={}", id, gpu_index);
}

void gpu_texture_resource::unload_hot(const utils::safe_handle_t&) {
  // TODO: реальное освобождение GPU-текстуры через render API.
  gpu_index = invalid_gpu_index;
}

void gpu_texture_resource::unload_warm(const utils::safe_handle_t&) {
  memory.clear();
  memory.shrink_to_fit();
}

}
}
