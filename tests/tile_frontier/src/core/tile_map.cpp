#include "tile_map.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace tile_frontier {
namespace core {

void tile_grid::resize(const uint32_t w, const uint32_t h) {
  width = w;
  height = h;
  tiles.assign(size_t(w) * size_t(h), tile{});
}

tile_chunk generate_mock_chunk(const chunk_coord coord, const uint32_t chunk_size, const uint32_t texture_count) {
  tile_chunk chunk;
  chunk.coord = coord;
  chunk.size = chunk_size;
  chunk.tiles.assign(size_t(chunk_size) * chunk_size, tile{});

  const uint32_t tex_count = std::max(texture_count, 1u);
  const int32_t base_x = coord.x * int32_t(chunk_size);
  const int32_t base_y = coord.y * int32_t(chunk_size);

  for (uint32_t y = 0; y < chunk_size; ++y) {
    for (uint32_t x = 0; x < chunk_size; ++x) {
      const int32_t wx = base_x + int32_t(x);
      const int32_t wy = base_y + int32_t(y);

      // Достаточно разнообразный, но стабильный паттерн: крупные полосы + локальный шум от coord.
      const uint32_t bands = uint32_t((wx / 4) + (wy / 7));
      const uint32_t ux = uint32_t(wx);
      const uint32_t uy = uint32_t(wy);
      const uint32_t ucx = uint32_t(coord.x);
      const uint32_t ucy = uint32_t(coord.y);
      const uint32_t noise = (ucx * 73856093u) ^ (ucy * 19349663u) ^ (ux * 83492791u) ^ (uy * 2654435761u);
      chunk.at(x, y).texture = (bands + (noise >> 29)) % tex_count;
    }
  }

  return chunk;
}

void apply_chunk(tile_grid& grid, const tile_chunk& chunk) {
  if (!chunk.valid() || grid.width == 0 || grid.height == 0) return;

  const int32_t base_x = chunk.coord.x * int32_t(chunk.size);
  const int32_t base_y = chunk.coord.y * int32_t(chunk.size);

  for (uint32_t y = 0; y < chunk.size; ++y) {
    const int32_t gy = base_y + int32_t(y);
    if (gy < 0 || gy >= int32_t(grid.height)) continue;

    for (uint32_t x = 0; x < chunk.size; ++x) {
      const int32_t gx = base_x + int32_t(x);
      if (gx < 0 || gx >= int32_t(grid.width)) continue;
      grid.at(uint32_t(gx), uint32_t(gy)) = chunk.at(x, y);
    }
  }
}

glm::mat4 camera2d::view_proj() const {
  // Ортопроекция мирового прямоугольника во view rect камеры. Z игнорируем (плоская карта),
  // берём широкий диапазон [-1,1]. Соглашение clip-пространства (Y вниз / depth 0..1) под
  // конкретный бэкенд painter ещё предстоит сверить на render-стороне — это заготовка.
  const glm::vec2 mn = view_min();
  const glm::vec2 mx = view_max();
  return glm::ortho(mn.x, mx.x, mn.y, mx.y, -1.0f, 1.0f);
}

tile_span visible_tiles(const camera2d& cam, const tile_grid& grid, const float margin_tiles) {
  if (grid.width == 0 || grid.height == 0 || grid.tile_size <= 0.0f) return {};

  const glm::vec2 mn = cam.view_min();
  const glm::vec2 mx = cam.view_max();
  const float inv = 1.0f / grid.tile_size;

  // мировой AABB камеры -> индексы тайлов; floor для нижней границы, ceil для верхней,
  // плюс запас margin_tiles по краям. Тайл (x,y) занимает [x*size,(x+1)*size).
  float fx0 = std::floor(mn.x * inv - margin_tiles);
  float fy0 = std::floor(mn.y * inv - margin_tiles);
  float fx1 = std::ceil (mx.x * inv + margin_tiles);
  float fy1 = std::ceil (mx.y * inv + margin_tiles);

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
} // namespace tile_frontier
