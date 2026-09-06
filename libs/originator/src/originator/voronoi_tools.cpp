#include <algorithm>
#include <array>
#include <cmath>
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
//   voronoi_polygons  "какой у области контур" — планарная сетка с ОБЩИМИ вершинами;
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

// ВРЕМЕННАЯ СТОИМОСТЬ ДИАГРАММЫ. Точки и таблицы CSR считаются точно, а сама диаграмма jc_voronoi —
// ОЦЕНКОЙ: её память принадлежит чужой библиотеке, и точное число знает только она. Оценка взята по
// её же документации — рёбер у диаграммы Вороного не больше `3n`, а узел ребра там держит две точки и
// указатели, — и она ВЕРХНЯЯ: занижать стоимость памяти нельзя, а признаться в оценке можно.
size_t diagram_footprint(const tool_call& call) {
  const size_t sites = call.range_count();
  const size_t points = sites * sizeof(jcv_point);
  const size_t csr = (sites + 1) * sizeof(size_t) + sites * sizeof(uint32_t) * 8;
  // ~3n рёбер, у каждого узел с двумя точками и связями; множитель взят с запасом.
  const size_t diagram = sites * 3 * (sizeof(jcv_point) * 2 + sizeof(void*) * 4);
  return points + csr + diagram;
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

  // Растр разметки — это форма ПРИЁМНИКА: именно в него пишется метка на клетку.
  const auto shape = resolve_extent(call, call.output(0), "width", "height");
  const size_t width = shape.x;
  const size_t height = shape.y;
  const float max_radius = float(call.params->number("max_radius", double(width + height)));
  (void)height; // участвует только в радиусе по умолчанию: разметка адресуется линейным индексом

  const auto accept_any = [](const uint32_t&) { return true; };

  // БЛИЗОСТЬ К ГРАНИЦЕ — необязательный второй выход, и он не «ещё одно поле заодно»: у самой границы
  // расстояния до двух ближайших сайтов равны, поэтому разность `d2 - d1` и есть расстояние до
  // границы области. Считается она ТЕМ ЖЕ обходом (второй запрос к тому же дереву, отвергающий
  // победителя), поэтому отдельный проход по растру не нужен.
  //
  // Именно поэтому выход необязательный: тому, кому нужна только разметка, незачем объявлять буфер
  // ради результата, который никто не прочитает.
  const bool want_edge = call.has_output(1);
  auto edge = want_edge ? call.output(1).write() : field_accessor{};

  for (size_t i = begin; i < end; ++i) {
    const std::array<float, 2> query{float(i % width), float(i / width)};
    const auto* nearest = tree.nearest(query, max_radius, accept_any);
    if (nearest == nullptr) {
      // Радиус меньше расстояния до любого сайта: это ошибка параметра, а не «пустая» клетка.
      utils::error{}("originator step '{}': voronoi_label found no site within {} of element {} — raise max_radius",
                     call.step_name, max_radius, i);
    }
    target.set(i, double(nearest->payload));

    if (!want_edge) {
      continue;
    }

    const auto winner = nearest->payload;
    const auto* second = tree.nearest(query, max_radius, [&](const uint32_t& id) { return id != winner; });
    if (second == nullptr) {
      // Сайт всего один: границ нет, и близость к ней не определена. Ноль здесь честнее любого
      // большого числа — «границы рядом нет» и «границы нет вовсе» это разные вещи только там, где
      // области больше одной.
      edge.set(i, 0.0);
      continue;
    }

    const auto distance = [&](const std::array<float, 2>& point) {
      const double dx = double(point[0]) - double(query[0]);
      const double dy = double(point[1]) - double(query[1]);
      return std::sqrt(dx * dx + dy * dy);
    };
    edge.set(i, distance(second->pos) - distance(nearest->pos));
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

// Полигоны областей как ПЛАНАРНАЯ СЕТКА С ОБЩИМИ ВЕРШИНАМИ, а не как набор независимых колец.
//
// jc_voronoi отдаёт таблицу уникальных вершин и индексы концов на каждом ребре, поэтому соседние
// области ссылаются на ОДНУ вершину, а не на две совпадающие копии. Для полигонального мира это
// принципиально: копии рано или поздно разъезжаются при любом преобразовании координат, и на стыке
// областей появляется щель, которой нет в данных.
//
// Выходы — тот же CSR, что у соседства, плюс таблица вершин:
//   0  offsets       начало кольца каждой области, offsets[site_count] = всего углов;
//   1  corners       индексы вершин в порядке обхода против часовой;
//   2  vertices      уникальные позиции вершин (v2);
//   3  vertex_count  один элемент: сколько вершин реально записано.
//
// Четвёртый выход существует потому, что число углов видно из offsets, а число вершин — нет, и
// молчаливая договорённость «читай до первого нуля» была бы хуже явного счётчика.
void tool_voronoi_polygons(const tool_call& call, const size_t begin, const size_t end) {
  require_position_field(call);

  if (begin != 0) {
    utils::error{}("originator step '{}': voronoi_polygons builds one whole diagram and needs a range "
                   "starting at 0, got [{}, {})",
                   call.step_name, begin, end);
  }

  const size_t site_count = end > begin ? end - begin : 0;
  if (site_count < 3) {
    utils::error{}("originator step '{}': voronoi_polygons needs at least 3 sites, got {}",
                   call.step_name, site_count);
  }

  auto offsets = call.output(0).write();
  auto corners = call.output(1).write();
  auto vertices = call.output(2).write();
  auto vertex_count_out = call.output(3).write();

  if (offsets.count() < site_count + 1) {
    utils::error{}("originator step '{}': voronoi_polygons writes {} offsets, but the buffer holds {}",
                   call.step_name, site_count + 1, offsets.count());
  }
  if (vertices.type().components < 2) {
    utils::error{}("originator step '{}': voronoi_polygons needs a 2-component vertex field, '{}.{}' has {}",
                   call.step_name, call.output(2).buffer_name(), call.output(2).field_name(),
                   vertices.type().components);
  }
  if (vertex_count_out.count() < 1) {
    utils::error{}("originator step '{}': voronoi_polygons needs a one-element field for the vertex count",
                   call.step_name);
  }

  const auto positions = call.input(0).read();
  std::vector<jcv_point> points(site_count);
  for (size_t i = 0; i < site_count; ++i) {
    points[i].x = jcv_real(positions.get(i, 0));
    points[i].y = jcv_real(positions.get(i, 1));
  }

  // Границы обязательны: без них полигоны краевых областей уходят в бесконечность, и замощения
  // конечной области не получается.
  const auto width = call.params->number("width", 0.0);
  if (width <= 0.0) {
    utils::error{}("originator step '{}': voronoi_polygons needs a positive 'width' — boundary cells have no "
                   "finite polygon without a clipping rectangle",
                   call.step_name);
  }
  jcv_rect bounds{};
  bounds.min.x = 0;
  bounds.min.y = 0;
  bounds.max.x = jcv_real(width);
  bounds.max.y = jcv_real(call.params->number("height", width));

  jcv_diagram diagram{};
  jcv_diagram_generate(int(site_count), points.data(), &bounds, nullptr, &diagram);

  const int unique_vertices = jcv_get_num_vertices(&diagram);
  if (unique_vertices < 0 || size_t(unique_vertices) > vertices.count()) {
    const size_t needed = unique_vertices < 0 ? 0 : size_t(unique_vertices);
    jcv_diagram_free(&diagram);
    utils::error{}("originator step '{}': voronoi_polygons produced {} unique vertices, but the buffer holds {} — "
                   "raise its declared size",
                   call.step_name, needed, vertices.count());
  }

  std::vector<jcv_point> table(unique_vertices > 0 ? size_t(unique_vertices) : 1);
  jcv_diagram_get_vertices(&diagram, table.data());

  // Кольца собираются по входному индексу сайта: часть точек диаграмма могла отбросить (дубликаты
  // или выход за границы), и такая область честно остаётся с пустым кольцом.
  std::vector<std::vector<uint32_t>> rings(site_count);
  const jcv_site* sites = jcv_diagram_get_sites(&diagram);

  for (int i = 0; i < diagram.numsites; ++i) {
    const jcv_site* site = &sites[i];
    if (site->index >= site_count) {
      continue;
    }

    jcv_edge_iter iter{};
    jcv_site_get_edges(&diagram, site, &iter);
    jcv_edge edge{};

    auto& ring = rings[site->index];
    while (jcv_edge_next(&iter, &edge) != 0) {
      const int corner = edge.vertices[0];
      if (corner < 0 || corner >= unique_vertices) {
        continue;
      }
      ring.push_back(uint32_t(corner));
    }

    // Канонизация: кольцо поворачивается так, чтобы начинаться с минимального индекса вершины.
    // Поворот замкнутого обхода ничего не меняет геометрически, но делает результат сравнимым между
    // запусками без оговорок про то, с какого ребра диаграмма начала обход.
    if (ring.size() > 1) {
      const auto smallest = std::min_element(ring.begin(), ring.end());
      std::rotate(ring.begin(), smallest, ring.end());
    }
  }

  jcv_diagram_free(&diagram);

  size_t total_corners = 0;
  for (const auto& ring : rings) {
    total_corners += ring.size();
  }
  if (total_corners > corners.count()) {
    utils::error{}("originator step '{}': voronoi_polygons produced {} corners, but the buffer holds {} — "
                   "raise its declared size",
                   call.step_name, total_corners, corners.count());
  }

  size_t written = 0;
  for (size_t site = 0; site < site_count; ++site) {
    offsets.set(site, double(written));
    for (const uint32_t corner : rings[site]) {
      corners.set(written, double(corner));
      ++written;
    }
  }
  offsets.set(site_count, double(written));

  for (size_t i = 0; i < size_t(unique_vertices); ++i) {
    vertices.set(i, double(table[i].x), 0);
    vertices.set(i, double(table[i].y), 1);
  }
  vertex_count_out.set(0, double(unique_vertices));
}

} // namespace

void add_voronoi_tools(tool_registry& registry) {
  registry.add(tool_description{
    .name = "voronoi_label", .shape = aperture::gather,
    .input_count = 1, .output_count = 2, .optional_outputs = 1,
    .body = tool_voronoi_label, .prepare = prepare_site_tree,
    // Подготовка строит kd-дерево сайтов: точка на сайт плюс узлы дерева. Сайтов на порядки меньше,
    // чем клеток, поэтому величина маленькая — но НАЗВАННАЯ, а не подразумеваемая.
    .footprint = [](const tool_call& call) {
      const size_t sites = call.input(0).valid() ? call.input(0).count() : 0;
      return sites * (sizeof(float) * 2 + sizeof(uint32_t) * 4);
    },
    // УСТРОЙСТВЕННАЯ ФОРМА — ПЕРЕБОР, а не дерево, и это не лень. kd-дерево на устройство не
    // переносится (его строит подготовка на хосте), а перебор по списку сайтов там стоит ровно
    // столько, сколько сайтов: работа на элемент плотная, а именно плотность и окупает устройство.
    //
    // `max_radius` устройственная форма НЕ читает, и это названо: на хосте он ограничивает ПОИСК по
    // дереву, а перебор ничего не ищет — он смотрит все сайты. Разойтись два пути могут только на
    // радиусе, меньшем расстояния до ближайшего сайта, то есть там, где путь на CPU уже падает.
    .device_body = "  uint width = args.extent_x;\n"
                   "  vec2 query = vec2(float(index % width), float(index / width));\n"
                   "  float nearest = 3.4e38;\n"
                   "  float second = 3.4e38;\n"
                   "  uint winner = 0u;\n"
                   "  uint site_count = in_0_length();\n"
                   "  for (uint i = 0u; i < site_count; ++i) {\n"
                   "    vec2 delta = vec2(in_0_at(i, 0u), in_0_at(i, 1u)) - query;\n"
                   "    float squared = dot(delta, delta);\n"
                   "    if (squared < nearest) {\n"
                   "      second = nearest;\n"
                   "      nearest = squared;\n"
                   "      winner = i;\n"
                   "    } else if (squared < second) {\n"
                   "      second = squared;\n"
                   "    }\n"
                   "  }\n"
                   "  out_0_set(index, float(winner));\n"
                   "#if ORIGINATOR_OUTPUTS > 1\n"
                   "  out_1_set(index, second > 3.3e38 ? 0.0 : sqrt(second) - sqrt(nearest));\n"
                   "#endif\n",
    .device_params = {},
    // МЕТКА ЦЕЛАЯ ПО ПРИРОДЕ: номер области — это номер, а не число, и приёмник у неё обычно `ui1`.
    // Поэтому инструмент объявляет себя годным для любого рода: тело пишет `float(winner)`, а
    // переводящая перегрузка аксессора кладёт в целое поле значение, а не биты. Точности `float32`
    // хватает с запасом — областей столько, сколько их перечислил хост, и до 2^24 отсюда далеко.
    .device_integer_ready = true});
  // scatter, а не sequential: инструмент пишет по ВЫЧИСЛЕННЫМ индексам в структуру другого размера
  // (CSR из смещений и дуг), и детерминизм ему даёт собственная схема фаз с канонизацией, а не
  // порядок обхода. Апертура scatter заодно означает, что диапазон относится ко ВХОДАМ — то есть к
  // числу сайтов, а не к размеру выходных буферов.
  registry.add(tool_description{.name = "voronoi_adjacency", .shape = aperture::scatter,
                                .input_count = 1, .output_count = 2,
                                .body = tool_voronoi_adjacency, .footprint = diagram_footprint});
  registry.add(tool_description{.name = "voronoi_polygons", .shape = aperture::scatter,
                                .input_count = 1, .output_count = 4,
                                .body = tool_voronoi_polygons, .footprint = diagram_footprint});
}

void add_all_primitives(tool_registry& registry) {
  add_noise_tools(registry);
  add_voronoi_tools(registry);
}

} // namespace originator
} // namespace devils_engine
