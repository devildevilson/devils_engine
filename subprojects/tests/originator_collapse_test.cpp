#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/computation_queue.h"
#include "devils_engine/originator/tools.h"

using namespace devils_engine;

// РЕШАТЕЛЬ ОГРАНИЧЕНИЙ (wave function collapse).
//
// Проверяется здесь не «получилась красивая картинка», а единственное, что алгоритм ОБЕЩАЕТ: в
// результате нет ни одной пары соседей, запрещённой таблицей. Всё остальное — эвристика, и её можно
// менять; это — контракт, и его нельзя.

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t tile_count = 4;
constexpr size_t side = 32;
constexpr size_t cell_count = side * side;
// Правила объявляются МАТРИЦЕЙ: `allowed[(axis * tiles + a) * tiles + b]` ненулевое означает «b может
// стоять по +axis от a». Битовые наборы — внутреннее дело решателя.
constexpr size_t rule_size = 2 * tile_count * tile_count;

originator::tool_registry& registry() {
  static originator::tool_registry r;
  if (r.size() == 0) {
    r.add_standard_tools();
  }
  return r;
}

// Ландшафтная лесенка: вода, песок, трава, лес. Соседство СИММЕТРИЧНО и разрешено только через
// соседнюю ступень — вода никогда не касается травы, трава никогда не касается воды.
struct rule_table {
  std::vector<uint8_t> matrix = std::vector<uint8_t>(rule_size, 0);

  void allow_directed(const size_t axis, const size_t from, const size_t to) {
    matrix[(axis * tile_count + from) * tile_count + to] = 1;
  }

  void allow(const size_t a, const size_t b) {
    for (size_t axis = 0; axis < 2; ++axis) {
      allow_directed(axis, a, b);
      allow_directed(axis, b, a);
    }
  }

  bool permits(const size_t axis, const size_t from, const size_t to) const {
    return matrix[(axis * tile_count + from) * tile_count + to] != 0;
  }
};

rule_table ladder_rules() {
  rule_table table;
  for (size_t tile = 0; tile < tile_count; ++tile) {
    table.allow(tile, tile);
  }
  table.allow(0, 1);
  table.allow(1, 2);
  table.allow(2, 3);
  return table;
}

struct scene {
  originator::buffer tiles;
  originator::buffer rules;
  originator::buffer cells;
  originator::buffer state;
};

scene make_scene(const rule_table& table) {
  scene result;

  const std::vector<field_pair> tile_fields = {{"weight", "v1"}};
  result.tiles = originator::buffer(
    "tiles", originator::make_buffer_layout(originator::storage_kind::soa, tile_fields, "tiles"), tile_count);

  const std::vector<field_pair> rule_fields = {{"allowed", "ui1"}};
  result.rules = originator::buffer(
    "rules", originator::make_buffer_layout(originator::storage_kind::soa, rule_fields, "rules"), rule_size);

  const std::vector<field_pair> cell_fields = {{"tile", "ui1"}, {"given", "ui1"}};
  result.cells = originator::buffer(
    "cells", originator::make_buffer_layout(originator::storage_kind::soa, cell_fields, "cells"),
    originator::buffer_extent{side, side, 0});

  const std::vector<field_pair> state_fields = {{"attempts", "ui1"}};
  result.state = originator::buffer(
    "state", originator::make_buffer_layout(originator::storage_kind::soa, state_fields, "state"), size_t(1));

  auto weights = result.tiles.field(result.tiles.find_field("weight"));
  for (size_t i = 0; i < tile_count; ++i) {
    weights.set(i, 1.0);
  }

  auto allowed = result.rules.field(result.rules.find_field("allowed"));
  for (size_t i = 0; i < rule_size; ++i) {
    allowed.set(i, double(table.matrix[i]));
  }
  return result;
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

void solve(scene& target, const uint64_t seed, const bool with_given, const int64_t attempts = 8) {
  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("attempts", double(attempts));

  std::vector<originator::field_ref> inputs{readable(target.tiles, "weight"), readable(target.rules, "allowed")};
  if (with_given) {
    inputs.push_back(readable(target.cells, "given"));
  }
  const std::vector<originator::field_ref> outputs{writable(target.cells, "tile"),
                                                   writable(target.state, "attempts")};

  originator::dispatch(*tool, inputs, outputs, params, seed, 0, cell_count, "collapse", nullptr);
}

std::vector<uint32_t> read_tiles(const scene& target) {
  const auto field = target.cells.field(target.cells.find_field("tile"));
  std::vector<uint32_t> values(cell_count);
  for (size_t i = 0; i < cell_count; ++i) {
    values[i] = uint32_t(field.get(i));
  }
  return values;
}

// Единственное, что алгоритм обещает: ни одной запрещённой пары соседей.
size_t broken_pairs(const std::vector<uint32_t>& tiles, const rule_table& table) {
  size_t broken = 0;
  const auto permitted = [&](const size_t axis, const size_t from, const size_t to) {
    return table.permits(axis, from, to);
  };

  for (size_t y = 0; y < side; ++y) {
    for (size_t x = 0; x < side; ++x) {
      const auto own = tiles[y * side + x];
      if (x + 1 < side) {
        broken += size_t(!permitted(0, own, tiles[y * side + x + 1]));
      }
      if (y + 1 < side) {
        broken += size_t(!permitted(1, own, tiles[(y + 1) * side + x]));
      }
    }
  }
  return broken;
}
} // namespace

