#include "devils_engine/originator/neighbourhood.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>

#include "devils_engine/utils/core.h"

// Соседство как привязка: разрешение формы соседства у вызова, ядра понятий и общие инструменты.
//
//   blur      входы: значение, [смещения, цели, [веса]]; выход: среднее. Без CSR — трафарет над
//             формой выхода (`stencil` = cross | box, `radius`, `wrap`). Апертура gather.
//   frontier  входы: метка, [смещения, цели]; выход: 1 на границе меток. gather.
//   slope     входы: значение, [смещения, цели]; выход: средний модуль разности со соседями. gather.
//   vote      входы: метка, вес, маска, [смещения, цели]; выход: метка (голос соседей). gather.
//   flood     входы: затравка, стоимость, проходимость, [смещения, цели, [веса]]; выходы: метка,
//             расстояние. sequential — результат и есть порядок присвоения.
//   edge_csr  входы: позиция узла, конец a, конец b; выходы: смещения, цели, [веса = длины рёбер].
//             Таблица соседства из СПИСКА рёбер (скелет пещеры, дороги): тот же граф, что видят
//             skeleton_*, но теперь им может пользоваться любое ядро выше. sequential.

namespace devils_engine {
namespace originator {

neighbourhood resolve_neighbourhood(const tool_call& call, const size_t offsets_input, const field_ref& shaped,
                                    const size_t count) {
  if (call.has_input(offsets_input) != call.has_input(offsets_input + 1)) {
    utils::error{}("originator step '{}': {} got CSR offsets without targets or the other way round", call.step_name,
                   call.tool_name);
  }
  if (call.has_input(offsets_input)) {
    csr_neighbourhood csr;
    csr.offsets = call.input(offsets_input).read();
    csr.arcs = call.input(offsets_input + 1).read();
    if (call.has_input(offsets_input + 2)) csr.weights = call.input(offsets_input + 2).read();
    csr.count = count;
    if (csr.offsets.count() < count + 1) {
      utils::error{}("originator step '{}': {} got {} CSR offsets for {} elements (needs one more)", call.step_name,
                     call.tool_name, csr.offsets.count(), count);
    }
    return csr;
  }
  stencil_neighbourhood stencil;
  stencil.extent = shaped.extent();
  if (!stencil.extent.declared()) {
    utils::error{}("originator step '{}': {} has no CSR and buffer '{}' declares no extent, so there are no "
                   "neighbours to walk", call.step_name, call.tool_name, shaped.buffer_name());
  }
  const std::string_view form = call.params->string("stencil", "cross");
  if (form == "box") {
    stencil.form = stencil_neighbourhood::shape::box;
  } else if (form != "cross") {
    utils::error{}("originator step '{}': {} stencil is 'cross' or 'box', got '{}'", call.step_name, call.tool_name,
                   form);
  }
  stencil.radius = size_t(std::clamp<int64_t>(call.params->integer("radius", 1), 1, 64));
  stencil.wrap = call.params->number("wrap", 0.0) != 0.0;
  return stencil;
}

void box_mean_range(const const_field_accessor& source, const field_accessor& target, const size_t width,
                    const size_t height, const size_t radius, const size_t begin, const size_t end) {
  const size_t count = source.count();
  for (size_t i = begin; i < end; ++i) {
    const size_t x = i % width;
    const size_t y = i / width;
    double sum = 0.0;
    size_t taken = 0;
    const size_t x0 = x >= radius ? x - radius : 0;
    const size_t x1 = std::min(x + radius, width - 1);
    const size_t y0 = y >= radius ? y - radius : 0;
    const size_t y1 = height == 0 ? y : std::min(y + radius, height - 1);
    for (size_t sy = y0; sy <= y1; ++sy) {
      for (size_t sx = x0; sx <= x1; ++sx) {
        const size_t index = sy * width + sx;
        if (index >= count) continue;
        sum += source.get(index);
        ++taken;
      }
    }
    target.set(i, taken == 0 ? 0.0 : sum / double(taken));
  }
}

void blur_range(const neighbourhood& around, const const_field_accessor& values, const field_accessor& target,
                const size_t begin, const size_t end, const double self_weight, const double neighbour_weight) {
  if (const auto* stencil = std::get_if<stencil_neighbourhood>(&around);
      stencil != nullptr && stencil->form == stencil_neighbourhood::shape::box && !stencil->wrap &&
      stencil->extent.z <= 1 && self_weight == neighbour_weight && self_weight > 0.0) {
    box_mean_range(values, target, stencil->extent.x, stencil->extent.y, stencil->radius, begin, end);
    return;
  }
  std::visit([&](const auto& form) {
    for (size_t i = begin; i < end; ++i) {
      double sum = self_weight * values.get(i);
      double weight = self_weight;
      form.each(i, [&](const size_t other, const double arc) {
        const double w = neighbour_weight * arc;
        sum += w * values.get(other);
        weight += w;
      });
      target.set(i, weight > 0.0 ? sum / weight : values.get(i));
    }
  }, around);
}

void frontier_range(const neighbourhood& around, const const_field_accessor& labels, const field_accessor& target,
                    const size_t begin, const size_t end, const double ignored) {
  std::visit([&](const auto& form) {
    for (size_t i = begin; i < end; ++i) {
      const double own = labels.get(i);
      double frontier = 0.0;
      if (own != ignored) {
        bool found = false;
        form.each(i, [&](const size_t other, const double) {
          if (found) return;
          const double label = labels.get(other);
          if (label != own && label != ignored) found = true;
        });
        frontier = found ? 1.0 : 0.0;
      }
      target.set(i, frontier);
    }
  }, around);
}

void slope_range(const neighbourhood& around, const const_field_accessor& values, const field_accessor& target,
                 const size_t begin, const size_t end) {
  std::visit([&](const auto& form) {
    for (size_t i = begin; i < end; ++i) {
      const double own = values.get(i);
      double total = 0.0;
      size_t taken = 0;
      form.each(i, [&](const size_t other, const double) {
        total += std::abs(values.get(other) - own);
        ++taken;
      });
      target.set(i, taken == 0 ? 0.0 : total / double(taken));
    }
  }, around);
}

void vote_range(const neighbourhood& around, const const_field_accessor& labels, const const_field_accessor& weights,
                const const_field_accessor& mask, const field_accessor& target, const size_t begin, const size_t end,
                const double threshold) {
  std::visit([&](const auto& form) {
    for (size_t i = begin; i < end; ++i) {
      const double own = labels.get(i);
      if (own != 0.0 || mask.get(i) == 0.0) {
        target.set(i, own);
        continue;
      }
      double best_label = 0.0;
      double best_weight = threshold;
      form.each(i, [&](const size_t other, const double) {
        const double other_label = labels.get(other);
        if (other_label == 0.0) return;
        const double weight = weights.get(other);
        if (weight > best_weight || (weight == best_weight && best_label != 0.0 && other_label < best_label)) {
          best_weight = weight;
          best_label = other_label;
        }
      });
      target.set(i, best_label);
    }
  }, around);
}

void flood_whole(const neighbourhood& around, const const_field_accessor& seeds, const const_field_accessor& costs,
                 const const_field_accessor& passable, const field_accessor& label_out,
                 const field_accessor& distance_out, const size_t count, const double unreached,
                 const size_t capacity, const std::string_view& step_name) {
  struct entry {
    double distance = 0.0;
    uint32_t label = 0;
    uint32_t cell = 0;
    // priority_queue отдаёт МАКСИМУМ, поэтому сравнение перевёрнуто: ближе — «больше».
    bool operator<(const entry& other) const noexcept {
      if (distance != other.distance) return distance > other.distance;
      if (label != other.label) return label > other.label;
      return cell > other.cell;
    }
  };

  std::priority_queue<entry> queue;
  std::vector<uint32_t> assigned(count, 0);
  std::vector<uint32_t> used;
  const bool open_everywhere = !passable.valid();

  for (size_t i = 0; i < count; ++i) {
    label_out.set(i, 0.0);
    distance_out.set(i, unreached);
    const auto seed = uint32_t(seeds.get(i));
    if (seed != 0 && (open_everywhere || passable.get(i) != 0.0)) {
      queue.push(entry{0.0, seed, uint32_t(i)});
      if (used.size() <= seed) used.resize(seed + 1, 0);
    }
  }

  std::visit([&](const auto& form) {
    const bool weighted = form.weighted();
    while (!queue.empty()) {
      const entry current = queue.top();
      queue.pop();
      if (assigned[current.cell] != 0) continue;
      if (used.size() <= current.label) used.resize(size_t(current.label) + 1, 0);
      // Метка, выбравшая свой лимит, дальше не растёт — но клетка при этом НЕ занимается: её ещё может
      // забрать другая метка, у которой лимит остался.
      if (capacity != 0 && used[current.label] >= capacity) continue;

      assigned[current.cell] = current.label;
      used[current.label] += 1;
      label_out.set(current.cell, double(current.label));
      distance_out.set(current.cell, current.distance);

      form.each(current.cell, [&](const size_t other, const double arc) {
        if (other >= count || assigned[other] != 0 || (!open_everywhere && passable.get(other) == 0.0)) return;
        double step = costs.valid() ? costs.get(other) : 0.0;
        if (weighted) step += arc;
        if (step < 0.0) {
          // Отрицательная стоимость превращает заливку в бесконечный цикл: это ошибка данных.
          utils::error{}("originator step '{}': flood got a negative step {} into element {}", step_name, step, other);
        }
        queue.push(entry{current.distance + step, current.label, uint32_t(other)});
      });
    }
  }, around);
}

namespace {

void tool_blur(const tool_call& call, const size_t begin, const size_t end) {
  const auto around = resolve_neighbourhood(call, 1, call.output(0), call.input(0).count());
  blur_range(around, call.input(0).read(), call.output(0).write(), begin, end,
             call.params->number("self_weight", 1.0), call.params->number("neighbour_weight", 1.0));
}

void tool_frontier(const tool_call& call, const size_t begin, const size_t end) {
  const auto around = resolve_neighbourhood(call, 1, call.output(0), call.input(0).count());
  frontier_range(around, call.input(0).read(), call.output(0).write(), begin, end, call.params->number("ignore", -1.0));
}

void tool_slope(const tool_call& call, const size_t begin, const size_t end) {
  const auto around = resolve_neighbourhood(call, 1, call.output(0), call.input(0).count());
  slope_range(around, call.input(0).read(), call.output(0).write(), begin, end);
}

void tool_vote(const tool_call& call, const size_t begin, const size_t end) {
  const auto around = resolve_neighbourhood(call, 3, call.output(0), call.input(0).count());
  vote_range(around, call.input(0).read(), call.input(1).read(), call.input(2).read(), call.output(0).write(), begin,
             end, call.params->number("threshold", 0.0));
}

void tool_flood(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': flood walks the WHOLE neighbourhood, its range must start at 0, got [{}, {})",
                   call.step_name, begin, end);
  }
  const size_t count = end;
  const auto around = resolve_neighbourhood(call, 3, call.output(0), count);
  flood_whole(around, call.input(0).read(), call.input(1).read(), call.input(2).read(), call.output(0).write(),
              call.output(1).write(), count, call.params->number("unreached", -1.0),
              size_t(std::max<int64_t>(call.params->integer("capacity", 0), 0)), call.step_name);
}

void tool_edge_csr(const tool_call& call, const size_t, const size_t) {
  const auto positions = call.input(0).read();
  const auto edge_a = call.input(1).read(), edge_b = call.input(2).read();
  auto offsets = call.output(0).write();
  auto arcs = call.output(1).write();
  const bool weighted = call.has_output(2);
  const auto nodes = size_t(std::max<int64_t>(call.params->integer("nodes", int64_t(positions.count())), 0));
  const auto edges = size_t(std::max<int64_t>(call.params->integer("edges", int64_t(edge_a.count())), 0));
  if (nodes > positions.count() || edges > edge_a.count() || edges > edge_b.count() || offsets.count() < nodes + 1 ||
      arcs.count() < 2 * edges || (weighted && call.output(2).write().count() < 2 * edges)) {
    utils::error{}("originator step '{}': edge_csr of {} nodes and {} edges does not fit its buffers", call.step_name,
                   nodes, edges);
  }
  std::vector<size_t> degree(nodes + 1, 0);
  for (size_t e = 0; e < edges; ++e) {
    const auto a = size_t(edge_a.get(e)), b = size_t(edge_b.get(e));
    if (a >= nodes || b >= nodes) {
      utils::error{}("originator step '{}': edge_csr edge {} joins {}-{} beyond the {} nodes", call.step_name, e, a, b,
                     nodes);
    }
    ++degree[a + 1];
    ++degree[b + 1];
  }
  for (size_t i = 0; i < nodes; ++i) degree[i + 1] += degree[i];
  for (size_t i = 0; i <= nodes; ++i) offsets.set(i, double(degree[i]));
  std::vector<size_t> fill(degree.begin(), degree.end() - 1);
  auto lengths = weighted ? call.output(2).write() : field_accessor{};
  for (size_t e = 0; e < edges; ++e) {
    const auto a = size_t(edge_a.get(e)), b = size_t(edge_b.get(e));
    double length = 0.0;
    if (weighted) {
      for (uint32_t c = 0; c < std::min<uint32_t>(positions.type().components, 3); ++c) {
        const double d = positions.get(a, c) - positions.get(b, c);
        length += d * d;
      }
      length = std::sqrt(length);
    }
    arcs.set(fill[a], double(b));
    if (weighted) lengths.set(fill[a], length);
    ++fill[a];
    arcs.set(fill[b], double(a));
    if (weighted) lengths.set(fill[b], length);
    ++fill[b];
  }
}

} // namespace

