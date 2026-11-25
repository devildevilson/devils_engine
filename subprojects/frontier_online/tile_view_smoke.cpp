// Отбор тайлов для рендера и сетка мира сессии — чистые функции клиента (core/tile_map.h).
//
// Рендер рисует камеру МЕЖДУ двумя последними снапшотами и дорисовывает кадр в полёте со снапшотом
// ещё старше; главный цикл публикует снапшоты 20 раз в секунду, а камера проходит 42 тайла в
// секунду. Срез вокруг одной текущей камеры оставлял отстающий край экрана чёрным. Здесь
// проверяется, что объединение покрывает каждую промежуточную камеру и что прыжок в него не входит.

#include <array>
#include <cmath>
#include <cstdio>

#include "core/tile_map.h"

namespace fo = frontier_online::core;

namespace {

int failures = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("CHECK failed at line %d: %s\n", __LINE__, #cond); \
      ++failures;                                             \
    }                                                         \
  } while (false)

fo::camera2d camera_at(const float x, const float y) {
  fo::camera2d value;
  value.center = {x, y};
  value.half_width = 28.0f;
  value.aspect = 16.0f / 9.0f;
  return value;
}

bool covers(const fo::tile_span& outer, const fo::tile_span& inner) {
  return outer.x0 <= inner.x0 && outer.y0 <= inner.y0 && outer.x1 >= inner.x1 && outer.y1 >= inner.y1;
}

fo::tile_span drawn_span(const fo::camera2d& current, const std::array<fo::camera2d, 2>& recent,
                         const fo::tile_grid& grid) {
  devils_engine::simul::published_cameras published;
  published.push(recent[1]);
  published.push(recent[0]);
  return fo::visible_tiles(published.bounds(current), grid, 1.0f);
}

void moving_camera_is_covered(const fo::tile_grid& grid) {
  // Камера идёт вправо на 2.1 тайла за снапшот: [0] — прошлый снапшот, [1] — позапрошлый.
  const auto current = camera_at(256.0f, 256.0f);
  const std::array<fo::camera2d, 2> recent{camera_at(253.9f, 256.0f), camera_at(251.8f, 256.0f)};
  const auto span = drawn_span(current, recent, grid);
  for (int step = 0; step <= 20; ++step) {
    const float t = float(step) / 20.0f;
    const auto drawn = camera_at(recent[1].center.x + (current.center.x - recent[1].center.x) * t, 256.0f);
    CHECK(covers(span, fo::visible_tiles(drawn, grid, 0.0f)));
  }
  // Одна текущая камера с тем же запасом отстающий край НЕ покрывает: ради этого и объединение.
  CHECK(!covers(fo::visible_tiles(current, grid, 1.0f), fo::visible_tiles(recent[1], grid, 0.0f)));
}

void jump_is_not_merged(const fo::tile_grid& grid) {
  const auto current = camera_at(256.0f, 256.0f);
  const std::array<fo::camera2d, 2> recent{camera_at(40.0f, 40.0f), camera_at(40.0f, 40.0f)}; // сброс, не движение
  const auto span = drawn_span(current, recent, grid);
  const auto alone = fo::visible_tiles(current, grid, 1.0f);
  CHECK(span.x0 == alone.x0 && span.y0 == alone.y0 && span.x1 == alone.x1 && span.y1 == alone.y1);
}

void no_history_is_current_view(const fo::tile_grid& grid) {
  const auto current = camera_at(100.0f, 80.0f);
  const devils_engine::simul::published_cameras none;
  const auto span = fo::visible_tiles(none.bounds(current), grid, 1.0f);
  const auto alone = fo::visible_tiles(current, grid, 1.0f);
  CHECK(span.x0 == alone.x0 && span.x1 == alone.x1 && span.count() == alone.count());
}

void session_grid_covers_the_box() {
  CHECK(fo::chunks_covering({512.0f, 512.0f}, 16, 1.0f) == glm::uvec2(32, 32));
  CHECK(fo::chunks_covering({64.5f, 10.0f}, 16, 1.0f) == glm::uvec2(5, 1)); // край не делится на чанк
  CHECK(fo::chunks_covering({0.1f, 0.1f}, 16, 1.0f) == glm::uvec2(1, 1));   // хотя бы один чанк
  CHECK(fo::chunks_covering({64.0f, 64.0f}, 16, 2.0f) == glm::uvec2(2, 2)); // тайл крупнее
}

} // namespace

int main() {
  fo::tile_grid grid;
  grid.resize(512, 512);
  moving_camera_is_covered(grid);
  jump_is_not_merged(grid);
  no_history_is_current_view(grid);
  session_grid_covers_the_box();
  std::printf("%s\n", failures == 0 ? "tile view smoke: ok" : "tile view smoke: FAILED");
  return failures == 0 ? 0 : 1;
}
