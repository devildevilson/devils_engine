#ifndef TILE_FRONTIER_CORE_TEXTURE_SET_H
#define TILE_FRONTIER_CORE_TEXTURE_SET_H

#include <cstdint>
#include <string_view>
#include <vector>

#include "texture_resource.h"

namespace devils_engine { namespace demiurg { class resource_system; } }

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Набор текстур карты, собранный из реестра ассетов ПО ПРЕФИКСУ ПУТИ (id ресурса).
// Индекс в textures == tile_instance.texture == слот в дескриптор-массиве на GPU
// (gpu_index текстуры). Порядок детерминирован порядком в реестре (отсортирован по id).
struct texture_set {
  std::vector<texture_resource*> textures;

  uint32_t count() const noexcept { return uint32_t(textures.size()); }

  // Собрать все texture_resource, чей id содержит prefix как подстроку (напр. "textures/"),
  // через resource_system::filter ("по части пути"). Возвращает число найденных. Перезаписывает.
  uint32_t gather(demiurg::resource_system& reg, const std::string_view& prefix);

  // gpu_index i-й текстуры (invalid_gpu_index, если ещё не загружена на GPU).
  uint32_t gpu_index(const uint32_t i) const noexcept {
    return i < textures.size() ? textures[i]->gpu_index : texture_resource::invalid_gpu_index;
  }
};

} // namespace core
} // namespace tile_frontier

#endif