TEST_CASE("originator collapse honours every declared adjacency rule") {
  const auto table = ladder_rules();
  auto target = make_scene(table);
  solve(target, 20260905, false);

  const auto tiles = read_tiles(target);
  CHECK(broken_pairs(tiles, table) == 0);

  // И решение обязано быть СОДЕРЖАТЕЛЬНЫМ: сетка из одного тайла тоже не нарушает ни одного правила,
  // но задачи не решает.
  std::vector<size_t> present(tile_count, 0);
  for (const auto tile : tiles) {
    REQUIRE(tile < tile_count);
    ++present[tile];
  }
  size_t used = 0;
  for (const auto seen : present) {
    used += size_t(seen != 0);
  }
  CHECK(used >= 3);
}

TEST_CASE("originator collapse repeats itself under the same seed and moves under another") {
  const auto table = ladder_rules();

  auto first = make_scene(table);
  solve(first, 777, false);
  auto again = make_scene(table);
  solve(again, 777, false);
  auto other = make_scene(table);
  solve(other, 778, false);

  // ПОБИТОВО: выбор клетки идёт по целому критерию, а ничья ломается хешем от номера и зерна, поэтому
  // порядок наблюдений один и тот же на любой машине.
  CHECK(read_tiles(first) == read_tiles(again));
  CHECK(read_tiles(first) != read_tiles(other));
}

TEST_CASE("originator collapse keeps the cells it was given") {
  const auto table = ladder_rules();
  auto target = make_scene(table);

  // Ноль означает «клетка свободна», как и у остальных инструментов с ключом; иначе это номер тайла
  // плюс один.
  auto given = target.cells.field(target.cells.find_field("given"));
  const std::vector<std::pair<size_t, uint32_t>> pinned = {{0, 0}, {side * side - 1, 3}, {side * 4 + 4, 2}};
  for (const auto& [cell, tile] : pinned) {
    given.set(cell, double(tile + 1));
  }

  solve(target, 4242, true);
  const auto tiles = read_tiles(target);

  CHECK(broken_pairs(tiles, table) == 0);
  for (const auto& [cell, tile] : pinned) {
    CHECK(tiles[cell] == tile);
  }
}

TEST_CASE("originator collapse refuses loudly instead of leaving a half-filled grid") {
  // Правила, у которых решения нет вовсе: два тайла, и ни один не может стоять рядом ни с одним —
  // включая самого себя. Первая же клетка после наблюдения оставляет соседям пустой набор.
  rule_table impossible;
  auto target = make_scene(impossible);

  CHECK_THROWS_AS(solve(target, 1, false, 3), std::runtime_error);
}

TEST_CASE("originator collapse names a rule table that does not fit its tiles") {
  const auto table = ladder_rules();
  auto target = make_scene(table);

  const std::vector<field_pair> rule_fields = {{"allowed", "ui1"}};
  originator::buffer narrow(
    "narrow", originator::make_buffer_layout(originator::storage_kind::soa, rule_fields, "narrow"), size_t(2));

  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);
  const std::vector<originator::field_ref> inputs{readable(target.tiles, "weight"), readable(narrow, "allowed")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "tile")};

  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, originator::parameters{}, 1, 0, cell_count,
                                       "collapse", nullptr),
                  std::runtime_error);
}

