#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

#include "devils_engine/originator/skeleton.h"
#include "devils_engine/originator/graph_walk.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"

// Инструменты СКЕЛЕТА: граф из точек и рёбер, заданных явными списками (см. skeleton.h).
//
// Набор отдельный по способу адресоваться. У графа поверхности соседство — данные на КАЖДУЮ клетку
// (CSR), и число элементов вызова равно числу клеток. У скелета узлов десятки, рёбра лежат отдельным
// буфером и дописываются скриптом по одному: генератор пещеры добавляет ход, спрашивает «не стал ли
// путь короче» и откатывает неудачный вариант, просто уменьшив свой счётчик. Поэтому ЧИСЛО занятых
// узлов и рёбер приходит параметром (`nodes`, `edges`), а ёмкость буферов — объявлением.
//
// Оба инструмента sequential: результат — функция ВСЕГО графа (кратчайший путь до узла зависит от
// рёбер где угодно), и разбивать такую работу на диапазоны нечего.
//
//   skeleton_distance  входы: позиция узла (v3), конец ребра a, конец ребра b, источник (узел с
//                      ненулевым значением — источник), [проходимость]; выход: путь от ближайшего
//                      источника, -1 у недостижимого.
//   skeleton_tour      входы: позиция, a, b, остановка (ненулевое — остановка), [проходимость];
//                      выход: длина обхода в элементе 0 поля; параметр `start` — откуда и куда.

