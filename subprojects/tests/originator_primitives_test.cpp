#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/primitives.h"
#include "devils_engine/thread/atomic_pool.h"

using namespace devils_engine;

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

// Закодированное дерево узлов из редактора FastNoise2. Именно так граф шума становится данными
// конфига, а не кодом.
constexpr std::string_view node_tree = "DQkGDA==";

constexpr size_t grid_width = 256;
constexpr size_t grid_count = grid_width * grid_width;
constexpr size_t site_count = 64;

originator::buffer make_cells(const originator::storage_kind::values storage) {
  const std::vector<field_pair> fields = {
    {"height", "v1"},
    {"label", "us1"},
    {"weight", "v1"},
  };
  auto layout = originator::make_buffer_layout(storage, fields, "cells");
  return originator::buffer("cells", std::move(layout), grid_count);
}

originator::buffer make_sites() {
  const std::vector<field_pair> fields = {{"position", "v2"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "sites");
  originator::buffer sites("sites", std::move(layout), site_count);

  auto position = sites.field(0);
  // Детерминированная россыпь: важно только то, что точки не на решётке.
  for (size_t i = 0; i < site_count; ++i) {
    const double x = double((i * 7919u) % grid_width);
    const double y = double((i * 104729u) % grid_width);
    position.set(i, x, 0);
    position.set(i, y, 1);
  }
  return sites;
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

originator::tool_registry make_registry() {
  originator::tool_registry registry;
  registry.add_standard_tools();
  originator::add_all_primitives(registry);
  return registry;
}

originator::parameters noise_params() {
  originator::parameters params;
  params.set_string("tree", std::string(node_tree));
  params.set_number("width", double(grid_width));
  params.set_number("frequency", 0.02);
  return params;
}
} // namespace

TEST_CASE("originator noise_grid is bit-identical at any number of threads") {
  const auto registry = make_registry();
  const auto* noise = registry.find("noise_grid");
  REQUIRE(noise != nullptr);

  const auto params = noise_params();

  auto reference = make_cells(originator::storage_kind::soa);
  const std::vector<originator::field_ref> reference_out{writable(reference, "height")};
  originator::dispatch(*noise, {}, reference_out, params, 4242, 0, grid_count, "terrain", nullptr);
  const auto expected = reference.field(reference.find_field("height"));

  // Целые строки генерируются даже когда чанк начался посреди строки: иначе GenUniformGrid2D
  // накапливал бы позиции с другого места и последний бит расходился бы.
  for (const size_t threads : {size_t(1), size_t(3), size_t(7)}) {
    thread::atomic_pool pool(threads);
    auto cells = make_cells(originator::storage_kind::soa);
    const std::vector<originator::field_ref> out{writable(cells, "height")};
    originator::dispatch(*noise, {}, out, params, 4242, 0, grid_count, "terrain", &pool);

    const auto actual = cells.field(cells.find_field("height"));
    bool identical = true;
    for (size_t i = 0; i < grid_count; ++i) {
      identical = identical && actual.get(i) == expected.get(i);
    }
    CHECK(identical);
  }

  // Поле действительно заполнено, а не оставлено нулями.
  double lowest = expected.get(0);
  double highest = expected.get(0);
  for (size_t i = 0; i < grid_count; ++i) {
    lowest = std::min(lowest, expected.get(i));
    highest = std::max(highest, expected.get(i));
  }
  CHECK(highest > lowest);
}

TEST_CASE("originator noise_grid gives the same values in both storage layouts") {
  const auto registry = make_registry();
  const auto* noise = registry.find("noise_grid");
  const auto params = noise_params();

  auto soa = make_cells(originator::storage_kind::soa);
  auto aos = make_cells(originator::storage_kind::aos);

  const std::vector<originator::field_ref> soa_out{writable(soa, "height")};
  const std::vector<originator::field_ref> aos_out{writable(aos, "height")};
  originator::dispatch(*noise, {}, soa_out, params, 11, 0, grid_count, "terrain", nullptr);
  originator::dispatch(*noise, {}, aos_out, params, 11, 0, grid_count, "terrain", nullptr);

  const auto left = soa.field(soa.find_field("height"));
  const auto right = aos.field(aos.find_field("height"));
  for (size_t i = 0; i < grid_count; i += 37) {
    CHECK(left.get(i) == right.get(i));
  }
}

TEST_CASE("originator noise_at samples a position field") {
  const auto registry = make_registry();
  const auto* noise = registry.find("noise_at");
  REQUIRE(noise != nullptr);

  auto sites = make_sites();
  const std::vector<field_pair> value_fields = {{"value", "v1"}};
  auto values = originator::buffer("values", originator::make_buffer_layout(originator::storage_kind::soa, value_fields, "values"), site_count);

  originator::parameters params;
  params.set_string("tree", std::string(node_tree));
  params.set_number("frequency", 0.05);

  const std::vector<originator::field_ref> inputs{readable(sites, "position")};
  const std::vector<originator::field_ref> outputs{writable(values, "value")};
  originator::dispatch(*noise, inputs, outputs, params, 7, 0, site_count, "sample", nullptr);

  const auto sampled = values.field(0);
  double lowest = sampled.get(0);
  double highest = sampled.get(0);
  for (size_t i = 0; i < site_count; ++i) {
    lowest = std::min(lowest, sampled.get(i));
    highest = std::max(highest, sampled.get(i));
  }
  CHECK(highest > lowest);

  // Путь по массиву позиций устойчив к разбиению: позиции заданы явно и ничего не накапливается.
  auto again = originator::buffer("values", originator::make_buffer_layout(originator::storage_kind::soa, value_fields, "values"), site_count);
  const std::vector<originator::field_ref> again_out{writable(again, "value")};
  thread::atomic_pool pool(3);
  originator::dispatch(*noise, inputs, again_out, params, 7, 0, site_count, "sample", &pool);
  const auto parallel = again.field(0);
  for (size_t i = 0; i < site_count; ++i) {
    CHECK(parallel.get(i) == sampled.get(i));
  }
}

TEST_CASE("originator voronoi_label matches an exhaustive nearest-site search") {
  const auto registry = make_registry();
  const auto* label = registry.find("voronoi_label");
  REQUIRE(label != nullptr);
  CHECK(label->shape == originator::aperture::gather);

  auto sites = make_sites();
  auto cells = make_cells(originator::storage_kind::soa);

  originator::parameters params;
  params.set_number("width", double(grid_width));
  params.set_number("height", double(grid_width));

  const std::vector<originator::field_ref> inputs{readable(sites, "position")};
  const std::vector<originator::field_ref> outputs{writable(cells, "label")};
  originator::dispatch(*label, inputs, outputs, params, 1, 0, grid_count, "zoning", nullptr);

  const auto position = sites.field(0);
  const auto labels = cells.field(cells.find_field("label"));

  // Полный перебор — эталон: kd-дерево обязано давать тот же ответ, что и линейный поиск.
  for (size_t i = 0; i < grid_count; i += 419) {
    const double x = double(i % grid_width);
    const double y = double(i / grid_width);
    size_t best = 0;
    double best_distance = std::numeric_limits<double>::infinity();
    for (size_t s = 0; s < site_count; ++s) {
      const double dx = position.get(s, 0) - x;
      const double dy = position.get(s, 1) - y;
      const double distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best_distance = distance;
        best = s;
      }
    }
    CHECK(labels.get(i) == double(best));
  }

  // Разметка не зависит от числа потоков: дерево строится подготовкой один раз и только читается.
  for (const size_t threads : {size_t(1), size_t(5)}) {
    thread::atomic_pool pool(threads);
    auto parallel_cells = make_cells(originator::storage_kind::soa);
    const std::vector<originator::field_ref> parallel_out{writable(parallel_cells, "label")};
    originator::dispatch(*label, inputs, parallel_out, params, 1, 0, grid_count, "zoning", &pool);

    const auto parallel_labels = parallel_cells.field(parallel_cells.find_field("label"));
    bool identical = true;
    for (size_t i = 0; i < grid_count; ++i) {
      identical = identical && parallel_labels.get(i) == labels.get(i);
    }
    CHECK(identical);
  }
}

