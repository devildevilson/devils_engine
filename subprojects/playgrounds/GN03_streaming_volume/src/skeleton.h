#ifndef DEVILS_ENGINE_GN03_SKELETON_H
#define DEVILS_ENGINE_GN03_SKELETON_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "devils_engine/originator/pipeline.h"

// КАРКАС МИРА: результат ГРУБОГО прохода, из которого чанки берут свою область.
//
// Библиотека объявляет два масштаба с самого начала («грубый мировой проход, маленький, один раз, и
// мелкий чанковый, большой, по требованию»), но до сих пор они ни разу не встречались: поле GN03 —
// чистый шум, каркас ему не нужен. Здесь они сходятся, и на стыке появляется ровно один новый
// вопрос: КАК ЧАНК ПОЛУЧАЕТ СВОЮ ЧАСТЬ КАРКАСА.
//
// Ответ, который сюда и записан: чанк получает не каркас, а ЗАПРОС ПО СВОЕЙ ОБЛАСТИ, и результат
// запроса приезжает обычным буфером с объявленной ёмкостью. Три следствия:
//
//   ЗАПРОС ОБЯЗАН БЫТЬ ПОЛНЫМ. Если он вернёт «то, что успело подгрузиться», один и тот же ключ
//   даст разный мир в зависимости от истории загрузки. Неполный запрос — это не «чуть хуже», это
//   ДРУГОЙ МИР. Поэтому индекс проверяется против полного перебора, а не «выглядит правильно».
//
//   ПОЛЕ ЗАПРОСА ВЫВОДИТСЯ ИЗ РАДИУСА ВЛИЯНИЯ. Отрезок маршрута, проходящий РЯДОМ с чанком, гнёт
//   поле внутри него, поэтому спрашивать надо область чанка плюс радиус влияния. Радиус объявляет
//   каркас (`corridor_radius` + запас), и он же попадает в пакет — иначе читатель пакета не знает,
//   насколько широко спрашивать.
//
//   ПЕРЕПОЛНЕНИЕ ЁМКОСТИ — ОТКАЗ, А НЕ ОБРЕЗКА. Обрезанный маршрут даёт коридор, который кончается
//   в середине горы, и найти причину по картинке нельзя.
//
// Резидентность: здесь каркас держится целиком, и это честно ровно пока он мал (сорок узлов, сотни
// точек). Шов для страничной загрузки — это сам запрос: он уже спрашивает ОБЛАСТЬ, а не «дай всё»,
// поэтому замена «весь каркас в памяти» на «тайлы по требованию» не меняет ни один вызов. Что
// изменится тогда — правило полноты: чанк, для которого нужный тайл ещё не резидентен, обязан быть
// ОТЛОЖЕН, а не посчитан с неполной метой.

namespace devils_engine::gn03 {

struct skeleton_node {
  double position[3]{};
  uint32_t kind = 0;
};

class world_skeleton {
public:
  // Что каркас говорит о себе. Радиус влияния лежит ЗДЕСЬ, а не у читателя: насколько широко
  // спрашивать маршрут, знает тот, кто его строил.
  struct description {
    uint64_t seed = 0;
    double world_span = 0.0;
    double influence = 0.0;
  };

  void build(const description& what, std::vector<skeleton_node> nodes, std::vector<std::array<double, 3>> points,
             std::vector<uint32_t> offsets);

  const description& about() const noexcept { return description_; }
  std::span<const skeleton_node> nodes() const noexcept { return nodes_; }
  std::span<const std::array<double, 3>> points() const noexcept { return points_; }
  std::span<const uint32_t> offsets() const noexcept { return offsets_; }
  bool empty() const noexcept { return points_.size() < 2; }

  // Запрос по области. `low`/`high` — коробка чанка в мировых координатах; поле запроса добавляется
  // внутри из объявленного радиуса влияния, чтобы читатель не мог забыть его прибавить.
  //
  // Возвращает false, если результат не влез в объявленную ёмкость: вызывающий обязан сказать об
  // этом вслух и НЕ считать чанк.
  struct query_result {
    std::vector<std::array<double, 3>> points;
    std::vector<uint32_t> offsets; // CSR: цепочек столько, сколько вернулось
    size_t chains = 0;
  };
  bool query(const std::array<double, 3>& low, const std::array<double, 3>& high, size_t point_capacity,
             size_t chain_capacity, query_result& out) const;

  // Тот же запрос ПОЛНЫМ ПЕРЕБОРОМ, без индекса. Существует ради проверки: индекс обязан находить
  // ровно то же, а «пропущенный отрезок» иначе виден только как коридор, оборвавшийся в горе.
  bool query_exhaustive(const std::array<double, 3>& low, const std::array<double, 3>& high,
                        size_t point_capacity, size_t chain_capacity, query_result& out) const;

  // Пакет на диск и с диска: грубый проход считается ОДИН раз, а чанки его читают.
  bool save(const std::string& path) const;
  bool load(const std::string& path);

private:
  void build_index();

  struct segment {
    std::array<double, 3> from{};
    std::array<double, 3> to{};
    uint32_t chain = 0;
    uint32_t point = 0; // индекс первой точки отрезка в `points_`
  };

  bool collect(const std::array<double, 3>& low, const std::array<double, 3>& high,
               std::span<const size_t> candidates, size_t point_capacity, size_t chain_capacity,
               query_result& out) const;

  description description_{};
  std::vector<skeleton_node> nodes_;
  std::vector<std::array<double, 3>> points_;
  std::vector<uint32_t> offsets_;

  // Пространственный индекс: сетка тайлов по горизонтали, тайл -> отрезки, его касающиеся. Тайл же
  // естественная единица резидентности, если каркас однажды перестанет влезать в память целиком.
  std::vector<segment> segments_;
  std::vector<std::vector<size_t>> tiles_;
  double tile_size_ = 0.0;
  int64_t tile_low_[2]{};
  int64_t tile_count_[2]{};
};

} // namespace devils_engine::gn03

#endif
