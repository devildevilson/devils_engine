#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/neighbourhood.h"
#include "devils_engine/originator/skeleton.h"
#include "devils_engine/originator/tools.h"

using namespace devils_engine;

// Соседство как привязка: общие blur / frontier / flood над CSR и над трафаретом, и обещание
// объединения — они дают то же, что инструменты-близнецы, бит в бит.
namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

originator::field_ref w(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}
originator::field_ref r(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

originator::tool_registry tools() {
  originator::tool_registry registry;
  registry.add_standard_tools();
  registry.add_graph_tools();
  registry.add_neighbourhood_tools();
  return registry;
}

constexpr size_t side = 9;

originator::buffer raster(const char* name) {
  const std::vector<field_pair> fields = {{"value", "v1"}, {"label", "ui1"}, {"out", "v1"}, {"twin", "v1"}};
  originator::buffer b(name, originator::make_buffer_layout(originator::storage_kind::soa, fields, name),
                       originator::buffer_extent{side, side, 0});
  for (size_t i = 0; i < side * side; ++i) {
    b.field(0).set(i, double((i * 37) % 11) * 0.25);
    b.field(1).set(i, (i % side) < 4 ? 1.0 : 2.0);
  }
  return b;
}
} // namespace

TEST_CASE("blur with a box stencil is box_blur, with CSR it is graph_blur") {
  auto registry = tools();
  auto grid = raster("grid");
  auto twin = raster("twin");
  originator::parameters params;
  params.set_number("radius", 2);
  params.set_string("stencil", "box");
  originator::dispatch(*registry.find("blur"), std::vector{r(grid, "value")}, std::vector{w(grid, "out")}, params, 1, 0,
                       side * side, "test", nullptr);
  originator::parameters box;
  box.set_number("radius", 2);
  originator::dispatch(*registry.find("box_blur"), std::vector{r(twin, "value")}, std::vector{w(twin, "twin")}, box, 1,
                       0, side * side, "test", nullptr);
  for (size_t i = 0; i < side * side; ++i) CHECK(grid.field(2).get(i) == twin.field(3).get(i));

  // Крест радиуса 1 с весами: среднее себя (вес 2) и четырёх соседей (вес 1) в середине растра.
  originator::parameters cross;
  cross.set_number("self_weight", 2.0);
  originator::dispatch(*registry.find("blur"), std::vector{r(grid, "value")}, std::vector{w(grid, "out")}, cross, 1, 0,
                       side * side, "test", nullptr);
  const size_t c = 4 * side + 4;
  const auto v = [&](const size_t i) { return grid.field(0).get(i); };
  CHECK(grid.field(2).get(c) == doctest::Approx((2.0 * v(c) + v(c - 1) + v(c + 1) + v(c - side) + v(c + side)) / 6.0));

  // CSR: цепочка 0-1-2-3, blur и graph_blur — одно ядро.
  const std::vector<field_pair> graph_fields = {{"offsets", "ui1"}, {"arcs", "ui1"}};
  originator::buffer csr("csr", originator::make_buffer_layout(originator::storage_kind::soa, graph_fields, "csr"), 8);
  const std::array<double, 5> offsets{0, 1, 3, 5, 6};
  const std::array<double, 6> arcs{1, 0, 2, 1, 3, 2};
  for (size_t i = 0; i < offsets.size(); ++i) csr.field(0).set(i, offsets[i]);
  for (size_t i = 0; i < arcs.size(); ++i) csr.field(1).set(i, arcs[i]);
  const std::vector<field_pair> line_fields = {{"value", "v1"}, {"a", "v1"}, {"b", "v1"}};
  originator::buffer line("line", originator::make_buffer_layout(originator::storage_kind::soa, line_fields, "line"), 4);
  for (size_t i = 0; i < 4; ++i) line.field(0).set(i, double(i * i));
  originator::parameters weights;
  weights.set_number("self_weight", 3.0);
  originator::dispatch(*registry.find("blur"), std::vector{r(line, "value"), r(csr, "offsets"), r(csr, "arcs")},
                       std::vector{w(line, "a")}, weights, 1, 0, 4, "test", nullptr);
  originator::dispatch(*registry.find("graph_blur"), std::vector{r(csr, "offsets"), r(csr, "arcs"), r(line, "value")},
                       std::vector{w(line, "b")}, weights, 1, 0, 4, "test", nullptr);
  for (size_t i = 0; i < 4; ++i) CHECK(line.field(1).get(i) == line.field(2).get(i));
}

