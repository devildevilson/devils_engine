#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"

using namespace devils_engine;

// Проверки объёмного инструмента.
//
// Таблица случаев marching cubes здесь не переписана из статьи, а ВЫВЕДЕНА из правила соединения на
// грани куба (см. volume_tools.cpp). Значит проверять надо не совпадение с чужой таблицей, а те
// свойства, ради которых она нужна:
//
//   - поверхность ЗАМКНУТА: каждое ребро треугольника принадлежит ровно двум треугольникам. Именно
//     это ломается от одной неверной строки, и именно это невозможно заметить глазом — дырка размером
//     в клетку видна раз в сотню чанков;
//   - ориентация согласована с полем: нормаль смотрит наружу от вещества, а обход вершин ей
//     соответствует;
//   - все 256 случаев встречаются, иначе замкнутость проверена не для всей таблицы;
//   - чанки складываются в целое ПОБИТОВО, а не «примерно»;
//   - параллельное исполнение совпадает с последовательным побитово;
//   - переполнение объявленной ёмкости — отказ, а не обрезанная поверхность.

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

struct volume_result {
  std::vector<std::array<double, 3>> positions;
  std::vector<std::array<double, 3>> normals;
  size_t vertex_count = 0;
  size_t distinct_masks = 0;
};

originator::buffer make_samples(const size_t count) {
  const std::vector<field_pair> fields = {{"position", "v3"}, {"density", "v1"}};
  return originator::buffer("samples", originator::make_buffer_layout(originator::storage_kind::soa, fields, "samples"), count);
}

originator::buffer make_vertices(const size_t capacity) {
  const std::vector<field_pair> fields = {{"position", "v3"}, {"normal", "v3"}};
  return originator::buffer("vertices", originator::make_buffer_layout(originator::storage_kind::soa, fields, "vertices"), capacity);
}