TEST_CASE("originator voronoi_adjacency builds a symmetric canonical graph") {
  const auto registry = make_registry();
  const auto* adjacency = registry.find("voronoi_adjacency");
  REQUIRE(adjacency != nullptr);
  CHECK(adjacency->shape == originator::aperture::scatter);

  auto sites = make_sites();

  const std::vector<field_pair> offset_fields = {{"start", "ui1"}};
  const std::vector<field_pair> arc_fields = {{"site", "us1"}};
  auto offsets = originator::buffer("offsets", originator::make_buffer_layout(originator::storage_kind::soa, offset_fields, "offsets"), site_count + 1);
  auto arcs = originator::buffer("arcs", originator::make_buffer_layout(originator::storage_kind::soa, arc_fields, "arcs"), site_count * 16);

  originator::parameters params;
  params.set_number("width", double(grid_width));
  params.set_number("height", double(grid_width));

  const std::vector<originator::field_ref> inputs{readable(sites, "position")};
  const std::vector<originator::field_ref> outputs{writable(offsets, "start"), writable(arcs, "site")};
  originator::dispatch(*adjacency, inputs, outputs, params, 1, 0, site_count, "topology", nullptr);

  const auto start = offsets.field(0);
  const auto neighbour = arcs.field(0);

  const auto total = size_t(start.get(site_count));
  CHECK(total > 0);
  CHECK(start.get(0) == 0.0);

  // Соседство неориентированное: если b есть у a, то a есть у b.
  size_t degree_sum = 0;
  for (size_t site = 0; site < site_count; ++site) {
    const auto first = size_t(start.get(site));
    const auto last = size_t(start.get(site + 1));
    CHECK(last >= first);
    degree_sum += last - first;

    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(neighbour.get(k));
      CHECK(other != site);

      bool mirrored = false;
      const auto other_first = size_t(start.get(other));
      const auto other_last = size_t(start.get(other + 1));
      for (size_t j = other_first; j < other_last; ++j) {
        mirrored = mirrored || size_t(neighbour.get(j)) == site;
      }
      CHECK(mirrored);
    }

    // Канонизация: список каждого сайта отсортирован строго по возрастанию, значит без повторов.
    for (size_t k = first + 1; k < last; ++k) {
      CHECK(neighbour.get(k - 1) < neighbour.get(k));
    }
  }
  CHECK(degree_sum == total);

  // Тот же вход — тот же граф.
  auto repeat_offsets = originator::buffer("offsets", originator::make_buffer_layout(originator::storage_kind::soa, offset_fields, "offsets"), site_count + 1);
  auto repeat_arcs = originator::buffer("arcs", originator::make_buffer_layout(originator::storage_kind::soa, arc_fields, "arcs"), site_count * 16);
  const std::vector<originator::field_ref> repeat_out{writable(repeat_offsets, "start"), writable(repeat_arcs, "site")};
  originator::dispatch(*adjacency, inputs, repeat_out, params, 1, 0, site_count, "topology", nullptr);
  for (size_t i = 0; i <= site_count; ++i) {
    CHECK(repeat_offsets.field(0).get(i) == start.get(i));
  }
  for (size_t i = 0; i < total; ++i) {
    CHECK(repeat_arcs.field(0).get(i) == neighbour.get(i));
  }
}

TEST_CASE("originator voronoi_adjacency refuses a neighbours buffer that is too small") {
  const auto registry = make_registry();
  const auto* adjacency = registry.find("voronoi_adjacency");

  auto sites = make_sites();
  const std::vector<field_pair> offset_fields = {{"start", "ui1"}};
  const std::vector<field_pair> arc_fields = {{"site", "us1"}};
  auto offsets = originator::buffer("offsets", originator::make_buffer_layout(originator::storage_kind::soa, offset_fields, "offsets"), site_count + 1);
  auto tiny = originator::buffer("arcs", originator::make_buffer_layout(originator::storage_kind::soa, arc_fields, "arcs"), 4);

  originator::parameters params;
  const std::vector<originator::field_ref> inputs{readable(sites, "position")};
  const std::vector<originator::field_ref> outputs{writable(offsets, "start"), writable(tiny, "site")};

  CHECK_THROWS_AS(originator::dispatch(*adjacency, inputs, outputs, params, 1, 0, site_count, "topology", nullptr),
                  std::runtime_error);
}