void tool_registry::add_neighbourhood_tools() {
  add(tool_description{.name = "blur", .shape = aperture::gather, .input_count = 4, .optional_inputs = 3,
                       .output_count = 1, .body = tool_blur, .footprint = no_temporary_memory});
  add(tool_description{.name = "frontier", .shape = aperture::gather, .input_count = 3, .optional_inputs = 2,
                       .output_count = 1, .body = tool_frontier, .footprint = no_temporary_memory});
  add(tool_description{.name = "slope", .shape = aperture::gather, .input_count = 3, .optional_inputs = 2,
                       .output_count = 1, .body = tool_slope, .footprint = no_temporary_memory});
  add(tool_description{.name = "vote", .shape = aperture::gather, .input_count = 5, .optional_inputs = 2,
                       .output_count = 1, .body = tool_vote, .footprint = no_temporary_memory});
  add(tool_description{.name = "flood", .shape = aperture::sequential, .input_count = 6, .optional_inputs = 3,
                       .output_count = 2, .body = tool_flood,
                       // Очередь не длиннее числа дуг (у трафарета — элементов на число соседей) плюс отметка
                       // занятости: ВЕРХНЯЯ оценка.
                       .footprint = [](const tool_call& call) {
                         const size_t count = call.range_count();
                         const size_t arcs = call.has_input(4) ? call.input(4).count() : count * 8;
                         return count * sizeof(uint32_t) * 2 + arcs * (sizeof(uint32_t) * 2 + sizeof(double));
                       }});
  add(tool_description{.name = "edge_csr", .shape = aperture::sequential, .input_count = 3, .output_count = 3,
                       .optional_outputs = 1, .body = tool_edge_csr,
                       .footprint = [](const tool_call& call) { return call.input(0).count() * sizeof(size_t) * 2; }});
}

} // namespace originator
} // namespace devils_engine
