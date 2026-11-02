#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/skeleton.h"
#include "devils_engine/originator/tools.h"

using namespace devils_engine;

// Скелет: узел 0 непроходим (у пещеры это полость узла маршрута), ребро 0-4 через него не
// сокращает путь; узел 5 оторван.
//
//   0 --- 1 ----- 2 ------- 4         5
//   \             |
//    `-----4      3
namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

const std::vector<std::array<double, 3>> points{{0, 0, 0}, {1, 0, 0}, {4, 0, 0}, {4, 3, 0}, {8, 0, 0}, {100, 0, 0}};
const std::vector<std::array<uint32_t, 2>> pairs{{0, 1}, {1, 2}, {2, 3}, {2, 4}, {0, 4}};

originator::buffer make_nodes() {
  const std::vector<field_pair> fields = {{"position", "v3"}, {"mark", "ub1"}, {"passable", "ub1"}, {"distance", "v1"}};
  originator::buffer nodes("nodes", originator::make_buffer_layout(originator::storage_kind::soa, fields, "nodes"), 8);
  for (size_t i = 0; i < points.size(); ++i) {
    for (uint32_t c = 0; c < 3; ++c) nodes.field(0).set(i, points[i][c], c);
    nodes.field(2).set(i, i == 0 ? 0.0 : 1.0);
  }
  return nodes;
}

originator::buffer make_edges() {
  const std::vector<field_pair> fields = {{"a", "ui1"}, {"b", "ui1"}};
  originator::buffer edges("edges", originator::make_buffer_layout(originator::storage_kind::soa, fields, "edges"), 8);
  for (size_t i = 0; i < pairs.size(); ++i) {
    edges.field(0).set(i, pairs[i][0]);
    edges.field(1).set(i, pairs[i][1]);
  }
  return edges;
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}
originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}
} // namespace

TEST_CASE("originator skeleton_distance follows edges and skips impassable nodes") {
  const std::vector<uint8_t> passable{0, 1, 1, 1, 1, 1};
  const originator::skeleton_graph graph{points, pairs, passable};
  const std::array<uint32_t, 1> sources{1};
  const auto d = originator::skeleton_distance(graph, sources);
  REQUIRE(d.size() == 6);
  CHECK(d[0] == -1.0);
  CHECK(d[1] == doctest::Approx(0.0));
  CHECK(d[2] == doctest::Approx(3.0));
  CHECK(d[3] == doctest::Approx(6.0));
  CHECK(d[4] == doctest::Approx(7.0)); // не 1 + 8 через узел 0 и не по прямой
  CHECK(d[5] == -1.0);

  // Без непроходимости ребро 0-4 сокращает путь до 4: 1 + 8 > 7, а до 0 — 1.
  const originator::skeleton_graph open{points, pairs, {}};
  const auto o = originator::skeleton_distance(open, sources);
  CHECK(o[0] == doctest::Approx(1.0));
  CHECK(o[4] == doctest::Approx(7.0));
}

TEST_CASE("originator skeleton_tour returns the closed tour and refuses unreachable stops") {
  const std::vector<uint8_t> passable{0, 1, 1, 1, 1, 1};
  const originator::skeleton_graph graph{points, pairs, passable};
  const std::array<uint32_t, 2> stops{3, 4};
  CHECK(originator::skeleton_tour(graph, 1, stops) == doctest::Approx(20.0));
  const std::array<uint32_t, 1> lost{5};
  CHECK(originator::skeleton_tour(graph, 1, lost) == -1.0);
}

TEST_CASE("originator skeleton tools read counts from parameters, not capacities") {
  originator::tool_registry registry;
  registry.add_skeleton_tools();
  const auto* distance = registry.find("skeleton_distance");
  const auto* tour = registry.find("skeleton_tour");
  REQUIRE(distance != nullptr);
  REQUIRE(tour != nullptr);
  CHECK(distance->shape == originator::aperture::sequential);

  auto nodes = make_nodes();
  auto edges = make_edges();
  nodes.field(1).set(1, 1.0); // источник

  originator::parameters params;
  params.set_number("nodes", 6);
  params.set_number("edges", 5);
  const std::vector<originator::field_ref> inputs{readable(nodes, "position"), readable(edges, "a"), readable(edges, "b"),
                                                  readable(nodes, "mark"), readable(nodes, "passable")};
  const std::vector<originator::field_ref> outputs{writable(nodes, "distance")};
  originator::dispatch(*distance, inputs, outputs, params, 1, 0, 8, "skeleton", nullptr);
  CHECK(nodes.field(3).get(4) == doctest::Approx(7.0));
  CHECK(nodes.field(3).get(0) == -1.0);

  // Остановки — тот же признак; обход — в элемент 0 выхода.
  nodes.field(1).set(1, 0.0);
  nodes.field(1).set(3, 1.0);
  nodes.field(1).set(4, 1.0);
  params.set_number("start", 1);
  originator::dispatch(*tour, inputs, outputs, params, 1, 0, 8, "skeleton", nullptr);
  CHECK(nodes.field(3).get(0) == doctest::Approx(20.0));

  // Ребро за пределами занятых узлов — громкий отказ, а не тихо пропущенное ребро.
  edges.field(1).set(4, 7);
  CHECK_THROWS(originator::dispatch(*distance, inputs, outputs, params, 1, 0, 8, "skeleton", nullptr));
}
