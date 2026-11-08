#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "devils_engine/utils/error.h"

// Общие обходы не знают ни о буферах, ни о способе записать соседство. Each(node, fn)
// перечисляет (сосед, стоимость шага), open(node) задаёт проходимость. Workspace принадлежит
// вызывающему и освобождается независимо от worker; raw label никогда не становится размером массива.
namespace devils_engine::originator {

struct walk_seed {
  uint32_t node = 0;
  uint32_t label = 1;
};

struct edge_workspace {
  std::vector<size_t> offsets;
  std::vector<size_t> cursor;
  size_t retained_bytes() const noexcept { return (offsets.capacity() + cursor.capacity()) * sizeof(size_t); }
  void release() { *this = {}; }
};

// CSR строится в порядке входных рёбер. Хранилище выходов задаёт adapter, поэтому C++
// граф и bound fields не копируются друг через друга. Reserve сообщает число дуг до записи.
template<class Ends, class Cost, class Open, class Offset, class Reserve, class Emit>
void build_edge_adjacency(const size_t count, const size_t edges, Ends ends, Cost cost, Open open,
                          Offset offset, Reserve reserve, Emit emit, edge_workspace& workspace,
                          const std::string_view context) {
  if (count > UINT32_MAX) utils::error{}("originator {}: graph exceeds 32-bit node indices", context);
  workspace.offsets.assign(count + 1, 0);
  for (size_t e = 0; e < edges; ++e) {
    const auto [a, b] = ends(e);
    if (a >= count || b >= count) utils::error{}("originator {}: edge {} joins {}-{} beyond {} nodes", context, e, a, b, count);
    if (!open(a) || !open(b)) continue;
    ++workspace.offsets[a + 1];
    ++workspace.offsets[b + 1];
  }
  for (size_t i = 0; i < count; ++i) {
    workspace.offsets[i + 1] += workspace.offsets[i];
    if (workspace.offsets[i + 1] > UINT32_MAX) utils::error{}("originator {}: graph exceeds 32-bit arc indices", context);
  }
  for (size_t i = 0; i <= count; ++i) offset(i, workspace.offsets[i]);
  reserve(workspace.offsets.back());
  workspace.cursor.assign(workspace.offsets.begin(), workspace.offsets.end() - 1);
  for (size_t e = 0; e < edges; ++e) {
    const auto [a, b] = ends(e);
    if (!open(a) || !open(b)) continue;
    const double weight = cost(e, a, b);
    if (!std::isfinite(weight) || weight < 0) utils::error{}("originator {}: edge {} has invalid cost {}", context, e, weight);
    emit(workspace.cursor[a]++, b, weight);
    emit(workspace.cursor[b]++, a, weight);
  }
}

// BFS хранит предшественников отдельно от весового обхода: равные пути выбираются
// FIFO в порядке дуг adapter, а не приоритетом node/label из Dijkstra.
struct breadth_workspace {
  std::vector<uint32_t> previous;
  std::vector<uint32_t> pending;
  size_t retained_bytes() const noexcept { return (previous.capacity() + pending.capacity()) * sizeof(uint32_t); }
  void release() { *this = {}; }
};

template<class Each>
bool breadth_path(const size_t count, const uint32_t start, const uint32_t goal, Each each,
                  breadth_workspace& workspace, const std::string_view context) {
  if (count > UINT32_MAX || start >= count || goal >= count)
    utils::error{}("originator {}: BFS endpoints {}-{} outside {} nodes", context, start, goal, count);
  workspace.previous.assign(count, UINT32_MAX);
  workspace.pending.clear();
  workspace.pending.reserve(count);
  workspace.previous[start] = start;
  workspace.pending.push_back(start);
  for (size_t head = 0; head < workspace.pending.size() && workspace.previous[goal] == UINT32_MAX; ++head) {
    const auto here = workspace.pending[head];
    each(here, [&](const uint32_t next) {
      if (next >= count) utils::error{}("originator {}: BFS arc {}-{} outside {} nodes", context, here, next, count);
      if (workspace.previous[next] != UINT32_MAX) return false;
      workspace.previous[next] = here;
      workspace.pending.push_back(next);
      return true;
    });
  }
  return workspace.previous[goal] != UINT32_MAX;
}

inline std::vector<uint32_t> reconstruct_path(const breadth_workspace& workspace, const uint32_t start,
                                              const uint32_t goal, const std::string_view context) {
  const auto count = workspace.previous.size();
  if (start >= count || goal >= count || workspace.previous[start] != start)
    utils::error{}("originator {}: invalid BFS reconstruction endpoints", context);
  if (workspace.previous[goal] == UINT32_MAX) return {};
  std::vector<uint32_t> path;
  for (auto node = goal; ; node = workspace.previous[node]) {
    if (node >= count || path.size() >= count) utils::error{}("originator {}: broken BFS predecessor chain", context);
    path.push_back(node);
    if (node == start) break;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

struct walk_workspace {
  struct entry {
    double distance = 0;
    uint32_t label = 0;
    uint32_t node = 0;
    uint32_t previous = 0;
    bool operator<(const entry& other) const noexcept {
      if (distance != other.distance) return distance > other.distance;
      if (label != other.label) return label > other.label;
      if (node != other.node) return node > other.node;
      return previous > other.previous;
    }
  };
  std::vector<entry> heap;
  std::vector<uint32_t> assigned;
  std::vector<uint32_t> labels;
  std::vector<size_t> used;

  size_t retained_bytes() const noexcept {
    return heap.capacity() * sizeof(entry) + (assigned.capacity() + labels.capacity()) * sizeof(uint32_t) +
           used.capacity() * sizeof(size_t);
  }
  void release() { *this = {}; }
};

inline size_t walk_size_add(const size_t a, const size_t b, const std::string_view context) {
  if (b > std::numeric_limits<size_t>::max() - a) utils::error{}("originator {}: scratch size overflow", context);
  return a + b;
}

inline size_t walk_size_multiply(const size_t a, const size_t b, const std::string_view context) {
  if (a && b > std::numeric_limits<size_t>::max() / a) utils::error{}("originator {}: scratch size overflow", context);
  return a * b;
}

// Во время роста heap одновременно живы старый и новый allocations. Резервирование ниже
// ограничивает capacity числом возможных постановок: один source на узел плюс все дуги.
inline size_t walk_footprint(const size_t nodes, const size_t arcs) {
  const auto entries = walk_size_add(nodes, arcs, "walk");
  return walk_size_add(walk_size_multiply(entries, 3 * sizeof(walk_workspace::entry), "walk"),
    walk_size_multiply(nodes, 2 * sizeof(walk_seed) + 4 * sizeof(uint32_t) + 3 * sizeof(size_t), "walk"), "walk");
}

// Finalize вызывается один раз для достигнутого узла. Capacity=0 — обычный nearest-source
// shortest path; иначе метка, исчерпавшая лимит, не занимает узел, его может забрать другая.
// Порядок (distance,label,node,previous) объявлен здесь для всех adapters.
template<class Each, class Open, class Finalize>
void walk_distances(const size_t count, const size_t arc_bound, const std::span<const walk_seed> seeds,
                    Each each, Open open, Finalize finalize, const size_t capacity,
                    walk_workspace& workspace, const std::string_view context) {
  if (count > std::numeric_limits<uint32_t>::max() || seeds.size() > count)
    utils::error{}("originator {}: walk needs at most one source per node and 32-bit node indices", context);
  workspace.heap.clear();
  workspace.assigned.assign(count, 0);
  workspace.labels.clear();
  workspace.used.clear();
  const auto bound = walk_size_add(count, arc_bound, context);
  const auto push = [&](const walk_workspace::entry value) {
    if (workspace.heap.size() >= bound) utils::error{}("originator {}: walk exceeded its declared arc bound", context);
    if (workspace.heap.size() == workspace.heap.capacity()) {
      const auto old = workspace.heap.capacity();
      const auto grown = old > bound / 2 ? bound : std::max<size_t>(1, old * 2);
      workspace.heap.reserve(std::min(bound, grown));
    }
    workspace.heap.push_back(value);
    std::push_heap(workspace.heap.begin(), workspace.heap.end());
  };
  for (const auto source : seeds) {
    if (source.node >= count || source.label == 0)
      utils::error{}("originator {}: invalid walk source node {} label {} for {} nodes", context,
                     source.node, source.label, count);
    if (!open(source.node)) continue;
    push({0.0, source.label, source.node, source.node});
    if (capacity) workspace.labels.push_back(source.label);
  }
  if (capacity) {
    std::sort(workspace.labels.begin(), workspace.labels.end());
    workspace.labels.erase(std::unique(workspace.labels.begin(), workspace.labels.end()), workspace.labels.end());
    workspace.used.assign(workspace.labels.size(), 0);
  }
  while (!workspace.heap.empty()) {
    std::pop_heap(workspace.heap.begin(), workspace.heap.end());
    const auto current = workspace.heap.back();
    workspace.heap.pop_back();
    if (workspace.assigned[current.node]) continue;
    if (capacity) {
      const auto slot = size_t(std::lower_bound(workspace.labels.begin(), workspace.labels.end(), current.label) -
                               workspace.labels.begin());
      if (workspace.used[slot] >= capacity) continue;
      ++workspace.used[slot];
    }
    workspace.assigned[current.node] = current.label;
    finalize(current.node, current.label, current.distance, current.previous);
    each(current.node, [&](const size_t next, const double step) {
      if (next >= count || workspace.assigned[next] || !open(next)) return;
      if (!std::isfinite(step) || step < 0.0 || !std::isfinite(current.distance + step))
        utils::error{}("originator {}: invalid walk cost {} from {} to {}", context, step, current.node, next);
      push({current.distance + step, current.label, uint32_t(next), current.node});
    });
  }
}

// Connected components: порядок новых labels определяется порядком roots, не worker.
// Enter возвращает false для уже посещённого/закрытого узла, иначе помечает его.
template<class Each, class Enter>
void walk_component(const uint32_t root, Each each, Enter enter, std::vector<uint32_t>& stack) {
  stack.clear();
  if (!enter(root)) return;
  stack.push_back(root);
  while (!stack.empty()) {
    const auto node = stack.back();
    stack.pop_back();
    each(node, [&](const size_t other, const double) {
      if (enter(other)) stack.push_back(uint32_t(other));
    });
  }
}

} // namespace devils_engine::originator
