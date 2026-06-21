#include "texture_resource.h"

#include <vector>
#include <span>

#include <stb_image.h>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/painter/assets_base.h>

#include <vulkan/vulkan_core.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

texture_resource::texture_resource() {
  // GPU-ресурс: warm→hot делает рендер (не warm_and_hot_same).
  set_flag(demiurg::resource_flags::warm_and_hot_same, false);
  set_flag(demiurg::resource_flags::binary, true);
}

void texture_resource::load_cold(const utils::safe_handle_t& handle) {
  std::vector<uint8_t> raw;
  module->load_binary(path, raw);

  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(raw.data(), int(raw.size()), &w, &h, &channels, STBI_rgb_alpha);
  if (pixels == nullptr) {
    utils::error{}("texture_resource '{}': stb failed to decode ({})", id, stbi_failure_reason());
    return;
  }

  width = uint32_t(w);
  height = uint32_t(h);
  const size_t bytes = size_t(width) * size_t(height) * 4;
  memory.assign(pixels, pixels + bytes);
  stbi_image_free(pixels);

  utils::info("texture_resource '{}': decoded {}x{} ({} bytes RGBA)", id, width, height, memory.size());
}

void texture_resource::load_warm(const utils::safe_handle_t& handle) {
  auto* ctx = handle.get<gpu_load_context>();

  const auto h = ctx->assets->register_texture_storage(path);
  painter::texture_create_info tci{};
  tci.extents = { width, height, 1 };
  tci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ctx->assets->create_texture_storage(h, tci);
  ctx->assets->populate_texture_storage(h, std::span<const uint8_t>(memory));
  ctx->assets->mark_ready_texture_slot(h);

  gpu_index = h;

  // На GPU — CPU-копия больше не нужна.
  memory.clear();
  memory.shrink_to_fit();

  utils::info("texture_resource '{}': uploaded to GPU, gpu_index(slot)={}", id, gpu_index);
}

void texture_resource::unload_hot(const utils::safe_handle_t& handle) {
  // TODO: реальное освобождение GPU-текстуры через render API.
  gpu_index = invalid_gpu_index;
}

void texture_resource::unload_warm(const utils::safe_handle_t& handle) {
  memory.clear();
  memory.shrink_to_fit();
}

}
}