originator::buffer make_state() {
  const std::vector<field_pair> fields = {{"vertex_count", "ui1"}};
  return originator::buffer("state", originator::make_buffer_layout(originator::storage_kind::soa, fields, "state"), 1);
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

// Мир теста: поле плотности как функция мировой позиции. Считается по одному выражению во всех
// прогонах, поэтому «чанк против целого» сравнивает именно инструмент, а не два разных поля.
using density_field = double (*)(const double, const double, const double);

double sphere_field(const double x, const double y, const double z) {
  const double radius = 3.4;
  return radius * radius - (x * x + y * y + z * z);
}

// Поле с настоящей структурой: пещеры, арки и отдельные куски. Нужно ради ПОКРЫТИЯ случаев — у сферы
// встречается меньше половины таблицы, потому что у неё нет ни тонких перемычек, ни узких щелей.
double lumpy_field(const double x, const double y, const double z) {
  return std::sin(x * 0.9) + std::cos(y * 0.75) + std::sin(z * 1.15) * 0.8 - 0.15;
}

// Шумовое поле на хеше: знак каждого узла почти независим от соседей, поэтому за несколько тысяч
// клеток встречаются ВСЕ 256 случаев. Ближе двух слоёв к краю поле объявляется плотным, иначе
// поверхность выходит наружу блока клеток и замкнутость проверить нельзя.
struct hashed_field {
  size_t size[3]{};

  double operator()(const size_t x, const size_t y, const size_t z) const {
    const bool near_edge = x < 2 || y < 2 || z < 2 || x + 2 >= size[0] || y + 2 >= size[1] || z + 2 >= size[2];
    if (near_edge) {
      return 1.0;
    }
    uint64_t key = (uint64_t(x) * 0x9e3779b97f4a7c15ull) ^ (uint64_t(y) * 0xbf58476d1ce4e5b9ull) ^
                   (uint64_t(z) * 0x94d049bb133111ebull);
    key ^= key >> 29;
    key *= 0xbf58476d1ce4e5b9ull;
    key ^= key >> 32;
    return double(key >> 40) * (1.0 / 16777216.0) - 0.5;
  }
};

struct grid_description {
  size_t size[3]{};
  int64_t first[3]{}; // индекс первого узла в МИРОВОЙ решётке, может быть отрицательным
  double cell = 1.0;
  size_t border = 1;
};

// Один прогон инструмента на описанной сетке. Плотность берётся у поля по МИРОВОЙ позиции узла,
// поэтому у двух чанков общие узлы получают одно и то же число, а не два похожих.
volume_result run_marching_cubes(const originator::tool_registry& registry, const grid_description& grid,
                                 const density_field field, thread::atomic_pool* pool,
                                 const size_t capacity = 1u << 20) {
  const auto* tool = registry.find("marching_cubes");
  REQUIRE(tool != nullptr);

  const size_t sample_count = grid.size[0] * grid.size[1] * grid.size[2];
  auto samples = make_samples(sample_count);
  auto vertices = make_vertices(capacity);
  auto state = make_state();

  auto position = samples.field(samples.find_field("position"));
  auto density = samples.field(samples.find_field("density"));
  for (size_t i = 0; i < sample_count; ++i) {
    const size_t x = i % grid.size[0];
    const size_t y = (i / grid.size[0]) % grid.size[1];
    const size_t z = i / (grid.size[0] * grid.size[1]);
    const double world_x = double(grid.first[0] + int64_t(x)) * grid.cell;
    const double world_y = double(grid.first[1] + int64_t(y)) * grid.cell;
    const double world_z = double(grid.first[2] + int64_t(z)) * grid.cell;
    position.set(i, world_x, 0);
    position.set(i, world_y, 1);
    position.set(i, world_z, 2);
    density.set(i, field(world_x, world_y, world_z));
  }

  originator::parameters params;
  params.set_number("size_x", double(grid.size[0]));
  params.set_number("size_y", double(grid.size[1]));
  params.set_number("size_z", double(grid.size[2]));
  params.set_number("border", double(grid.border));
  params.set_number("iso", 0.0);

  const std::vector<originator::field_ref> inputs{readable(samples, "density"), readable(samples, "position")};
  const std::vector<originator::field_ref> outputs{writable(vertices, "position"), writable(vertices, "normal"),
                                                   writable(state, "vertex_count")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, sample_count, "surface", pool);

  volume_result result;
  result.vertex_count = size_t(state.field(0).get(0));
  const auto out_position = vertices.field(vertices.find_field("position"));
  const auto out_normal = vertices.field(vertices.find_field("normal"));
  result.positions.reserve(result.vertex_count);
  result.normals.reserve(result.vertex_count);
  for (size_t i = 0; i < result.vertex_count; ++i) {
    result.positions.push_back({out_position.get(i, 0), out_position.get(i, 1), out_position.get(i, 2)});
    result.normals.push_back({out_normal.get(i, 0), out_normal.get(i, 1), out_normal.get(i, 2)});
  }
  return result;
}

// Ключ вершины — её позиция как есть. Сравнение точное, и это не небрежность: вершина общего ребра
// двух клеток считается из ОДНОЙ пары узлов, значит и число выходит одно и то же. Если бы совпадение
// было лишь приблизительным, замкнутость пришлось бы проверять с допуском, а допуск скрыл бы ровно
// ту щель, которую тест ищет.
using vertex_key = std::array<double, 3>;

// Замкнутость считается по НАПРАВЛЕННЫМ рёбрам, и это не придирка к формулировке.
//
// Критерий «каждое ребро ровно у двух треугольников» неверен, и это выяснилось на первом же шумовом
// поле: 74 ребра из 20752 встретились ЧЕТЫРЕ раза. Дырок при этом не было ни одной. Так выходит на
// неоднозначной грани, где знаки чередуются по кругу: у обоих кубов, делящих такую грань, внутреннее
// ребро веера ложится в ПЛОСКОСТЬ грани и совпадает с таким же ребром соседа. Поверхность там
// касается сама себя вдоль отрезка — свойство обычного marching cubes, а не щель.
//
// Что действительно означает «замкнута»: у каждого ребра число проходов ВПЕРЁД равно числу проходов
// НАЗАД. Дырка даёт непарное ребро (один проход), вывернутый треугольник — два прохода в одну
// сторону, а касание двух листов даёт два и два, то есть проходит.
struct edge_balance {
  int forward = 0;
  int backward = 0;
};

std::map<std::pair<vertex_key, vertex_key>, edge_balance> edge_usage(const std::vector<vertex_key>& positions) {
  std::map<std::pair<vertex_key, vertex_key>, edge_balance> usage;
  for (size_t i = 0; i + 2 < positions.size(); i += 3) {
    for (size_t k = 0; k < 3; ++k) {
      const auto from = positions[i + k];
      const auto to = positions[i + (k + 1) % 3];
      if (from == to) {
        continue; // вырожденное ребро: вершина попала ровно в узел решётки
      }
      const bool ordered = from < to;
      const std::pair<vertex_key, vertex_key> key = ordered ? std::pair{from, to} : std::pair{to, from};
      auto& balance = usage[key];
      if (ordered) {
        balance.forward += 1;
      } else {
        balance.backward += 1;
      }
    }
  }
  return usage;
}

originator::tool_registry make_registry() {
  originator::tool_registry registry;
  registry.add_standard_tools();
  registry.add_volume_tools();
  return registry;
}
} // namespace

