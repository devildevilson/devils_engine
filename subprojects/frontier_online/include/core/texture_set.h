#ifndef FRONTIER_ONLINE_CORE_TEXTURE_SET_H
#define FRONTIER_ONLINE_CORE_TEXTURE_SET_H

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/painter/gpu_texture_resource.h>

namespace frontier_online {
namespace core {

using namespace devils_engine;

// Набор текстур карты, собранный из реестра ассетов ПО ПРЕФИКСУ ПУТИ (id ресурса).
// Тайлы хранят stable handles из этого palette. gpu_index вычисляется поздно, при сборке
// render payload, поэтому порядок фактической GPU-загрузки не влияет на мир.
struct texture_set {
  std::vector<demiurg::resource_handle> textures;

  uint32_t count() const noexcept {
    return uint32_t(textures.size());
  }
  std::span<const demiurg::resource_handle> handles() const noexcept {
    return textures;
  }

  // Собрать все texture_resource, чей id содержит prefix как подстроку (напр. "textures/"),
  // через resource_system::filter ("по части пути"). Возвращает число найденных. Перезаписывает.
  uint32_t gather(demiurg::resource_system& reg, const std::string_view& prefix);

  // Manifest path: сохранить явно перечисленную palette, отбрасывая не-текстуры.
  uint32_t assign(std::span<const demiurg::resource_handle> handles);

  // Разрешить logical handle тайла в bindless GPU slot. Чужой или неготовый ресурс invalid.
  uint32_t gpu_index(demiurg::resource_handle handle) const noexcept;

  // Код рельефа -> bindless GPU slot. ЗДЕСЬ и проходит граница причинного и презентационного:
  // генератор выдаёт код, палитра решает, чем его показать.
  //
  // Отображение ПОЗИЦИОННОЕ, и порядок палитры объявлен конфигом: группа tile_textures в манифесте
  // сцены перечисляет текстуры в порядке кодов рельефа. Значит «чем показать воду» меняется правкой
  // манифеста, без C++ и без генератора. Код вне палитры сворачивается по модулю, а не роняет кадр:
  // генератор вправе завести новый класс раньше, чем художник — картинку для него.
  uint32_t gpu_index_for_terrain(uint32_t terrain) const noexcept {
    if (textures.empty()) return uint32_t(-1);
    return gpu_index(textures[terrain % textures.size()]);
  }
};

} // namespace core
} // namespace frontier_online

#endif
