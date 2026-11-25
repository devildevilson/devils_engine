#ifndef FRONTIER_ONLINE_CORE_TILE_MAP_H
#define FRONTIER_ONLINE_CORE_TILE_MAP_H

#include <cstdint>

#include <span>

#include <devils_engine/simul/camera.h>
#include <glm/glm.hpp>

#include "core/terrain.h" // причинная модель земли: код рельефа, чанк, сетка

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
// Камера — движковый тип (simul/camera.h): тип в конфиге мира, состояние на главном потоке.
using camera2d = devils_engine::simul::camera2d;
using devils_engine::simul::screen_to_world;

// Срез сетки, попадающий во view rect камеры. margin_tiles — запас по краям (в тайлах),
// чтобы тайлы, частично заехавшие в кадр, не пропадали. Результат клампится к границам сетки.
tile_span visible_tiles(const camera2d& cam, const tile_grid& grid, const float margin_tiles = 1.0f);
// Срез под мировой AABB. Рендер рисует камеру между опубликованными снапшотами: отбирать по
// simul::published_cameras::bounds(текущая), а не по одной текущей камере.
tile_span visible_tiles(const devils_engine::simul::view_bounds& view, const tile_grid& grid,
                        const float margin_tiles = 1.0f);

// Сколько чанков покрывает бокс мира [.., max] целиком: край бокса не обязан делиться на чанк.
// Хотя бы один чанк по каждой оси.
glm::uvec2 chunks_covering(glm::vec2 box_max, uint32_t chunk_size, float tile_size);

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
