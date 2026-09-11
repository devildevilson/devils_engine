#ifndef FRONTIER_ONLINE_CORE_TILE_MAP_H
#define FRONTIER_ONLINE_CORE_TILE_MAP_H

#include <cstdint>

#include <glm/glm.hpp>

#include "terrain.h" // причинная модель земли: код рельефа, чанк, сетка

// ПРЕЗЕНТАЦИОННАЯ сторона карты: как причинную сетку смотрят и во что её превращают для GPU.
//  - camera2d     : ортографическая top-down камера; её «фрустум» = мировой прямоугольник
//  - tile_span    : прямоугольный срез сетки (пересечение view rect с сеткой)
//  - tile_instance: то, что уедет на GPU одним инстансом (layout "v2ui1")
//
// Сама земля (tile / tile_grid / tile_chunk / terrain_source) живёт в terrain.h и про рендер не
// знает: клетка определяется КОДОМ РЕЛЬЕФА, а текстура — его отображение, и выбирается она здесь.

namespace frontier_online {
namespace core {

// Мировой центр тайла. Начало сетки в (0,0), тайл (x,y) занимает [x*size,(x+1)*size).
inline glm::vec2 tile_world_center(const tile_grid& grid, const uint32_t x,
                                   const uint32_t y) noexcept {
  return glm::vec2((float(x) + 0.5f) * grid.tile_size, (float(y) + 0.5f) * grid.tile_size);
}

// Мировой размер всей сетки.
inline glm::vec2 grid_world_extent(const tile_grid& grid) noexcept {
  return glm::vec2(float(grid.width), float(grid.height)) * grid.tile_size;
}

struct tile_span {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t width() const noexcept {
    return x1 - x0;
  }
  uint32_t height() const noexcept {
    return y1 - y0;
  }
  uint32_t count() const noexcept {
    return width() * height();
  }
  bool empty() const noexcept {
    return x1 <= x0 || y1 <= y0;
  }
};

// Ортографическая top-down камера. center — точка в мире, на которую смотрим;
// half_width — половина видимой ширины в мировых единицах (зум). Высота берётся из aspect.
struct camera2d {
  glm::vec2 center{0.0f, 0.0f};
  float half_width = 8.0f;
  float aspect = 16.0f / 9.0f; // width/height вьюпорта

  float half_height() const noexcept {
    return half_width / aspect;
  }

  // Видимая область как мировой AABB. min = левый-нижний угол, max = правый-верхний.
  glm::vec2 view_min() const noexcept {
    return center - glm::vec2(half_width, half_height());
  }
  glm::vec2 view_max() const noexcept {
    return center + glm::vec2(half_width, half_height());
  }

  // Матрица world(xy) -> clip. Понадобится render-стороне (шейдер трансформирует tile_instance.pos).
  glm::mat4 view_proj() const;
};

// Window-space cursor (origin top-left) -> world point. Uses logical window dimensions, not
// framebuffer pixels, so HiDPI cursor coordinates remain consistent.
inline glm::vec2 screen_to_world(
  const camera2d& cam, const glm::vec2 cursor, const glm::vec2 window_size) noexcept {
  const glm::vec2 safe_size = glm::max(window_size, glm::vec2{1.0f, 1.0f});
  const glm::vec2 unit = glm::clamp(cursor / safe_size, glm::vec2{0.0f}, glm::vec2{1.0f});
  return cam.view_min() + unit * (cam.view_max() - cam.view_min());
}

// Срез сетки, попадающий во view rect камеры. margin_tiles — запас по краям (в тайлах),
// чтобы тайлы, частично заехавшие в кадр, не пропадали. Результат клампится к границам сетки.
tile_span visible_tiles(const camera2d& cam, const tile_grid& grid, const float margin_tiles = 1.0f);

// Один инстанс тайла на GPU. ДОЛЖЕН байт-в-байт соответствовать layout "v2ui1":
//   pos     -> v2  (glm::vec2, SFLOAT x2, 8B)
//   texture -> ui1 (uint32,    UINT,      4B)
// sizeof = 12, паддинга нет (см. instance_layout::gpu_atom_size). pos — мировой центр тайла;
// трансформацию в clip делает шейдер по camera2d::view_proj.
struct tile_instance {
  glm::vec2 pos;
  uint32_t texture;
};

} // namespace core
} // namespace frontier_online

#endif
