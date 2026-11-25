#include <algorithm>

#include <devils_engine/demiurg/resource_system.h>

#include "core/texture_set.h"

namespace frontier_online {
namespace core {

uint32_t texture_set::assign(const std::span<const demiurg::resource_handle> handles) {
  textures.clear();
  textures.reserve(handles.size());
  for (const auto handle : handles) {
    if (handle.get<painter::gpu_texture_resource>() != nullptr) {
      textures.push_back(handle);
    }
  }
  return uint32_t(textures.size());
}

uint32_t texture_set::gpu_index(const demiurg::resource_handle handle) const noexcept {
  const bool belongs_to_palette = std::find_if(textures.begin(), textures.end(), [handle](const auto cur) {
                                    return cur.system == handle.system && cur.hash == handle.hash;
                                  }) != textures.end();
  if (!belongs_to_palette) {
    return painter::gpu_texture_resource::invalid_gpu_index;
  }

  auto* texture = handle.get<painter::gpu_texture_resource>();
  if (texture == nullptr || !texture->usable()) {
    return painter::gpu_texture_resource::invalid_gpu_index;
  }
  return texture->gpu_index;
}

} // namespace core
} // namespace frontier_online