TEST_CASE("marching_cubes closes a sphere and orients it outwards") {
  const auto registry = make_registry();
  const auto* tool = registry.find("marching_cubes");
  REQUIRE(tool != nullptr);
  CHECK(tool->shape == originator::aperture::scatter);

  grid_description grid;
  grid.size[0] = grid.size[1] = grid.size[2] = 20;
  grid.first[0] = grid.first[1] = grid.first[2] = -10;
  grid.cell = 0.5;

  const auto surface = run_marching_cubes(registry, grid, &sphere_field, nullptr);
  REQUIRE(surface.vertex_count > 0);
  CHECK(surface.vertex_count % 3 == 0);

  // Замкнутость: каждое ребро ровно у двух треугольников. Сфера целиком внутри блока клеток,
  // поэтому у неё не должно быть ни одного ребра с другим числом.
  size_t unbalanced_edges = 0;
  for (const auto& [edge, balance] : edge_usage(surface.positions)) {
    if (balance.forward != balance.backward) {
      ++unbalanced_edges;
    }
  }
  CHECK(unbalanced_edges == 0);

  // Нормаль смотрит по радиусу наружу, а обход вершин согласован с нормалью.
  size_t wrong_normals = 0;
  size_t wrong_winding = 0;
  for (size_t i = 0; i + 2 < surface.positions.size(); i += 3) {
    const auto& a = surface.positions[i];
    const auto& b = surface.positions[i + 1];
    const auto& c = surface.positions[i + 2];

    const double radius = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const auto& n = surface.normals[i];
    const double radial = (a[0] * n[0] + a[1] * n[1] + a[2] * n[2]) / std::max(radius, 1.0e-9);
    if (radial < 0.9) {
      ++wrong_normals;
    }

    const std::array<double, 3> first{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const std::array<double, 3> second{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const std::array<double, 3> cross{first[1] * second[2] - first[2] * second[1],
                                      first[2] * second[0] - first[0] * second[2],
                                      first[0] * second[1] - first[1] * second[0]};
    const double length = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    if (length <= 0.0) {
      continue; // вырожденный треугольник: ребро прошло ровно через узел
    }
    const double agreement = (cross[0] * n[0] + cross[1] * n[1] + cross[2] * n[2]) / length;
    if (agreement <= 0.0) {
      ++wrong_winding;
    }
  }
  CHECK(wrong_normals == 0);
  CHECK(wrong_winding == 0);
}

TEST_CASE("marching_cubes stays closed on a noisy field and covers every case") {
  const auto registry = make_registry();
  const auto* tool = registry.find("marching_cubes");
  REQUIRE(tool != nullptr);

  // Шумовое поле: знак узла почти независим от соседей, поэтому за 3375 клеток встречаются все 256
  // сочетаний знаков. Поле собирается здесь же, потому что оно задано ИНДЕКСОМ узла, а не позицией.
  constexpr size_t side = 20;
  const size_t sample_count = side * side * side;
  hashed_field field{{side, side, side}};

  auto samples = make_samples(sample_count);
  auto vertices = make_vertices(1u << 20);
  auto state = make_state();

  auto position = samples.field(samples.find_field("position"));
  auto density = samples.field(samples.find_field("density"));
  for (size_t i = 0; i < sample_count; ++i) {
    const size_t x = i % side;
    const size_t y = (i / side) % side;
    const size_t z = i / (side * side);
    position.set(i, double(x), 0);
    position.set(i, double(y), 1);
    position.set(i, double(z), 2);
    density.set(i, field(x, y, z));
  }

  originator::parameters params;
  params.set_number("size_x", double(side));
  params.set_number("size_y", double(side));
  params.set_number("size_z", double(side));
  params.set_number("border", 1.0);

  const std::vector<originator::field_ref> inputs{readable(samples, "density"), readable(samples, "position")};
  const std::vector<originator::field_ref> outputs{writable(vertices, "position"), writable(vertices, "normal"),
                                                   writable(state, "vertex_count")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, sample_count, "noise_surface", nullptr);

  const size_t vertex_count = size_t(state.field(0).get(0));
  REQUIRE(vertex_count > 0);

  std::vector<vertex_key> positions;
  positions.reserve(vertex_count);
  const auto out_position = vertices.field(vertices.find_field("position"));
  for (size_t i = 0; i < vertex_count; ++i) {
    positions.push_back({out_position.get(i, 0), out_position.get(i, 1), out_position.get(i, 2)});
  }

  size_t unbalanced_edges = 0;
  size_t touching_edges = 0; // рёбра, вдоль которых поверхность касается сама себя
  for (const auto& [edge, balance] : edge_usage(positions)) {
    if (balance.forward != balance.backward) {
      ++unbalanced_edges;
    } else if (balance.forward != 1) {
      ++touching_edges;
    }
  }
  CHECK(unbalanced_edges == 0);
  // Число касаний не проверяется на равенство чему-то: оно свойство поля, а не инструмента. Но оно
  // печатается, потому что именно оно однажды выглядело как 74 незамкнутых ребра.
  MESSAGE("self-touching edges: " << touching_edges);

  // Покрытие случаев: сколько разных сочетаний знаков реально встретилось. Без этого числа
  // замкнутость доказана только для той части таблицы, которая попалась.
  std::set<uint32_t> masks;
  for (size_t z = 1; z + 2 < side; ++z) {
    for (size_t y = 1; y + 2 < side; ++y) {
      for (size_t x = 1; x + 2 < side; ++x) {
        uint32_t mask = 0;
        for (uint32_t corner = 0; corner < 8; ++corner) {
          const double value = field(x + (corner & 1u), y + ((corner >> 1) & 1u), z + ((corner >> 2) & 1u));
          if (value >= 0.0) {
            mask |= 1u << corner;
          }
        }
        masks.insert(mask);
      }
    }
  }
  CHECK(masks.size() == 256);
}

TEST_CASE("marching_cubes gives the same vertices with any number of threads") {
  const auto registry = make_registry();

  grid_description grid;
  grid.size[0] = grid.size[1] = grid.size[2] = 34;
  grid.first[0] = grid.first[1] = grid.first[2] = -17;
  grid.cell = 0.35;

  const auto sequential = run_marching_cubes(registry, grid, &lumpy_field, nullptr);
  thread::atomic_pool pool(4);
  const auto parallel = run_marching_cubes(registry, grid, &lumpy_field, &pool);

  REQUIRE(sequential.vertex_count > 0);
  REQUIRE(sequential.vertex_count == parallel.vertex_count);
  CHECK(sequential.positions == parallel.positions);
  CHECK(sequential.normals == parallel.normals);
}

TEST_CASE("marching_cubes chunks add up to the whole volume bit for bit") {
  const auto registry = make_registry();

  // Полоса перекрытия — один узел с каждой стороны (border = 1). С ней у каждого угла клетки
  // центральная разность считается по узлам, которые есть в ОБОИХ прогонах, поэтому нормаль на
  // общей грани двух чанков совпадает точно.
  constexpr int64_t cells_x = 8;
  constexpr int64_t cells_y = 8;
  constexpr int64_t cells_z = 8;

  grid_description whole;
  whole.size[0] = size_t(2 * cells_x + 3);
  whole.size[1] = size_t(cells_y + 3);
  whole.size[2] = size_t(cells_z + 3);
  whole.first[0] = -1;
  whole.first[1] = -1;
  whole.first[2] = -1;
  whole.cell = 0.4;

  grid_description left = whole;
  left.size[0] = size_t(cells_x + 3);
  left.first[0] = -1;

  grid_description right = left;
  right.first[0] = cells_x - 1;

  const auto full = run_marching_cubes(registry, whole, &lumpy_field, nullptr);
  const auto first_chunk = run_marching_cubes(registry, left, &lumpy_field, nullptr);
  const auto second_chunk = run_marching_cubes(registry, right, &lumpy_field, nullptr);

  REQUIRE(full.vertex_count > 0);
  CHECK(first_chunk.vertex_count + second_chunk.vertex_count == full.vertex_count);

  // Порядок вершин у чанков ДРУГОЙ: у каждого своя нумерация клеток. Поэтому сравниваются
  // отсортированные наборы — совпадать обязаны сами вершины, а не порядок обхода.
  std::vector<std::array<double, 6>> whole_vertices;
  std::vector<std::array<double, 6>> chunked_vertices;
  const auto append = [](std::vector<std::array<double, 6>>& out, const volume_result& source) {
    for (size_t i = 0; i < source.vertex_count; ++i) {
      out.push_back({source.positions[i][0], source.positions[i][1], source.positions[i][2],
                     source.normals[i][0], source.normals[i][1], source.normals[i][2]});
    }
  };
  append(whole_vertices, full);
  append(chunked_vertices, first_chunk);
  append(chunked_vertices, second_chunk);
  std::sort(whole_vertices.begin(), whole_vertices.end());
  std::sort(chunked_vertices.begin(), chunked_vertices.end());

  CHECK(whole_vertices == chunked_vertices);
}

TEST_CASE("marching_cubes refuses to overflow the declared vertex capacity") {
  const auto registry = make_registry();

  grid_description grid;
  grid.size[0] = grid.size[1] = grid.size[2] = 20;
  grid.first[0] = grid.first[1] = grid.first[2] = -10;
  grid.cell = 0.5;

  // Ёмкости заведомо не хватает: обрезать поверхность нельзя, потому что обрезанная поверхность
  // выглядит как дырка в мире и ищется по картинке, а не по сообщению.
  CHECK_THROWS_AS(run_marching_cubes(registry, grid, &sphere_field, nullptr, 128), std::runtime_error);
}

TEST_CASE("polyline_distance measures the way a corridor needs it") {
  const auto registry = make_registry();
  const auto* tool = registry.find("polyline_distance");
  REQUIRE(tool != nullptr);
  CHECK(tool->shape == originator::aperture::gather);

  // Две цепочки: отрезок вдоль x и отдельный отрезок вдоль z. Разные цепочки нужны потому, что
  // маршрут в мире не один, а конец одного не должен соединяться с началом другого — иначе между
  // ними появился бы коридор, которого никто не строил.
  const std::vector<field_pair> point_fields = {{"position", "v3"}};
  auto points = originator::buffer(
    "points", originator::make_buffer_layout(originator::storage_kind::soa, point_fields, "points"), 5);
  auto point = points.field(0);
  const double coordinates[5][3] = {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {50, 0, 50}, {50, 0, 70}};
  for (size_t i = 0; i < 5; ++i) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      point.set(i, coordinates[i][axis], axis);
    }
  }

  const std::vector<field_pair> offset_fields = {{"offset", "ui1"}};
  auto offsets = originator::buffer(
    "offsets", originator::make_buffer_layout(originator::storage_kind::soa, offset_fields, "offsets"), 3);
  offsets.field(0).set(0, 0.0);
  offsets.field(0).set(1, 3.0);
  offsets.field(0).set(2, 5.0);

  const std::vector<field_pair> sample_fields = {{"position", "v3"}, {"distance", "v1"}};
  constexpr size_t sample_count = 6;
  auto samples = originator::buffer(
    "probes", originator::make_buffer_layout(originator::storage_kind::soa, sample_fields, "probes"), sample_count);
  auto sample_position = samples.field(0);
  const double probes[sample_count][3] = {
    {5, 0, 0},      // на первой цепочке
    {5, 4, 0},      // в четырёх метрах над ней
    {-10, 0, 0},    // ЗА концом цепочки: расстояние до КОНЦА, а не до прямой
    {50, 0, 60},    // на второй цепочке
    {200, 0, 200},  // далеко от обеих цепочек: дальше объявленного предела
    {15, 3, 4},     // около первой цепочки под углом
  };
  for (size_t i = 0; i < sample_count; ++i) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      sample_position.set(i, probes[i][axis], axis);
    }
  }

  originator::parameters params;
  params.set_number("max_distance", 30.0);

  const std::vector<originator::field_ref> inputs{readable(samples, "position"), readable(points, "position"),
                                                  readable(offsets, "offset")};
  const std::vector<originator::field_ref> outputs{writable(samples, "distance")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, sample_count, "route", nullptr);

  const auto distance = samples.field(samples.find_field("distance"));
  CHECK(distance.get(0) == doctest::Approx(0.0));
  CHECK(distance.get(1) == doctest::Approx(4.0));
  // ЗА концом маршрута расстояние считается до конца, а не до бесконечной прямой: иначе коридор
  // продолжался бы за станцию, то есть тоннель уходил бы в никуда.
  CHECK(distance.get(2) == doctest::Approx(10.0));
  CHECK(distance.get(3) == doctest::Approx(0.0));
  // Дальше предела значение равно пределу, а не бесконечности: полем дальше пользуется арифметика
  // конфига, и бесконечность превратила бы любую сумму в бесконечность. Проба взята заведомо далёкой
  // (212 метров до ближайшей цепочки): первая попытка ставила её в 21 метре и падала честно — там
  // расстояние настоящее, а не предел.
  CHECK(distance.get(4) == doctest::Approx(30.0));
  CHECK(distance.get(5) == doctest::Approx(5.0));

  // Параллельно — то же самое: апертура gather читает точки, которые никто не пишет.
  auto again = originator::buffer(
    "probes", originator::make_buffer_layout(originator::storage_kind::soa, sample_fields, "probes"), sample_count);
  auto again_position = again.field(0);
  for (size_t i = 0; i < sample_count; ++i) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      again_position.set(i, probes[i][axis], axis);
    }
  }
  const std::vector<originator::field_ref> again_inputs{readable(again, "position"), readable(points, "position"),
                                                        readable(offsets, "offset")};
  const std::vector<originator::field_ref> again_outputs{writable(again, "distance")};
  thread::atomic_pool pool(3);
  originator::dispatch(*tool, again_inputs, again_outputs, params, 1, 0, sample_count, "route", &pool);
  const auto parallel = again.field(again.find_field("distance"));
  for (size_t i = 0; i < sample_count; ++i) {
    CHECK(parallel.get(i) == distance.get(i));
  }

  // ПРЕДЕЛ ОБЯЗАТЕЛЕН. Он значит две вещи сразу — насколько широко смотреть и что отвечать там, где
  // ломаной нет, — и пока у него было значение по умолчанию, эти два смысла разъезжались: подготовка
  // брала ноль, тело миллиард, и вызов без параметра давал поле с разрывом на границе прямоугольника
  // отрезка. Разрыв в поле плотности выглядит как стена посреди мира.
  originator::parameters no_limit;
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, no_limit, 1, 0, sample_count, "route", nullptr),
                  std::runtime_error);

  // Цепочка, выходящая за буфер точек, — ошибка конфига, а не повод читать чужую память.
  offsets.field(0).set(2, 99.0);
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, params, 1, 0, sample_count, "route", nullptr),
                  std::runtime_error);
}

