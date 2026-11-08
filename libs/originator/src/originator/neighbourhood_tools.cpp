#include "neighbourhood_device.h"
#include "devils_engine/originator/neighbourhood.h"
#include "devils_engine/originator/graph_walk.h"

#include <algorithm>
#include <array>
#include <type_traits>
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
                                    const size_t count, const bool weighted) {
  if (call.has_input(offsets_input) != call.has_input(offsets_input + 1)) {
    utils::error{}("originator step '{}': {} got CSR offsets without targets or the other way round", call.step_name,
                   call.tool_name);
  }
  if (call.has_input(offsets_input)) {
    csr_neighbourhood csr;
    csr.offsets = call.input(offsets_input).read();
    csr.arcs = call.input(offsets_input + 1).read();
    if (weighted && call.has_input(offsets_input + 2)) csr.weights = call.input(offsets_input + 2).read();
    csr.count = count;
    if (csr.offsets.count() <= call.range_end) {
      utils::error{}("originator step '{}': {} got {} CSR offsets for range ending at {} (needs one more)", call.step_name,
                     call.tool_name, csr.offsets.count(), call.range_end);
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

size_t components_whole(const neighbourhood& around, const const_field_accessor& mask,
                        const const_field_accessor& existing, const field_accessor& target,
                        const size_t count, const size_t minimum, const uint32_t label_base) {
  if (count > UINT32_MAX) utils::error{}("originator components: node count exceeds uint32");
  std::vector<uint32_t> raw(count, 0), stack;
  std::vector<size_t> sizes;
  stack.reserve(count);
  sizes.reserve(count);
  std::visit([&](const auto& form) {
    for (size_t root = 0; root < count; ++root) {
      if (raw[root] || mask.get(root) == 0 || (existing.valid() && existing.get(root) != 0)) continue;
      const auto label = uint32_t(sizes.size() + 1);
      sizes.push_back(0);
      walk_component(uint32_t(root), [&](size_t node, auto visit) { form.each(node, visit); },
        [&](size_t node) {
          if (node >= count || raw[node] || mask.get(node) == 0 || (existing.valid() && existing.get(node) != 0)) return false;
          raw[node] = label;
          ++sizes.back();
          return true;
        }, stack);
    }
  }, around);
  std::vector<uint32_t> renumber(sizes.size() + 1, 0);
  size_t kept = 0;
  for (size_t i = 0; i < sizes.size(); ++i) {
    if (sizes[i] < minimum) continue;
    if (uint64_t(label_base) + ++kept > UINT32_MAX) utils::error{}("originator components: label domain exceeds uint32");
    renumber[i + 1] = uint32_t(label_base + kept);
  }
  for (size_t node = 0; node < count; ++node) {
    const double before = existing.valid() ? existing.get(node) : 0.0;
    target.set(node, before != 0 ? before : double(renumber[raw[node]]));
  }
  return kept;
}

size_t neighbourhood_arc_bound(const neighbourhood& around, const size_t count) {
  return std::visit([&](const auto& form) -> size_t {
    using type = std::decay_t<decltype(form)>;
    if constexpr (std::is_same_v<type, csr_neighbourhood>) return form.arcs.count();
    else {
      const size_t dimensions = 1 + size_t(form.extent.y > 1) + size_t(form.extent.z > 1);
      size_t neighbours = 0;
      if (form.form == stencil_neighbourhood::shape::cross) neighbours = dimensions * 2 * form.radius;
      else {
        neighbours = 1;
        for (size_t axis = 0; axis < dimensions; ++axis)
          neighbours = walk_size_multiply(neighbours, 2 * form.radius + 1, "stencil");
        --neighbours;
      }
      return walk_size_multiply(count, neighbours, "stencil");
    }
  }, around);
}

size_t flood_footprint(const neighbourhood& around, const size_t count) {
  return walk_footprint(count, neighbourhood_arc_bound(around, count));
}

void flood_whole(const neighbourhood& around, const const_field_accessor& seeds, const const_field_accessor& costs,
                 const const_field_accessor& passable, const field_accessor& label_out,
                 const field_accessor& distance_out, const size_t count, const double unreached,
                 const size_t capacity, const std::string_view& step_name) {
  std::vector<walk_seed> sources;
  sources.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    label_out.set(i, 0.0);
    distance_out.set(i, unreached);
    const double raw = seeds.get(i);
    if (!std::isfinite(raw) || raw < 0 || raw > double(UINT32_MAX) || std::floor(raw) != raw)
      utils::error{}("originator step '{}': flood label {} at {} is not uint32", step_name, raw, i);
    const auto label = uint32_t(raw);
    if (label) sources.push_back({uint32_t(i), label});
  }
  walk_workspace workspace;
  std::visit([&](const auto& form) {
    const size_t arcs = neighbourhood_arc_bound(around, count);
    const auto each = [&](const size_t node, auto visit) {
      form.each(node, [&](const size_t other, const double arc) {
        double step = costs.valid() ? costs.get(other) : 0.0;
        if (form.weighted()) step += arc;
        visit(other, step);
      });
    };
    walk_distances(count, arcs, sources, each,
      [&](const size_t node) { return !passable.valid() || passable.get(node) != 0.0; },
      [&](const size_t node, const uint32_t label, const double distance, const uint32_t) {
        label_out.set(node, double(label));
        distance_out.set(node, distance);
      }, capacity, workspace, step_name);
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

void tool_components(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) utils::error{}("originator step '{}': components walks the whole neighbourhood", call.step_name);
  const auto around = resolve_neighbourhood(call, 1, call.output(0), end, false);
  const auto base = call.params->number("label_base", 0);
  if (!std::isfinite(base) || base < 0 || base > double(UINT32_MAX) || std::floor(base) != base)
    utils::error{}("originator step '{}': components label_base {} is not uint32", call.step_name, base);
  const auto count = components_whole(around, call.input(0).read(),
    call.has_input(3) ? call.input(3).read() : const_field_accessor{}, call.output(0).write(), end,
    size_t(std::max<int64_t>(0, call.params->integer("min_size", 0))), uint32_t(base));
  if (call.has_output(1)) call.output(1).write().set(0, double(count));
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
  auto lengths = weighted ? call.output(2).write() : field_accessor{};
  edge_workspace scratch;
  build_edge_adjacency(nodes, edges,
    [&](size_t e) { return std::array{size_t(edge_a.get(e)), size_t(edge_b.get(e))}; },
    [&](size_t, size_t a, size_t b) {
      double length = 0.0;
      if (weighted) {
        for (uint32_t c = 0; c < std::min<uint32_t>(positions.type().components, 3); ++c) {
          const double d = positions.get(a, c) - positions.get(b, c);
          length += d * d;
        }
      }
      return std::sqrt(length);
    }, [](size_t) { return true; },
    [&](size_t node, size_t offset) { offsets.set(node, double(offset)); },
    [](size_t) {},
    [&](size_t index, size_t node, double length) {
      arcs.set(index, double(node));
      if (weighted) lengths.set(index, length);
    }, scratch, call.step_name);

}

} // namespace

void tool_registry::add_neighbourhood_tools() {
  add(tool_description{.name = "blur", .shape = aperture::gather, .input_count = 4, .optional_inputs = 3,
                       .output_count = 1, .body = tool_blur, .footprint = no_temporary_memory,
                       .device_body = detail::csr_device_body("blur",0,1,2,3,-1,true), .device_params = {{"self_weight",1.0},{"neighbour_weight",1.0}},
                       .device_min_inputs = 3, .device_integer_ready = true});
  add(tool_description{.name = "frontier", .shape = aperture::gather, .input_count = 3, .optional_inputs = 2,
                       .output_count = 1, .body = tool_frontier, .footprint = no_temporary_memory,
                       .device_body = detail::csr_device_body("frontier",0,1,2), .device_params = {{"ignore",-1.0}},
                       .device_min_inputs = 3, .device_integer_ready = true});
  add(tool_description{.name = "slope", .shape = aperture::gather, .input_count = 3, .optional_inputs = 2,
                       .output_count = 1, .body = tool_slope, .footprint = no_temporary_memory,
                       .device_body = detail::csr_device_body("slope",0,1,2), .device_params = {},
                       .device_min_inputs = 3, .device_integer_ready = true});
  add(tool_description{.name = "vote", .shape = aperture::gather, .input_count = 5, .optional_inputs = 2,
                       .output_count = 1, .body = tool_vote, .footprint = no_temporary_memory,
                       .device_body = detail::csr_device_body("vote",0,3,4,1,2), .device_params = {{"threshold",0.0}},
                       .device_min_inputs = 5, .device_integer_ready = true});
  add(tool_description{.name = "flood", .shape = aperture::sequential, .input_count = 6, .optional_inputs = 3,
                       .output_count = 2, .body = tool_flood,
                       .footprint = [](const tool_call& call) {
                         const auto around = resolve_neighbourhood(call, 3, call.output(0), call.range_count());
                         return flood_footprint(around, call.range_count());
                       }});
  add(tool_description{.name = "components", .shape = aperture::sequential, .input_count = 4,
                       .optional_inputs = 3, .output_count = 2, .optional_outputs = 1,
                       .body = tool_components,
                       .footprint = [](const tool_call& call) { return (call.range_count() + 1) * 64; }});
  add(tool_description{.name = "edge_csr", .shape = aperture::sequential, .input_count = 3, .output_count = 3,
                       .optional_outputs = 1, .body = tool_edge_csr,
                       .footprint = [](const tool_call& call) { return (call.input(0).count() * 2 + 1) * sizeof(size_t); }});
}

} // namespace originator
} // namespace devils_engine
