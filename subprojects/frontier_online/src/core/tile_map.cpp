#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "core/tile_map.h"

// Реализация презентационной стороны карты. Модель земли (resize/apply_chunk/генерация) живёт в
// terrain.cpp и сюда не заглядывает.

namespace frontier_online {
namespace core {

tile_span visible_tiles(const camera2d& cam, const tile_grid& grid, const float margin_tiles) {
  return visible_tiles(devils_engine::simul::view_bounds{cam.view_min(), cam.view_max()}, grid, margin_tiles);
}

tile_span visible_tiles(const devils_engine::simul::view_bounds& view, const tile_grid& grid, const float margin_tiles) {
  if (grid.width == 0 || grid.height == 0 || grid.tile_size <= 0.0f) {
    return {};
  }

  const glm::vec2 mn = view.min;
  const glm::vec2 mx = view.max;
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

glm::uvec2 chunks_covering(const glm::vec2 box_max, const uint32_t chunk_size, const float tile_size) {
  const float chunk_extent = float(chunk_size) * tile_size;
  const auto over = [chunk_extent](const float edge) {
    return uint32_t(std::max(1.0f, std::ceil(edge / chunk_extent)));
  };
  return {over(box_max.x), over(box_max.y)};
}

} // namespace core
} // namespace frontier_online
