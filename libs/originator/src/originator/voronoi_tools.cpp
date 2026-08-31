#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#define JC_VORONOI_IMPLEMENTATION
#include <jc_voronoi.h>

#include "devils_engine/originator/primitives.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/kd_tree.h"

// Обвязка jc_voronoi плюс запрос ближайшего сайта.
//
// Два инструмента отвечают на РАЗНЫЕ вопросы, и путать их не стоит:
//
//   voronoi_label     "какому сайту принадлежит эта клетка" — растровая разметка. Это запрос
//                     ближайшего сайта, диаграмма для него не нужна: kd-дерево строится один раз
//                     подготовкой, дальше запросы независимы, и проход спокойно параллелится.
//   voronoi_adjacency "какие области соседствуют" — точная топология. Растр на такой вопрос
//                     отвечает плохо: тонкая область может не получить ни одной клетки, а две
//                     области могут коснуться по диагонали. Здесь считается триангуляция Делоне,
//                     где ребро между сайтами и есть соседство по построению.
//
// Соседи в CSR отсортированы по возрастанию, поэтому результат не зависит от порядка обхода
// диаграммы и его можно честно сравнивать между запусками.

namespace devils_engine {
namespace originator {

namespace {

using site_tree = utils::kd_tree<uint32_t, std::array<float, 2>, 2>;

size_t read_site_count(const tool_call& call) {
  const auto declared = call.params->integer("site_count", 0);
  const size_t available = call.input(0).count();
  if (declared <= 0) {
    return available;
  }
  const auto count = size_t(declared);
  if (count > available) {
    utils::error{}("originator step '{}': tool '{}' was given site_count {}, but '{}.{}' holds only {} elements",
                   call.step_name, call.tool_name, count,
                   call.input(0).buffer_name(), call.input(0).field_name(), available);
  }
  return count;
}

void require_position_field(const tool_call& call) {
  const auto components = call.input(0).type().components;
  if (components < 2) {
    utils::error{}("originator step '{}': tool '{}' needs a 2-component site position field, '{}.{}' has {}",
                   call.step_name, call.tool_name,
                   call.input(0).buffer_name(), call.input(0).field_name(), components);
  }
}

std::shared_ptr<void> prepare_site_tree(const tool_call& call) {
  require_position_field(call);

  const size_t site_count = read_site_count(call);
  if (site_count == 0) {
    utils::error{}("originator step '{}': tool '{}' got no sites", call.step_name, call.tool_name);
  }

  const auto positions = call.input(0).read();
  auto tree = std::make_shared<site_tree>();
  tree->reserve(site_count);
  for (size_t i = 0; i < site_count; ++i) {
    tree->insert({float(positions.get(i, 0)), float(positions.get(i, 1))}, uint32_t(i));
  }
  tree->build();
  return tree;
}

void tool_voronoi_label(const tool_call& call, const size_t begin, const size_t end) {
  const auto& tree = *static_cast<const site_tree*>(call.shared);
  auto target = call.output(0).write();

  const auto width = size_t(std::max<int64_t>(call.params->integer("width", 1), 1));
  const auto height = size_t(std::max<int64_t>(call.params->integer("height", int64_t(width)), 1));
  const float max_radius = float(call.params->number("max_radius", double(width + height)));
  (void)height; // участвует только в радиусе по умолчанию: разметка адресуется линейным индексом

  const auto accept_any = [](const uint32_t&) { return true; };

  for (size_t i = begin; i < end; ++i) {
    const std::array<float, 2> query{float(i % width), float(i / width)};
    const auto* nearest = tree.nearest(query, max_radius, accept_any);
    if (nearest == nullptr) {
      // Радиус меньше расстояния до любого сайта: это ошибка параметра, а не «пустая» клетка.
      utils::error{}("originator step '{}': voronoi_label found no site within {} of element {} — raise max_radius",
                     call.step_name, max_radius, i);
    }
    target.set(i, double(nearest->payload));
  }
}

// Соседство из триангуляции Делоне: ребро Делоне между двумя сайтами в точности означает, что их
// области ворони граничат. jcv_delaunay_generate считает только эту связность, без геометрии рёбер.
void tool_voronoi_adjacency(const tool_call& call, const size_t begin, const size_t end) {
  require_position_field(call);

  if (begin != 0) {
    utils::error{}("originator step '{}': voronoi_adjacency builds one whole diagram and needs a range "
                   "starting at 0, got [{}, {})",
                   call.step_name, begin, end);
  }

  const size_t site_count = end > begin ? end - begin : 0;
  if (site_count < 3) {
    utils::error{}("originator step '{}': voronoi_adjacency needs at least 3 sites, got {}",
                   call.step_name, site_count);
  }

  auto offsets = call.output(0).write();
  auto neighbours = call.output(1).write();

  if (offsets.count() < site_count + 1) {
    utils::error{}("originator step '{}': voronoi_adjacency writes {} offsets, but the buffer holds {}",
                   call.step_name, site_count + 1, offsets.count());
  }

  const auto positions = call.input(0).read();
  std::vector<jcv_point> points(site_count);
  for (size_t i = 0; i < site_count; ++i) {
    points[i].x = jcv_real(positions.get(i, 0));
    points[i].y = jcv_real(positions.get(i, 1));
  }

  jcv_rect bounds{};
  const bool has_bounds = call.params->has("width");
  if (has_bounds) {
    bounds.min.x = 0;
    bounds.min.y = 0;
    bounds.max.x = jcv_real(call.params->number("width", 1.0));
    bounds.max.y = jcv_real(call.params->number("height", call.params->number("width", 1.0)));
  }

  jcv_diagram diagram{};
  jcv_delaunay_generate(int(site_count), points.data(), has_bounds ? &bounds : nullptr, nullptr, &diagram);

  // Сначала степени вершин, потом префиксная сумма, потом заполнение — та же схема, что у group_by,
  // потому что задача та же: разложить переменное число элементов по группам без гонок.
  std::vector<uint32_t> degree(site_count, 0);
  std::vector<std::pair<uint32_t, uint32_t>> arcs;
  arcs.reserve(size_t(jcv_delaunay_get_edge_count(&diagram)) * 2);

  jcv_delaunay_iter iter{};
  jcv_delaunay_begin(&diagram, &iter);
  jcv_delaunay_edge edge{};
  while (jcv_delaunay_next(&iter, &edge) != 0) {
    if (edge.sites[0] == nullptr || edge.sites[1] == nullptr) {
      continue;
    }
    const auto left = uint32_t(edge.sites[0]->index);
    const auto right = uint32_t(edge.sites[1]->index);
    if (left >= site_count || right >= site_count || left == right) {
      continue;
    }
    // Соседство неориентированное, поэтому в CSR оно кладётся с обеих сторон.
    arcs.emplace_back(left, right);
    arcs.emplace_back(right, left);
    degree[left] += 1;
    degree[right] += 1;
  }

  jcv_diagram_free(&diagram);

  std::vector<size_t> start(site_count + 1, 0);
  for (size_t i = 0; i < site_count; ++i) {
    start[i + 1] = start[i] + degree[i];
  }

  const size_t total = start[site_count];
  if (neighbours.count() < total) {
    utils::error{}("originator step '{}': voronoi_adjacency produced {} arcs, but the neighbours buffer holds {} — "
                   "raise its declared size",
                   call.step_name, total, neighbours.count());
  }

  std::vector<uint32_t> flat(total, 0);
  std::vector<size_t> cursor(start.begin(), start.end() - 1);
  for (const auto& [from, to] : arcs) {
    flat[cursor[from]] = to;
    cursor[from] += 1;
  }

  // Канонизация: список соседей каждого сайта отсортирован и без повторов, поэтому результат не
  // зависит от того, в каком порядке диаграмма отдала рёбра.
  size_t written = 0;
  for (size_t site = 0; site < site_count; ++site) {
    auto* first = flat.data() + start[site];
    auto* last = flat.data() + start[site + 1];
    std::sort(first, last);
    last = std::unique(first, last);

    offsets.set(site, double(written));
    for (auto* it = first; it != last; ++it) {
      neighbours.set(written, double(*it));
      ++written;
    }
  }
  offsets.set(site_count, double(written));
}

} // namespace

void add_voronoi_tools(tool_registry& registry) {
  registry.add(tool_description{.name = "voronoi_label", .shape = aperture::gather,
                                .input_count = 1, .output_count = 1,
                                .body = tool_voronoi_label, .prepare = prepare_site_tree});
  // scatter, а не sequential: инструмент пишет по ВЫЧИСЛЕННЫМ индексам в структуру другого размера
  // (CSR из смещений и дуг), и детерминизм ему даёт собственная схема фаз с канонизацией, а не
  // порядок обхода. Апертура scatter заодно означает, что диапазон относится ко ВХОДАМ — то есть к
  // числу сайтов, а не к размеру выходных буферов.
  registry.add(tool_description{.name = "voronoi_adjacency", .shape = aperture::scatter,
                                .input_count = 1, .output_count = 2,
                                .body = tool_voronoi_adjacency});
}

void add_all_primitives(tool_registry& registry) {
  add_noise_tools(registry);
  add_voronoi_tools(registry);
}

} // namespace originator
} // namespace devils_engine
