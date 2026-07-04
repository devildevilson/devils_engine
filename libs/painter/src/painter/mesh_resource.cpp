#include "mesh_resource.h"

#include <span>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <devils_engine/demiurg/module_interface.h>

#include "assets_base.h"
#include "gpu_load_context.h"

namespace devils_engine {
namespace painter {

mesh_resource::mesh_resource() {
  // НЕ warm_and_hot_same: это GPU-ресурс, переход warm->hot делает рендер.
  set_flag(demiurg::resource_flags::warm_and_hot_same, false);
  set_flag(demiurg::resource_flags::binary, true);
}

void mesh_resource::load_cold(const utils::safe_handle_t&) {
  module->load_binary(path, memory);
  vertex_count = uint32_t(memory.size() / vertex_stride);
  utils::info("mesh_resource '{}': loaded {} bytes ({} verts) to memory", id, memory.size(), vertex_count);
}

void mesh_resource::load_warm(const utils::safe_handle_t& handle) {
  auto* ctx = handle.get<gpu_load_context>();

  const auto h = ctx->assets->register_buffer_storage(path);
  buffer_create_info bci{ "g1", vertex_count, 0 };
  ctx->assets->create_buffer_storage(h, bci);
  ctx->assets->populate_buffer_storage(h, std::span<const uint8_t>(memory), std::span<const uint8_t>());
  ctx->assets->mark_ready_buffer_slot(h);

  gpu_index = h;

  // Ресурс теперь на GPU — сырые байты больше не нужны, возвращаем CPU-память.
  memory.clear();
  memory.shrink_to_fit();

  utils::info("mesh_resource '{}': uploaded to GPU, gpu_index={}", id, gpu_index);
}

void mesh_resource::unload_hot(const utils::safe_handle_t&) {
  // TODO: реальное освобождение GPU-буфера через render API.
  gpu_index = invalid_gpu_index;
}

void mesh_resource::unload_warm(const utils::safe_handle_t&) {
  memory.clear();
  memory.shrink_to_fit();
}

}
}
