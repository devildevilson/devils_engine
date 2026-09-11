#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "tile_map.h"

// Реализация презентационной стороны карты. Модель земли (resize/apply_chunk/генерация) живёт в
// terrain.cpp и сюда не заглядывает.

namespace frontier_online {
namespace core {

glm::mat4 camera2d::view_proj() const {
  // Ортопроекция мирового прямоугольника во view rect камеры. Z игнорируем (плоская карта),
  // берём широкий диапазон [-1,1]. Соглашение clip-пространства (Y вниз / depth 0..1) под
  // конкретный бэкенд painter ещё предстоит сверить на render-стороне — это заготовка.
  const glm::vec2 mn = view_min();
  const glm::vec2 mx = view_max();
  return glm::ortho(mn.x, mx.x, mn.y, mx.y, -1.0f, 1.0f);
}

tile_span visible_tiles(const camera2d& cam, const tile_grid& grid, const float margin_tiles) {
  if (grid.width == 0 || grid.height == 0 || grid.tile_size <= 0.0f) {
    return {};
  }

  const glm::vec2 mn = cam.view_min();
  const glm::vec2 mx = cam.view_max();
  const float inv = 1.0f / grid.tile_size;

  // мировой AABB камеры -> индексы тайлов; floor для нижней границы, ceil для верхней,
  // плюс запас margin_tiles по краям. Тайл (x,y) занимает [x*size,(x+1)*size).
  float fx0 = std::floor(mn.x * inv - margin_tiles);
  float fy0 = std::floor(mn.y * inv - margin_tiles);
  float fx1 = std::ceil(mx.x * inv + margin_tiles);
  float fy1 = std::ceil(mx.y * inv + margin_tiles);

  // кламп к границам сетки (и к нулю снизу), полуоткрытый [x0,x1) x [y0,y1)
  const float w = float(grid.width);
  const float h = float(grid.height);
  const uint32_t x0 = uint32_t(std::clamp(fx0, 0.0f, w));
  const uint32_t y0 = uint32_t(std::clamp(fy0, 0.0f, h));
  const uint32_t x1 = uint32_t(std::clamp(fx1, 0.0f, w));
  const uint32_t y1 = uint32_t(std::clamp(fy1, 0.0f, h));

  return tile_span{x0, y0, x1, y1};
}

} // namespace core
} // namespace frontier_online