TEST_CASE("originator collapse is refused by a queue, and the reason is the order") {
  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);

  // Апертура `sequential` — не деталь реализации: какую клетку наблюдать следующей, решает поле,
  // оставшееся после предыдущего распространения. Поэтому решатель НИКОГДА не попадёт в очередь и
  // никогда не поедет на устройство, и отказ очереди говорит ровно это.
  CHECK(tool->shape == originator::aperture::sequential);
  CHECK_FALSE(originator::fits_in_queue(tool->shape));
  CHECK(originator::queue_rejection_reason(tool->shape).find("ordered by construction") != std::string::npos);
}

// ПРАВИЛА ИЗ ОБРАЗЦА, РАСКЛАДКА ПО ГРАФУ И ОТКАТЫ.
//
// Три разные вещи, но проверяются они об одно и то же обещание: в результате нет пары соседей,
// которую правила не разрешают. Меняется только то, ОТКУДА правила взялись и по чему считается
// соседство.
namespace {

originator::buffer make_field_buffer(const char* name, const char* type, const size_t count) {
  const std::vector<field_pair> fields = {{"value", type}};
  return originator::buffer(name, originator::make_buffer_layout(originator::storage_kind::soa, fields, name),
                            count);
}

originator::buffer make_raster_buffer(const char* name, const char* type, const size_t width, const size_t height) {
  const std::vector<field_pair> fields = {{"value", type}};
  return originator::buffer(name, originator::make_buffer_layout(originator::storage_kind::soa, fields, name),
                            originator::buffer_extent{width, height, 0});
}

constexpr size_t sample_side = 8;

// Образец: концентрические кольца лесенки. Пара «край рядом с центром» в нём не встречается НИ РАЗУ,
// и это то, что обучение обязано вынести из картинки.
std::vector<uint32_t> ring_sample() {
  std::vector<uint32_t> values(sample_side * sample_side, 0);
  const int centre = int(sample_side) / 2;
  for (int y = 0; y < int(sample_side); ++y) {
    for (int x = 0; x < int(sample_side); ++x) {
      const int distance = std::max(std::abs(x - centre), std::abs(y - centre));
      values[size_t(y) * sample_side + size_t(x)] = uint32_t(std::max(0, 3 - distance));
    }
  }
  return values;
}

struct learned_scene {
  originator::buffer sample;
  originator::buffer weights;
  originator::buffer rules;
  originator::buffer representative;
  originator::buffer found;
  size_t capacity = 0;
  size_t axes = 2;
};

learned_scene learn(const std::vector<uint32_t>& values, const size_t capacity, const int64_t window,
                    const bool symmetric = false) {
  learned_scene scene;
  scene.capacity = capacity;
  scene.axes = symmetric ? 1 : 2;
  scene.sample = make_raster_buffer("sample", "ui1", sample_side, sample_side);
  auto sample_field = scene.sample.field(0);
  for (size_t i = 0; i < values.size(); ++i) {
    sample_field.set(i, double(values[i]));
  }

  scene.weights = make_field_buffer("weights", "v1", capacity);
  scene.rules = make_field_buffer("rules", "ui1", scene.axes * capacity * capacity);
  scene.representative = make_field_buffer("representative", "ui1", capacity);
  scene.found = make_field_buffer("found", "ui1", 1);

  const auto* tool = registry().find("learn_rules");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("window", double(window));
  if (symmetric) {
    params.set_number("symmetric", 1.0);
  }

  const std::vector<originator::field_ref> inputs{readable(scene.sample, "value")};
  const std::vector<originator::field_ref> outputs{writable(scene.weights, "value"),
                                                   writable(scene.rules, "value"),
                                                   writable(scene.representative, "value"),
                                                   writable(scene.found, "value")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, values.size(), "learn", nullptr);
  return scene;
}

bool learned_permits(const learned_scene& scene, const size_t axis, const size_t a, const size_t b) {
  const auto allowed = scene.rules.field(0);
  return allowed.get((axis * scene.capacity + a) * scene.capacity + b) != 0.0;
}

// Соседство ГРАФА: сетка, выложенная в CSR. Растр здесь только для того, чтобы правила остались
// сравнимыми с растровым решателем; сам решатель про форму ничего не знает — у него есть дуги.
struct graph_scene {
  originator::buffer offsets;
  originator::buffer arcs;
  originator::buffer weights;
  originator::buffer rules;
  originator::buffer cells;
  originator::buffer state;
  size_t side = 0;
};

graph_scene make_graph_scene(const rule_table& table, const size_t graph_side) {
  graph_scene scene;
  scene.side = graph_side;
  const size_t nodes = graph_side * graph_side;

  std::vector<std::vector<uint32_t>> neighbours(nodes);
  for (size_t y = 0; y < graph_side; ++y) {
    for (size_t x = 0; x < graph_side; ++x) {
      const size_t node = y * graph_side + x;
      if (x + 1 < graph_side) {
        neighbours[node].push_back(uint32_t(node + 1));
        neighbours[node + 1].push_back(uint32_t(node));
      }
      if (y + 1 < graph_side) {
        neighbours[node].push_back(uint32_t(node + graph_side));
        neighbours[node + graph_side].push_back(uint32_t(node));
      }
    }
  }

  size_t arc_count = 0;
  for (const auto& list : neighbours) {
    arc_count += list.size();
  }

  scene.offsets = make_field_buffer("offsets", "ui1", nodes + 1);
  scene.arcs = make_field_buffer("arcs", "ui1", arc_count);
  auto offsets = scene.offsets.field(0);
  auto arcs = scene.arcs.field(0);
  size_t cursor = 0;
  for (size_t node = 0; node < nodes; ++node) {
    offsets.set(node, double(cursor));
    for (const auto other : neighbours[node]) {
      arcs.set(cursor++, double(other));
    }
  }
  offsets.set(nodes, double(cursor));

  scene.weights = make_field_buffer("weights", "v1", tile_count);
  auto weights = scene.weights.field(0);
  for (size_t i = 0; i < tile_count; ++i) {
    weights.set(i, 1.0);
  }

  // У дуги нет направления, поэтому матрица ОДНА и она обязана быть симметричной.
  scene.rules = make_field_buffer("rules", "ui1", tile_count * tile_count);
  auto allowed = scene.rules.field(0);
  for (size_t a = 0; a < tile_count; ++a) {
    for (size_t b = 0; b < tile_count; ++b) {
      allowed.set(a * tile_count + b, table.permits(0, a, b) ? 1.0 : 0.0);
    }
  }

  scene.cells = make_field_buffer("cells", "ui1", nodes);
  scene.state = make_field_buffer("state", "ui1", 1);
  return scene;
}

void solve_graph(graph_scene& scene, const uint64_t seed) {
  const auto* tool = registry().find("graph_collapse");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("attempts", 8.0);

  const std::vector<originator::field_ref> inputs{readable(scene.offsets, "value"), readable(scene.arcs, "value"),
                                                  readable(scene.weights, "value"), readable(scene.rules, "value")};
  const std::vector<originator::field_ref> outputs{writable(scene.cells, "value"), writable(scene.state, "value")};
  originator::dispatch(*tool, inputs, outputs, params, seed, 0, scene.side * scene.side, "graph", nullptr);
}

// Раскладка по трём цветам на ЗАМКНУТОМ растре нечётной стороны: соседи обязаны различаться. Решение
// существует, но жадный решатель в него попадает редко — ошибка становится видна далеко от того
// места, где сделана. Ровно тот случай, ради которого нужны откаты.
size_t solved_three_colours(const size_t colour_side, const int64_t rollbacks, const size_t seeds) {
  auto weights = make_field_buffer("weights", "v1", 3);
  for (size_t i = 0; i < 3; ++i) {
    weights.field(0).set(i, 1.0);
  }
  auto rules = make_field_buffer("rules", "ui1", 2 * 3 * 3);
  for (size_t axis = 0; axis < 2; ++axis) {
    for (size_t a = 0; a < 3; ++a) {
      for (size_t b = 0; b < 3; ++b) {
        rules.field(0).set((axis * 3 + a) * 3 + b, a == b ? 0.0 : 1.0);
      }
    }
  }

  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);