TEST_CASE("polyline_distance in the chebyshev metric gives an angular corridor") {
  const auto registry = make_registry();
  const auto* tool = registry.find("polyline_distance");
  REQUIRE(tool != nullptr);

  // Один отрезок вдоль оси x: у него ответ в чебышёвской метрике точен, потому что проекция совпадает
  // в обеих метриках.
  const std::vector<field_pair> point_fields = {{"position", "v3"}};
  auto points = originator::buffer(
    "points", originator::make_buffer_layout(originator::storage_kind::soa, point_fields, "points"), 2);
  auto point = points.field(0);
  point.set(0, 0.0, 0);
  point.set(1, 40.0, 0);

  const std::vector<field_pair> offset_fields = {{"offset", "ui1"}};
  auto offsets = originator::buffer(
    "offsets", originator::make_buffer_layout(originator::storage_kind::soa, offset_fields, "offsets"), 2);
  offsets.field(0).set(0, 0.0);
  offsets.field(0).set(1, 2.0);

  const std::vector<field_pair> sample_fields = {{"position", "v3"}, {"distance", "v1"}};
  auto samples = originator::buffer(
    "probes", originator::make_buffer_layout(originator::storage_kind::soa, sample_fields, "probes"), 2);
  auto sample_position = samples.field(0);
  // Точка по диагонали от оси: у круглого сечения она в 4.24 метра, у квадратного — в 3.
  sample_position.set(0, 20.0, 0);
  sample_position.set(0, 3.0, 1);
  sample_position.set(0, 3.0, 2);
  sample_position.set(1, 20.0, 0);
  sample_position.set(1, 3.0, 1);

  const std::vector<originator::field_ref> inputs{readable(samples, "position"), readable(points, "position"),
                                                  readable(offsets, "offset")};
  const std::vector<originator::field_ref> outputs{writable(samples, "distance")};

  originator::parameters round_params;
  round_params.set_number("max_distance", 30.0);
  originator::dispatch(*tool, inputs, outputs, round_params, 1, 0, 2, "round", nullptr);
  const auto round = samples.field(samples.find_field("distance"));
  CHECK(round.get(0) == doctest::Approx(std::sqrt(18.0)));
  CHECK(round.get(1) == doctest::Approx(3.0));

  originator::parameters angular_params;
  angular_params.set_number("max_distance", 30.0);
  angular_params.set_string("metric", "chebyshev");
  originator::dispatch(*tool, inputs, outputs, angular_params, 1, 0, 2, "angular", nullptr);
  const auto angular = samples.field(samples.find_field("distance"));
  // ФОРМА СЕЧЕНИЯ, а не мелкая настройка: у чебышёвской метрики поверхность уровня — куб, поэтому
  // диагональная точка попадает внутрь трубы того же «радиуса», а круглая её не достаёт.
  CHECK(angular.get(0) == doctest::Approx(3.0));
  CHECK(angular.get(1) == doctest::Approx(3.0));

  originator::parameters wrong;
  wrong.set_string("metric", "manhattan");
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, wrong, 1, 0, 2, "angular", nullptr),
                  std::runtime_error);
}

TEST_CASE("marching_cubes refuses a grid that disagrees with its buffer") {
  const auto registry = make_registry();
  const auto* tool = registry.find("marching_cubes");
  REQUIRE(tool != nullptr);

  auto samples = make_samples(1000);
  auto vertices = make_vertices(1024);
  auto state = make_state();

  originator::parameters params;
  params.set_number("size_x", 12.0); // 12*12*12 = 1728 != 1000
  params.set_number("border", 1.0);

  const std::vector<originator::field_ref> inputs{readable(samples, "density"), readable(samples, "position")};
  const std::vector<originator::field_ref> outputs{writable(vertices, "position"), writable(vertices, "normal"),
                                                   writable(state, "vertex_count")};
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, params, 1, 0, 1000, "surface", nullptr),
                  std::runtime_error);
}
