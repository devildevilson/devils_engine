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