  size_t solved = 0;
  for (size_t seed = 1; seed <= seeds; ++seed) {
    auto cells = make_raster_buffer("cells", "ui1", colour_side, colour_side);
    originator::parameters params;
    params.set_number("wrap", 1.0);
    params.set_number("attempts", 1.0);
    params.set_number("rollbacks", double(rollbacks));

    const std::vector<originator::field_ref> inputs{readable(weights, "value"), readable(rules, "value")};
    const std::vector<originator::field_ref> outputs{writable(cells, "value")};
    try {
      originator::dispatch(*tool, inputs, outputs, params, seed, 0, colour_side * colour_side, "colours", nullptr);
      solved += 1;
    } catch (const std::runtime_error&) {
      // Противоречие — НОРМАЛЬНЫЙ исход этого алгоритма, и здесь считается именно его частота.
    }
  }
  return solved;
}
} // namespace

TEST_CASE("originator learn_rules takes the alphabet and the rules off a sample") {
  const auto values = ring_sample();
  const auto scene = learn(values, 8, 1);

  // Окно в одну клетку: алфавит узоров это просто тайлы, которые в образце есть.
  CHECK(scene.found.field(0).get(0) == 4.0);

  // Представитель узора — тайл в его якоре: им раскладка переводится обратно в тайлы обычным
  // `lookup`, второго механизма для этого не нужно.
  std::vector<size_t> pattern_of_tile(4, 8);
  for (size_t pattern = 0; pattern < 4; ++pattern) {
    const auto tile = size_t(scene.representative.field(0).get(pattern));
    REQUIRE(tile < 4);
    pattern_of_tile[tile] = pattern;
  }
  for (const auto pattern : pattern_of_tile) {
    CHECK(pattern < 4);
  }

  // Вес узора — сколько раз он встретился. Сумма обязана сойтись с числом позиций окна.
  double total = 0.0;
  for (size_t i = 0; i < scene.capacity; ++i) {
    total += scene.weights.field(0).get(i);
  }
  CHECK(total == double(values.size()));

  // Свободная ёмкость алфавита получает НУЛЕВОЙ вес: решатель понимает это как «никогда», и лишний
  // узор из незанятого места не появится.
  for (size_t i = 4; i < scene.capacity; ++i) {
    CHECK(scene.weights.field(0).get(i) == 0.0);
  }

  // Кольца лесенки: соседние ступени в образце встречаются, а через ступень — ни разу.
  CHECK(learned_permits(scene, 0, pattern_of_tile[0], pattern_of_tile[1]));
  CHECK(learned_permits(scene, 0, pattern_of_tile[1], pattern_of_tile[2]));
  CHECK_FALSE(learned_permits(scene, 0, pattern_of_tile[0], pattern_of_tile[2]));
  CHECK_FALSE(learned_permits(scene, 0, pattern_of_tile[0], pattern_of_tile[3]));
  CHECK_FALSE(learned_permits(scene, 1, pattern_of_tile[1], pattern_of_tile[3]));
}

