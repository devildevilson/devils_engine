#ifndef TILE_FRONTIER_CORE_TILE_MAP_H
#define TILE_FRONTIER_CORE_TILE_MAP_H

#include <cstdint>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

// Главная (геймплейная) сторона: модель мира под рендер тайловой карты.
//  - tile        : одна клетка (пока хранит только индекс текстуры)
//  - tile_grid   : плоская квадратная сетка W x H, row-major, мировые координаты
//  - tile_chunk  : CPU payload одного чанка; пока генерируется mock-ассетами
//  - texture_set : набор текстур, собранный из реестра ассетов по префиксу пути
//  - camera2d    : ортографическая top-down камера; её "фрустум" = мировой прямоугольник
//  - tile_span   : прямоугольный срез сетки (пересечение view rect с сеткой)
//  - tile_instance : то, что уедет на GPU одним инстансом (layout "v2ui1")
//
// Никакой зависимости от painter здесь нет — это чистая модель на glm. Упаковка в байты
// (draw_intent) живёт отдельно в tile_batch.h.

namespace tile_frontier {
namespace core {

// Одна клетка карты. Пока для рендера нужен только индекс текстуры в texture_set.
struct tile {
  uint32_t texture = 0;
};

// Плоская квадратная сетка тайлов. Тайл (x,y), x в [0,width), y в [0,height).
// Мировые координаты: центр тайла = ((x+0.5)*tile_size, (y+0.5)*tile_size). Начало в (0,0).
struct tile_grid {
  uint32_t width = 0;
  uint32_t height = 0;
  float tile_size = 1.0f;
  std::vector<tile> tiles; // row-major, размер = width*height

  void resize(const uint32_t w, const uint32_t h);
  bool in_bounds(const uint32_t x, const uint32_t y) const noexcept { return x < width && y < height; }
  tile& at(const uint32_t x, const uint32_t y) noexcept { return tiles[size_t(y) * width + x]; }
  const tile& at(const uint32_t x, const uint32_t y) const noexcept { return tiles[size_t(y) * width + x]; }

  glm::vec2 world_center(const uint32_t x, const uint32_t y) const noexcept {
    return glm::vec2((float(x) + 0.5f) * tile_size, (float(y) + 0.5f) * tile_size);
  }
  // мировой размер всей карты
  glm::vec2 world_extent() const noexcept { return glm::vec2(float(width), float(height)) * tile_size; }
};

struct chunk_coord {
  int32_t x = 0;
  int32_t y = 0;
};

// CPU-представление одного квадратного чанка. tiles.size() == size*size, row-major.
// Чанк (cx,cy) покрывает глобальные тайлы:
//   x in [cx*size, (cx+1)*size), y in [cy*size, (cy+1)*size)
struct tile_chunk {
  chunk_coord coord{};
  uint32_t size = 0;
  std::vector<tile> tiles;

  bool valid() const noexcept { return size != 0 && tiles.size() == size_t(size) * size; }
  tile& at(const uint32_t x, const uint32_t y) noexcept { return tiles[size_t(y) * size + x]; }
  const tile& at(const uint32_t x, const uint32_t y) const noexcept { return tiles[size_t(y) * size + x]; }
};

// Mock "asset load": детерминированно генерирует содержимое чанка на CPU. Реальная версия позже
// заменит тело на чтение/декод demiurg-ресурса, но contract останется тем же: coord -> tile_chunk.
tile_chunk generate_mock_chunk(chunk_coord coord, uint32_t chunk_size, uint32_t texture_count);

// Скопировать чанк в глобальную сетку. Часть чанка за границей grid молча отбрасывается.
void apply_chunk(tile_grid& grid, const tile_chunk& chunk);

// Прямоугольный срез сетки: полуоткрытый диапазон [x0,x1) x [y0,y1).
struct tile_span {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  uint32_t width() const noexcept { return x1 - x0; }
  uint32_t height() const noexcept { return y1 - y0; }
  uint32_t count() const noexcept { return width() * height(); }
  bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
};

// Ортографическая top-down камера. center — точка в мире, на которую смотрим;
// half_width — половина видимой ширины в мировых единицах (зум). Высота берётся из aspect.
struct camera2d {
  glm::vec2 center{0.0f, 0.0f};
  float half_width = 8.0f;
  float aspect = 16.0f / 9.0f; // width/height вьюпорта

  float half_height() const noexcept { return half_width / aspect; }

  // Видимая область как мировой AABB. min = левый-нижний угол, max = правый-верхний.
  glm::vec2 view_min() const noexcept { return center - glm::vec2(half_width, half_height()); }
  glm::vec2 view_max() const noexcept { return center + glm::vec2(half_width, half_height()); }

  // Матрица world(xy) -> clip. Понадобится render-стороне (шейдер трансформирует tile_instance.pos).
  glm::mat4 view_proj() const;
};

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
} // namespace tile_frontier

#endif