TEST_CASE("frontier over a stencil marks both sides of a label border") {
  auto registry = tools();
  auto grid = raster("grid");
  originator::parameters params;
  originator::dispatch(*registry.find("frontier"), std::vector{r(grid, "label")}, std::vector{w(grid, "out")}, params, 1,
                       0, side * side, "test", nullptr);
  for (size_t y = 0; y < side; ++y) {
    for (size_t x = 0; x < side; ++x) {
      CHECK(grid.field(2).get(y * side + x) == ((x == 3 || x == 4) ? 1.0 : 0.0));
    }
  }
}

TEST_CASE("slope and vote: a stencil by hand, CSR is graph_slope / graph_vote") {
  auto registry = tools();
  auto grid = raster("grid");
  originator::parameters params;
  originator::dispatch(*registry.find("slope"), std::vector{r(grid, "value")}, std::vector{w(grid, "out")}, params, 1,
                       0, side * side, "test", nullptr);
  const size_t c = 4 * side + 4;
  const auto v = [&](const size_t i) { return grid.field(0).get(i); };
  const double around = std::abs(v(c - 1) - v(c)) + std::abs(v(c + 1) - v(c)) + std::abs(v(c - side) - v(c)) +
                        std::abs(v(c + side) - v(c));
  CHECK(grid.field(2).get(c) == doctest::Approx(around / 4.0));

  // Цепочка 0-1-2-3: метки 5 0 0 7, веса соседей 1 2 2 1 — узел 1 видит только 5, узел 2 только 7
  // (у узла 1 сосед 2 без метки). Ядро одно, поэтому vote и graph_vote совпадают бит в бит.
  const std::vector<field_pair> graph_fields = {{"offsets", "ui1"}, {"arcs", "ui1"}};
  originator::buffer csr("csr", originator::make_buffer_layout(originator::storage_kind::soa, graph_fields, "csr"), 8);
  const std::array<double, 5> offsets{0, 1, 3, 5, 6};
  const std::array<double, 6> arcs{1, 0, 2, 1, 3, 2};
  for (size_t i = 0; i < offsets.size(); ++i) csr.field(0).set(i, offsets[i]);
  for (size_t i = 0; i < arcs.size(); ++i) csr.field(1).set(i, arcs[i]);
  const std::vector<field_pair> line_fields = {{"label", "ui1"}, {"weight", "v1"}, {"mask", "ub1"}, {"a", "v1"}, {"b", "v1"}};
  originator::buffer line("line", originator::make_buffer_layout(originator::storage_kind::soa, line_fields, "line"), 4);
  const std::array<double, 4> labels{5, 0, 0, 7}, weights{1, 2, 2, 1};
  for (size_t i = 0; i < 4; ++i) {
    line.field(0).set(i, labels[i]);
    line.field(1).set(i, weights[i]);
    line.field(2).set(i, 1.0);
  }
  originator::dispatch(*registry.find("vote"),
                       std::vector{r(line, "label"), r(line, "weight"), r(line, "mask"), r(csr, "offsets"), r(csr, "arcs")},
                       std::vector{w(line, "a")}, params, 1, 0, 4, "test", nullptr);
  originator::dispatch(*registry.find("graph_vote"),
                       std::vector{r(csr, "offsets"), r(csr, "arcs"), r(line, "label"), r(line, "weight"), r(line, "mask")},
                       std::vector{w(line, "b")}, params, 1, 0, 4, "test", nullptr);
  const std::array<double, 4> expected{5, 5, 7, 7};
  for (size_t i = 0; i < 4; ++i) {
    CHECK(line.field(3).get(i) == expected[i]);
    CHECK(line.field(4).get(i) == line.field(3).get(i));
  }
}