TEST_CASE("originator collapse never puts down a pair the sample did not show") {
  const auto values = ring_sample();
  const auto scene = learn(values, 8, 1);

  // Пары, ВСТРЕЧЕННЫЕ в образце. Это и есть обещание обученных правил, и проверяется оно по
  // образцу, а не по таблице: таблица — то, что проверяется.
  std::vector<uint8_t> seen(4 * 4 * 2, 0);
  for (size_t y = 0; y < sample_side; ++y) {
    for (size_t x = 0; x < sample_side; ++x) {
      const auto own = values[y * sample_side + x];
      if (x + 1 < sample_side) seen[(0 * 4 + own) * 4 + values[y * sample_side + x + 1]] = 1;
      if (y + 1 < sample_side) seen[(1 * 4 + own) * 4 + values[(y + 1) * sample_side + x]] = 1;
    }
  }

  auto cells = make_raster_buffer("cells", "ui1", side, side);
  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("attempts", 16.0);
  params.set_number("rollbacks", 64.0);
  const std::vector<originator::field_ref> inputs{readable(scene.weights, "value"), readable(scene.rules, "value")};
  const std::vector<originator::field_ref> outputs{writable(cells, "value")};
  originator::dispatch(*tool, inputs, outputs, params, 20260905, 0, cell_count, "collapse", nullptr);

  // Решатель работает в алфавите УЗОРОВ, поэтому раскладка переводится в тайлы представителями.
  std::vector<uint32_t> tiles(cell_count, 0);
  for (size_t i = 0; i < cell_count; ++i) {
    const auto pattern = size_t(cells.field(0).get(i));
    REQUIRE(pattern < scene.capacity);
    tiles[i] = uint32_t(scene.representative.field(0).get(pattern));
  }

  size_t unseen = 0;
  for (size_t y = 0; y < side; ++y) {
    for (size_t x = 0; x < side; ++x) {
      const auto own = tiles[y * side + x];
      if (x + 1 < side) unseen += size_t(seen[(0 * 4 + own) * 4 + tiles[y * side + x + 1]] == 0);
      if (y + 1 < side) unseen += size_t(seen[(1 * 4 + own) * 4 + tiles[(y + 1) * side + x]] == 0);
    }
  }
  CHECK(unseen == 0);
}