namespace devils_engine::originator {

namespace {

struct adjacency {
  std::vector<uint32_t> offsets;
  std::vector<uint32_t> targets;
  std::vector<double> lengths;
};

double distance_between(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool passable_node(const skeleton_graph& graph, const uint32_t node) {
  return graph.passable.empty() || graph.passable[node] != 0;
}

// Соседство строится на каждый вызов: узлов десятки, а граф между вызовами меняется.
adjacency build_adjacency(const skeleton_graph& graph) {
  const size_t n = graph.nodes.size();
  adjacency out;
  out.offsets.resize(n + 1);
  edge_workspace scratch;
  build_edge_adjacency(n, graph.edges.size(), [&](size_t e) { return graph.edges[e]; },
    [&](size_t, size_t a, size_t b) { return distance_between(graph.nodes[a], graph.nodes[b]); },
    [&](size_t node) { return passable_node(graph, uint32_t(node)); },
    [&](size_t node, size_t offset) { out.offsets[node] = uint32_t(offset); },
    [&](size_t arcs) { out.targets.resize(arcs); out.lengths.resize(arcs); },
    [&](size_t index, size_t node, double length) { out.targets[index] = uint32_t(node); out.lengths[index] = length; },
    scratch, "skeleton");
  return out;
}

// Дейкстра от нескольких источников. Кратчайшее расстояние единственно, поэтому результат не
// зависит от порядка, в котором очередь разбирает равные ключи.
std::vector<double> dijkstra(const skeleton_graph& graph, const adjacency& adj,
                             std::span<const uint32_t> sources, walk_workspace& workspace) {
  const size_t n = graph.nodes.size();
  std::vector<double> distance(n, -1.0);
  std::vector<walk_seed> seeds;
  for (const auto node : sources) {
    if (node >= n) utils::error{}("originator skeleton: source {} is beyond the {} nodes", node, n);
    seeds.push_back({node, 1});
  }
  std::sort(seeds.begin(), seeds.end(), [](const auto a, const auto b) { return a.node < b.node; });
  seeds.erase(std::unique(seeds.begin(), seeds.end(), [](const auto a, const auto b) { return a.node == b.node; }), seeds.end());
  walk_distances(n, adj.targets.size(), seeds,
    [&](const size_t node, auto visit) {
      for (size_t k = adj.offsets[node]; k < adj.offsets[node + 1]; ++k) visit(adj.targets[k], adj.lengths[k]);
    }, [&](const size_t node) { return passable_node(graph, uint32_t(node)); },
    [&](const size_t node, const uint32_t, const double d, const uint32_t) { distance[node] = d; },
    0, workspace, "skeleton");
  return distance;
}

// Скелет из привязок инструмента: позиции, концы рёбер, необязательная проходимость.
struct bound_skeleton {
  std::vector<std::array<double, 3>> nodes;
  std::vector<std::array<uint32_t, 2>> edges;
  std::vector<uint8_t> passable;

  skeleton_graph view() const { return {nodes, edges, passable}; }
};

bound_skeleton read_skeleton(const tool_call& call, const size_t passable_input) {
  const auto positions = call.input(0).read();
  const auto edge_a = call.input(1).read();
  const auto edge_b = call.input(2).read();
  if (positions.type().components < 3) {
    utils::error{}("originator step '{}': {} needs a 3-component node position, '{}' has {}", call.step_name,
                   call.tool_name, call.input(0).field_name(), positions.type().components);
  }
  const auto node_count = size_t(std::max<int64_t>(call.params->integer("nodes", int64_t(positions.count())), 0));
  const auto edge_count = size_t(std::max<int64_t>(call.params->integer("edges", int64_t(edge_a.count())), 0));
  if (node_count > positions.count() || edge_count > edge_a.count() || edge_count > edge_b.count()) {
    utils::error{}("originator step '{}': {} asks for {} nodes and {} edges, the buffers hold {} and {}",
                   call.step_name, call.tool_name, node_count, edge_count, positions.count(),
                   std::min(edge_a.count(), edge_b.count()));
  }

  bound_skeleton out;
  out.nodes.resize(node_count);
  for (size_t i = 0; i < node_count; ++i) {
    out.nodes[i] = {positions.get(i, 0), positions.get(i, 1), positions.get(i, 2)};
  }
  out.edges.resize(edge_count);
  for (size_t i = 0; i < edge_count; ++i) {
    out.edges[i] = {uint32_t(edge_a.get(i)), uint32_t(edge_b.get(i))};
  }
  if (call.has_input(passable_input)) {
    const auto passable = call.input(passable_input).read();
    out.passable.resize(node_count);
    for (size_t i = 0; i < node_count; ++i) out.passable[i] = passable.get(i) != 0.0 ? 1 : 0;
  }
  return out;
}

std::vector<uint32_t> marked(const tool_call& call, const size_t input, const size_t node_count) {
  const auto field = call.input(input).read();
  std::vector<uint32_t> out;
  for (size_t i = 0; i < node_count && i < field.count(); ++i) {
    if (field.get(i) != 0.0) out.push_back(uint32_t(i));
  }
  return out;
}

void tool_skeleton_distance(const tool_call& call, const size_t, const size_t) {
  const bound_skeleton skeleton = read_skeleton(call, 4);
  const auto distance = skeleton_distance(skeleton.view(), marked(call, 3, skeleton.nodes.size()));
  auto out = call.output(0).write();
  if (out.count() < distance.size()) {
    utils::error{}("originator step '{}': skeleton_distance writes {} nodes into '{}' of {}", call.step_name,
                   distance.size(), call.output(0).field_name(), out.count());
  }
  for (size_t i = 0; i < distance.size(); ++i) out.set(i, distance[i]);
}

void tool_skeleton_tour(const tool_call& call, const size_t, const size_t) {
  const bound_skeleton skeleton = read_skeleton(call, 4);
  const int64_t start = call.params->integer("start", -1);
  if (start < 0 || size_t(start) >= skeleton.nodes.size()) {
    utils::error{}("originator step '{}': skeleton_tour needs a start node below {}, got {}", call.step_name,
                   skeleton.nodes.size(), start);
  }
  const auto stops = marked(call, 3, skeleton.nodes.size());
  call.output(0).write().set(0, skeleton_tour(skeleton.view(), uint32_t(start), stops));
}

size_t skeleton_footprint(const tool_call& call) {
  // Узлы, рёбра, соседство в обе стороны, расстояния и очередь: всё линейно по их числу.
  const size_t nodes = call.input(0).count();
  const size_t edges = call.input(1).count();
  // Tour держит таблицу расстояний между всеми отмеченными узлами в худшем случае.
  const auto topology = nodes * (sizeof(std::array<double, 3>) + sizeof(double) * 2 + sizeof(uint32_t) * 4) +
    edges * (sizeof(std::array<uint32_t, 2>) + 2 * (sizeof(uint32_t) + sizeof(double)));
  const auto pairs = call.tool_name == "skeleton_tour" ?
    walk_size_multiply(walk_size_multiply(nodes + 1, nodes + 1, "skeleton tour"), sizeof(double), "skeleton tour") : 0;
  return walk_size_add(walk_size_add(topology, pairs, "skeleton"), walk_footprint(nodes, 2 * edges), "skeleton");
}

} // namespace

std::vector<double> skeleton_distance(const skeleton_graph& graph, std::span<const uint32_t> sources,
                                      walk_workspace& workspace) {
  if (!graph.passable.empty() && graph.passable.size() != graph.nodes.size()) {
    utils::error{}("originator skeleton: passability lists {} nodes, the graph has {}", graph.passable.size(),
                   graph.nodes.size());
  }
  return dijkstra(graph, build_adjacency(graph), sources, workspace);
}

std::vector<double> skeleton_distance(const skeleton_graph& graph, std::span<const uint32_t> sources) {
  walk_workspace workspace;
  return skeleton_distance(graph, sources, workspace);
}

double skeleton_tour(const skeleton_graph& graph, const uint32_t start, std::span<const uint32_t> stops) {
  if (!graph.passable.empty() && graph.passable.size() != graph.nodes.size())
    utils::error{}("originator skeleton tour: passability size does not match nodes");
  if (start >= graph.nodes.size()) utils::error{}("originator skeleton tour: start {} is outside nodes", start);
  for (const auto stop : stops)
    if (stop >= graph.nodes.size()) utils::error{}("originator skeleton tour: stop {} is outside nodes", stop);
  walk_workspace workspace;
  const adjacency adj = build_adjacency(graph);
  // Пути только между остановками и стартом: остановок единицы, и обход от каждой дешевле, чем все
  // пары узлов скелета.
  std::vector<uint32_t> points{start};
  for (const uint32_t stop : stops) {
    if (stop != start) points.push_back(stop);
  }
  const size_t k = points.size();
  std::vector<double> d(k * k, 0.0);
  for (size_t i = 0; i < k; ++i) {
    const std::array<uint32_t, 1> from{points[i]};
    const auto row = dijkstra(graph, adj, from, workspace);
    for (size_t j = 0; j < k; ++j) {
      if (row[points[j]] < 0.0) return -1.0;
      d[i * k + j] = row[points[j]];
    }
  }
  // Номера в tour — позиции в points; 0 — старт.
  std::vector<size_t> tour{0};
  std::vector<size_t> left;
  for (size_t i = 1; i < k; ++i) left.push_back(i);
  while (!left.empty()) {
    size_t best = 0;
    for (size_t c = 1; c < left.size(); ++c) {
      if (d[tour.back() * k + left[c]] < d[tour.back() * k + left[best]]) best = c;
    }
    tour.push_back(left[best]);
    left.erase(left.begin() + long(best));
  }
  tour.push_back(0);
  for (bool improved = true; improved;) {
    improved = false;
    for (size_t i = 1; i + 2 < tour.size(); ++i) {
      for (size_t j = i + 1; j + 1 < tour.size(); ++j) {
        const double before = d[tour[i - 1] * k + tour[i]] + d[tour[j] * k + tour[j + 1]];
        const double after = d[tour[i - 1] * k + tour[j]] + d[tour[i] * k + tour[j + 1]];
        if (after + 1e-9 < before) {
          std::reverse(tour.begin() + long(i), tour.begin() + long(j) + 1);
          improved = true;
        }
      }
    }
  }
  double total = 0.0;
  for (size_t i = 0; i + 1 < tour.size(); ++i) total += d[tour[i] * k + tour[i + 1]];
  return total;
}

void tool_registry::add_skeleton_tools() {
  add(tool_description{.name = "skeleton_distance", .shape = aperture::sequential, .input_count = 5,
                       .optional_inputs = 1, .output_count = 1, .body = tool_skeleton_distance,
                       .footprint = skeleton_footprint});
  add(tool_description{.name = "skeleton_tour", .shape = aperture::sequential, .input_count = 5,
                       .optional_inputs = 1, .output_count = 1, .body = tool_skeleton_tour,
                       .footprint = skeleton_footprint});
}

} // namespace devils_engine::originator
