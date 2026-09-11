#ifndef FRONTIER_ONLINE_CORE_TERRAIN_H
#define FRONTIER_ONLINE_CORE_TERRAIN_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// ЗЕМЛЯ — ПРИЧИННАЯ ВЕЛИЧИНА И ЧИСТАЯ ФУНКЦИЯ.
//
// Клетка определяется КОДОМ РЕЛЬЕФА, а не текстурой. Раньше здесь лежал
// `demiurg::resource_handle` — а внутри него сырой указатель на реестр: такую клетку нельзя ни
// сохранить, ни передать по проводу, и у авторитета реестра текстур нет вовсе. Текстура — это
// презентационное отображение кода, и живёт оно на стороне клиента.
//
// Чанк — функция (зерно мира, ключ чанка) и НИЧЕГО больше. Поэтому по сети земля не едет: обе
// стороны считают её сами, а на рукопожатие уходят только зерно и отпечаток генератора. Отсюда же
// требование к самому генератору: результат чанка не зависит от того, какие чанки посчитаны
// раньше.

namespace devils_engine {
namespace demiurg {
class resource_system;
}
namespace thread {
class atomic_pool;
}
} // namespace devils_engine

namespace frontier_online {
namespace core {

// Код рельефа. Узкий намеренно: он едет в состоянии, и его ширина — это цена клетки.
// Значения задаёт генератор; сейчас классификация даёт 0 вода, 1 сухая суша, 2 обычная, 3 влажная.
using terrain_code = uint8_t;

inline constexpr terrain_code terrain_unknown = 0xff;

struct tile {
  terrain_code terrain = terrain_unknown;
};

struct chunk_coord {
  int32_t x = 0;
  int32_t y = 0;

  bool operator==(const chunk_coord&) const noexcept = default;
};

// CPU-представление одного квадратного чанка. tiles.size() == size*size, row-major.
// Чанк (cx,cy) покрывает глобальные тайлы:
//   x in [cx*size, (cx+1)*size), y in [cy*size, (cy+1)*size)
struct tile_chunk {
  chunk_coord coord{};
  uint32_t size = 0;
  std::vector<tile> tiles;

  bool valid() const noexcept {
    return size != 0 && tiles.size() == size_t(size) * size;
  }
  tile& at(const uint32_t x, const uint32_t y) noexcept {
    return tiles[size_t(y) * size + x];
  }
  const tile& at(const uint32_t x, const uint32_t y) const noexcept {
    return tiles[size_t(y) * size + x];
  }
};

// Плоская сетка тайлов. Мировые координаты: центр тайла = ((x+0.5)*tile_size, (y+0.5)*tile_size).
//
// ОГРАНИЧЕНИЕ, которое ещё предстоит снять: сетка КОНЕЧНА и начинается в (0,0), а apply_chunk молча
// отбрасывает всё, что за границей. Для стриминга во все стороны нужно окно чанков вокруг
// наблюдателя и координаты относительно чанка камеры (урок GN03), иначе далеко от начала мира
// точность float просто кончится.
struct tile_grid {
  uint32_t width = 0;
  uint32_t height = 0;
  float tile_size = 1.0f;
  std::vector<tile> tiles; // row-major, размер = width*height

  void resize(uint32_t w, uint32_t h);
  bool in_bounds(const uint32_t x, const uint32_t y) const noexcept {
    return x < width && y < height;
  }
  tile& at(const uint32_t x, const uint32_t y) noexcept {
    return tiles[size_t(y) * width + x];
  }
  const tile& at(const uint32_t x, const uint32_t y) const noexcept {
    return tiles[size_t(y) * width + x];
  }
};

// Скопировать чанк в сетку. Часть чанка за границей сетки отбрасывается.
void apply_chunk(tile_grid& grid, const tile_chunk& chunk);

// Генератор земли: пайплайн originator, объявленный в конфиге, прогоняемый по одному чанку.
//
// ОДИН ИСТОЧНИК — ОДИН ПОТОК. Пайплайн переиспользует свои буферы между чанками (в этом и смысл
// set_chunk: не платить аллокацией за каждый чанк), поэтому два потока не могут делить один
// источник. Нужен второй поток — нужен второй источник; они независимы по построению, потому что
// чанк не зависит ни от чего, кроме зерна и ключа.
class terrain_source {
public:
  // entry_id — demiurg-id точки входа генератора (например "generator/world"). Размер чанка и
  // зерно мира приходят от хоста: генератор объявляет ФОРМУ, а масштаб выбирает мир.
  terrain_source(const devils_engine::demiurg::resource_system& resources,
                 std::string_view entry_id, uint32_t chunk_size, uint64_t world_seed,
                 devils_engine::thread::atomic_pool* pool = nullptr);
  ~terrain_source() noexcept;

  terrain_source(const terrain_source&) = delete;
  terrain_source& operator=(const terrain_source&) = delete;

  uint32_t chunk_size() const noexcept {
    return chunk_size_;
  }
  uint64_t world_seed() const noexcept {
    return world_seed_;
  }

  // ОТПЕЧАТОК МИРА: зерно, размер чанка и тексты всех документов генератора, свёрнутые в одно
  // число. Он и есть то, что едет на рукопожатии вместо самой земли: две стороны с разными
  // отпечатками считают РАЗНЫЕ миры, и узнать это надо до первого тика, а не по картинке.
  uint64_t fingerprint() const noexcept {
    return fingerprint_;
  }

  // Чанк по ключу. Чистая функция (зерно, ключ): повторный вызов с тем же ключом обязан дать те же
  // байты, и порядок вызовов на результат не влияет.
  tile_chunk generate(chunk_coord coord);

  // Сырое поле ПОСЛЕДНЕГО посчитанного чанка по имени и номеру клетки.
  //
  // Существует ради КАЛИБРОВКИ. Перевод выхода шума в диапазон, где заданы пороги, обязан быть
  // ОДИНАКОВЫМ для всех чанков (иначе соседи меряют разными линейками), то есть константой — а
  // константу нельзя подобрать, глядя на итоговую картинку: подкрутка порога и подкрутка масштаба
  // дают похожий результат, и какая величина не та, по картинке не видно. Поэтому диапазон поля
  // измеряется прямо, один раз, и вписывается в конфиг числом.
  double raw_field(std::string_view field_name, size_t element) const;

private:
  struct state;

  std::unique_ptr<state> state_;
  uint32_t chunk_size_ = 0;
  uint64_t world_seed_ = 0;
  uint64_t fingerprint_ = 0;
};

} // namespace core
} // namespace frontier_online

#endif