TEST_CASE("originator learn_rules names an alphabet that does not fit the declared capacity") {
  // Ёмкость алфавита ОБЪЯВЛЯЕТСЯ: генератор обязан уметь назвать свою стоимость до запуска, а
  // урезанный алфавит был бы другими правилами под тем же именем.
  CHECK_THROWS_AS(learn(ring_sample(), 2, 1), std::runtime_error);

  // Окно 2x2 по кольцам даёт заметно больше узоров, чем тайлов, — и это ровно то, за чем окно нужно.
  const auto wide = learn(ring_sample(), 64, 2);
  CHECK(wide.found.field(0).get(0) > 4.0);
}

TEST_CASE("originator learn_rules refuses to call a two-dimensional window symmetric") {
  // Симметричная таблица нужна ГРАФУ, у которого нет ни оси, ни поворота. Окно больше клетки — это
  // утверждение о растре, и оно зависит от направления по построению.
  CHECK_THROWS_AS(learn(ring_sample(), 64, 2, true), std::runtime_error);
  CHECK_NOTHROW(learn(ring_sample(), 8, 1, true));
}

TEST_CASE("originator graph_collapse honours the rules along every arc") {
  const auto table = ladder_rules();
  auto scene = make_graph_scene(table, 24);
  solve_graph(scene, 20260905);

  const auto offsets = scene.offsets.field(0);
  const auto arcs = scene.arcs.field(0);
  const auto tiles = scene.cells.field(0);

  size_t broken = 0;
  size_t checked = 0;
  for (size_t node = 0; node < scene.side * scene.side; ++node) {
    const auto own = size_t(tiles.get(node));
    const auto first = size_t(offsets.get(node));
    const auto last = size_t(offsets.get(node + 1));
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(tiles.get(size_t(arcs.get(k))));
      broken += size_t(!table.permits(0, own, other));
      checked += 1;
    }
  }
  CHECK(checked > 0);
  CHECK(broken == 0);

  // И решение содержательно: сетка из одного тайла тоже не нарушает правил, но задачи не решает.
  std::vector<size_t> present(tile_count, 0);
  for (size_t node = 0; node < scene.side * scene.side; ++node) {
    ++present[size_t(tiles.get(node))];
  }
  size_t used = 0;
  for (const auto seen : present) {
    used += size_t(seen != 0);
  }
  CHECK(used >= 3);
}

TEST_CASE("originator graph_collapse refuses an asymmetric rule table") {
  const auto table = ladder_rules();
  auto scene = make_graph_scene(table, 8);

  // «a рядом с b» и «b рядом с a» на графе — ОДНО утверждение: у дуги нет направления. Тихая
  // симметризация дала бы правила, которых автор не писал, поэтому это отказ.
  scene.rules.field(0).set(0 * tile_count + 1, 0.0);
  CHECK_THROWS_AS(solve_graph(scene, 1), std::runtime_error);
}

TEST_CASE("originator rollbacks rescue attempts that a restart would throw away") {
  // На замкнутом растре нечётной стороны раскладка по трём цветам существует, но жадный решатель
  // приходит к ней редко: ошибка становится видна далеко от того места, где сделана.
  const size_t seeds = 12;
  const auto without = solved_three_colours(15, 0, seeds);
  const auto with = solved_three_colours(15, 256, seeds);

  CHECK(without < seeds); // иначе задача слишком лёгкая и мерить нечего
  CHECK(with > without);
}

