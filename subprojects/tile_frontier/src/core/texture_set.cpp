#include "texture_set.h"

#include <algorithm>

#include <devils_engine/demiurg/resource_system.h>

namespace tile_frontier {
namespace core {

uint32_t texture_set::gather(demiurg::resource_system& reg, const std::string_view& prefix) {
  textures.clear();
  // filter<T>: подстрочный поиск по id ("по части пути") + фильтр по типу T. Итерирует массив
  // ресурсов напрямую. НЕ используем resource_system::find<T> — он идёт через demiurg::view<>,
  // у которого operator[] возвращает висячую ссылку на временный T* (баг: "returning reference
  // to temporary", resource_system.h:50) → UB/segfault. См. заметку в обзоре.
  std::vector<painter::gpu_texture_resource*> found;
  reg.filter<painter::gpu_texture_resource>(prefix, found);
  textures.reserve(found.size());
  for (auto* texture : found) {
    if (texture != nullptr) textures.push_back(reg.handle(texture->id));
  }
  return uint32_t(textures.size());
}

uint32_t texture_set::gpu_index(const demiurg::resource_handle handle) const noexcept {
  const bool belongs_to_palette = std::find_if(textures.begin(), textures.end(), [handle](const auto cur) {
    return cur.system == handle.system && cur.hash == handle.hash;
  }) != textures.end();
  if (!belongs_to_palette) return painter::gpu_texture_resource::invalid_gpu_index;

  auto* texture = handle.get<painter::gpu_texture_resource>();
  if (texture == nullptr || !texture->usable()) return painter::gpu_texture_resource::invalid_gpu_index;
  return texture->gpu_index;
}

} // namespace core
} // namespace tile_frontier