TEST_CASE("flood over edge_csr is skeleton_distance") {
  auto registry = tools();
  const std::vector<std::array<double, 3>> points{{0, 0, 0}, {1, 0, 0}, {4, 0, 0}, {4, 3, 0}, {8, 0, 0}, {100, 0, 0}};
  const std::vector<std::array<uint32_t, 2>> pairs{{0, 1}, {1, 2}, {2, 3}, {2, 4}, {0, 4}};
  const std::vector<field_pair> node_fields = {{"position", "d3"}, {"seed", "ui1"}, {"cost", "v1"},
                                               {"open", "ub1"},    {"label", "ui1"}, {"distance", "d1"}};
  originator::buffer nodes("nodes", originator::make_buffer_layout(originator::storage_kind::aos, node_fields, "nodes"),
                           points.size());
  const std::vector<field_pair> edge_fields = {{"a", "ui1"}, {"b", "ui1"}};
  originator::buffer edges("edges", originator::make_buffer_layout(originator::storage_kind::aos, edge_fields, "edges"),
                           pairs.size());
  const std::vector<field_pair> csr_fields = {{"offsets", "ui1"}, {"arcs", "ui1"}, {"length", "d1"}};
  originator::buffer csr("csr", originator::make_buffer_layout(originator::storage_kind::soa, csr_fields, "csr"), 16);
  for (size_t i = 0; i < points.size(); ++i) {
    for (uint32_t k = 0; k < 3; ++k) nodes.field(0).set(i, points[i][k], k);
    nodes.field(3).set(i, 1.0);
  }
  for (size_t e = 0; e < pairs.size(); ++e) {
    edges.field(0).set(e, pairs[e][0]);
    edges.field(1).set(e, pairs[e][1]);
  }
  originator::parameters build;
  originator::dispatch(*registry.find("edge_csr"), std::vector{r(nodes, "position"), r(edges, "a"), r(edges, "b")},
                       std::vector{w(csr, "offsets"), w(csr, "arcs"), w(csr, "length")}, build, 1, 0, 1, "test",
                       nullptr);

  nodes.field(1).set(1, 1.0); // источник
  originator::parameters flood;
  originator::dispatch(*registry.find("flood"),
                       std::vector{r(nodes, "seed"), r(nodes, "cost"), r(nodes, "open"), r(csr, "offsets"),
                                   r(csr, "arcs"), r(csr, "length")},
                       std::vector{w(nodes, "label"), w(nodes, "distance")}, flood, 1, 0, points.size(), "test",
                       nullptr);

  const std::array<uint32_t, 1> sources{1};
  const auto reference = originator::skeleton_distance({points, pairs, {}}, sources);
  for (size_t i = 0; i < points.size(); ++i) CHECK(nodes.field(5).get(i) == reference[i]);
  CHECK(nodes.field(4).get(5) == 0.0); // оторванный узел не достигнут
}

TEST_CASE("neighbourhood refusals") {
  auto registry = tools();
  const std::vector<field_pair> fields = {{"value", "v1"}, {"out", "v1"}};
  originator::buffer flat("flat", originator::make_buffer_layout(originator::storage_kind::soa, fields, "flat"), 16);
  originator::parameters params;
  // Линейный буфер без CSR: соседей нет.
  CHECK_THROWS(originator::dispatch(*registry.find("blur"), std::vector{r(flat, "value")}, std::vector{w(flat, "out")},
                                    params, 1, 0, 16, "test", nullptr));
  // Неизвестная форма трафарета.
  auto grid = raster("grid");
  params.set_string("stencil", "hex");
  CHECK_THROWS(originator::dispatch(*registry.find("blur"), std::vector{r(grid, "value")}, std::vector{w(grid, "out")},
                                    params, 1, 0, side * side, "test", nullptr));
}