TEST_CASE("originator rollbacks keep the result reproducible") {
  const auto table = ladder_rules();

  const auto solve_with_rollbacks = [&](const uint64_t seed) {
    auto target = make_scene(table);
    const auto* tool = registry().find("collapse");
    REQUIRE(tool != nullptr);
    originator::parameters params;
    params.set_number("attempts", 8.0);
    params.set_number("rollbacks", 32.0);
    const std::vector<originator::field_ref> inputs{readable(target.tiles, "weight"),
                                                    readable(target.rules, "allowed")};
    const std::vector<originator::field_ref> outputs{writable(target.cells, "tile"),
                                                     writable(target.state, "attempts")};
    originator::dispatch(*tool, inputs, outputs, params, seed, 0, cell_count, "collapse", nullptr);
    return read_tiles(target);
  };

  // Откат меняет ПУТЬ решателя, но не его определённость: и выбор клетки, и ничья, и бросок кости
  // считаются целыми от зерна.
  CHECK(solve_with_rollbacks(31337) == solve_with_rollbacks(31337));
}

TEST_CASE("originator collapse reports the rollbacks it actually spent") {
  auto weights = make_field_buffer("weights", "v1", 3);
  for (size_t i = 0; i < 3; ++i) {
    weights.field(0).set(i, 1.0);
  }
  auto rules = make_field_buffer("rules", "ui1", 2 * 3 * 3);
  for (size_t axis = 0; axis < 2; ++axis) {
    for (size_t a = 0; a < 3; ++a) {
      for (size_t b = 0; b < 3; ++b) {
        rules.field(0).set((axis * 3 + a) * 3 + b, a == b ? 0.0 : 1.0);
      }
    }
  }

  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);

  double spent_total = 0.0;
  for (uint64_t seed = 1; seed <= 8; ++seed) {
    auto cells = make_raster_buffer("cells", "ui1", 15, 15);
    auto attempts = make_field_buffer("attempts", "ui1", 1);
    auto spent = make_field_buffer("spent", "ui1", 1);

    originator::parameters params;
    params.set_number("wrap", 1.0);
    params.set_number("attempts", 8.0);
    params.set_number("rollbacks", 256.0);

    const std::vector<originator::field_ref> inputs{readable(weights, "value"), readable(rules, "value")};
    const std::vector<originator::field_ref> outputs{writable(cells, "value"), writable(attempts, "value"),
                                                     writable(spent, "value")};
    originator::dispatch(*tool, inputs, outputs, params, seed, 0, 15 * 15, "colours", nullptr);

    CHECK(attempts.field(0).get(0) >= 1.0);
    spent_total += spent.field(0).get(0);
  }

  // Сколько попыток и сколько откатов ушло — единственное, по чему видно, насколько тесны правила.
  // Без этих чисел тесная таблица и просторная выглядят с результата одинаково.
  CHECK(spent_total > 0.0);
}

TEST_CASE("originator collapse catches the short inputs its dispatch cannot") {
  const auto table = ladder_rules();
  auto target = make_scene(table);

  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);

  // Диапазон у `sequential` покрывает только первый ВЫХОД, а входы читаются целиком — значит слишком
  // короткое поле условий диспетчер не поймает, и поймать его обязан инструмент. Иначе он читал бы за
  // концом буфера молча.
  auto narrow = make_field_buffer("narrow", "ui1", 4);
  const std::vector<originator::field_ref> inputs{readable(target.tiles, "weight"),
                                                  readable(target.rules, "allowed"),
                                                  readable(narrow, "value")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "tile")};
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, originator::parameters{}, 1, 0, cell_count,
                                       "collapse", nullptr),
                  std::runtime_error);
}

TEST_CASE("originator collapse refuses patience without memory") {
  const auto table = ladder_rules();
  auto target = make_scene(table);

  const auto* tool = registry().find("collapse");
  REQUIRE(tool != nullptr);

  // Откатываться некуда, а автор объявил, что откатываться можно. Тихо превратить это в перезапуск
  // значило бы исполнять не то, что написано.
  originator::parameters params;
  params.set_number("rollbacks", 32.0);
  params.set_number("history", 0.0);

  const std::vector<originator::field_ref> inputs{readable(target.tiles, "weight"),
                                                  readable(target.rules, "allowed")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "tile")};
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, params, 1, 0, cell_count, "collapse", nullptr),
                  std::runtime_error);
}
