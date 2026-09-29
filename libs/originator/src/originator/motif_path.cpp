#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <vector>

#include "devils_engine/originator/motif_path.h"

// ЕДИНЫЙ КОНТУР ПУТИ. Нормали сегментов пересекаются в общем miter-сечении,
// поэтому соседние части имеют одинаковые вершины, а не два независимо выбранных торца.
// Никакого libm в плане: длина и нормали fixed point, один финальный переход к целым мерам.
// Завершение пути работает по реальным общим отрезкам; обрезка двигает только свободный
// осевой торец. Используемые порты и miter остаются на прежних координатах.

namespace devils_engine::originator {
namespace {

constexpr int64_t scale = 4096;
struct fixed_point {
  int64_t x, y;
};

uint64_t integer_root(const uint64_t value) noexcept {
  uint64_t low = 0, high = 1;
  while (high <= value / high) {
    high *= 2;
  }
  while (low + 1 < high) {
    const auto middle = (low + high) / 2;
    if (middle <= value / middle)
      low = middle;
    else
      high = middle;
  }
  return low;
}

int64_t fixed_length(const int64_t dx, const int64_t dy) noexcept {
  const auto squared = uint64_t(dx * dx + dy * dy);
  auto factor = uint64_t(scale);
  while (squared > UINT64_MAX / factor / factor) {
    factor /= 2;
  }
  return int64_t(integer_root(squared * factor * factor) * (scale / factor));
}

int32_t rounded(const int64_t value) noexcept {
  return int32_t(value < 0 ? -((-value + scale / 2) / scale) : (value + scale / 2) / scale);
}

std::vector<std::vector<motif_path_point>> stroke(const std::vector<motif_path_point>& axis2,
                                                  const uint32_t width) {
  std::vector<fixed_point> normals;
  for (size_t i = 1; i < axis2.size(); ++i) {
    const auto dx = int64_t(axis2[i].x) - axis2[i - 1].x;
    const auto dy = int64_t(axis2[i].y) - axis2[i - 1].y;
    const auto length = fixed_length(dx, dy);
    normals.push_back({-dy * scale * scale / length, dx * scale * scale / length});
  }
  std::vector<motif_path_point> left, right;
  const auto half_width = int64_t(width) * scale / 2;
  for (size_t i = 0; i < axis2.size(); ++i) {
    auto offset = normals[i == 0 ? 0 : i - 1];
    if (i != 0 && i + 1 != axis2.size()) {
      const auto next = normals[i];
      const auto divisor = scale * scale + offset.x * next.x + offset.y * next.y;
      offset = {(offset.x + next.x) * scale * scale / divisor,
                (offset.y + next.y) * scale * scale / divisor};
    }
    const auto x = int64_t(axis2[i].x) * scale / 2;
    const auto y = int64_t(axis2[i].y) * scale / 2;
    const auto ox = offset.x * half_width / scale, oy = offset.y * half_width / scale;
    left.push_back({rounded(x - ox), rounded(y - oy)});
    right.push_back({rounded(x + ox), rounded(y + oy)});
  }
  std::vector<std::vector<motif_path_point>> parts;
  for (size_t i = 1; i < axis2.size(); ++i) {
    parts.push_back({left[i - 1], left[i], right[i], right[i - 1]});
  }
  return parts;
}

bool separated(const std::span<const motif_path_point> edges,
               const std::span<const motif_path_point> a,
               const std::span<const motif_path_point> b) noexcept {
  for (size_t i = 0; i < edges.size(); ++i) {
    const auto p = edges[i], q = edges[(i + 1) % edges.size()];
    const auto nx = int64_t(p.y) - q.y, ny = int64_t(q.x) - p.x;
    if (nx == 0 && ny == 0) continue;
    int64_t a_min = INT64_MAX, b_min = INT64_MAX, a_max = INT64_MIN, b_max = INT64_MIN;
    for (const auto point : a) {
      const auto projection = nx * point.x + ny * point.y;
      a_min = std::min(a_min, projection);
      a_max = std::max(a_max, projection);
    }
    for (const auto point : b) {
      const auto projection = nx * point.x + ny * point.y;
      b_min = std::min(b_min, projection);
      b_max = std::max(b_max, projection);
    }
    if (a_max <= b_min || b_max <= a_min) return true;
  }
  return false;
}

template<class Visitor>
bool visit_shared_boundary(const std::span<const motif_path_point> a,
                          const std::span<const motif_path_point> b, Visitor&& visit) {
  for (size_t i = 0; i < a.size(); ++i) {
    const auto p = a[i];
    const auto q = a[(i + 1) % a.size()];
    const auto dx = int64_t(q.x) - p.x;
    const auto dy = int64_t(q.y) - p.y;
    if (dx == 0 && dy == 0) continue;
    for (size_t j = 0; j < b.size(); ++j) {
      const auto r = b[j];
      const auto s = b[(j + 1) % b.size()];
      if (dx * (int64_t(r.y) - p.y) != dy * (int64_t(r.x) - p.x) ||
          dx * (int64_t(s.y) - p.y) != dy * (int64_t(s.x) - p.x)) continue;
      const auto along = [&](const motif_path_point v) {
        return dx * (int64_t(v.x) - p.x) + dy * (int64_t(v.y) - p.y);
      };
      const auto low = std::max(int64_t(0), std::min(along(r), along(s)));
      const auto high = std::min(dx * dx + dy * dy, std::max(along(r), along(s)));
      if (low >= high) continue;
      motif_boundary_segment segment;
      for (const auto point : {p, q, r, s}) {
        if (along(point) == low) segment.a = point;
        if (along(point) == high) segment.b = point;
      }
      if (visit(segment)) return true;
    }
  }
  return false;
}

} // namespace

motif_path_shape make_motif_path(const motif_path_parameters& p) {
  motif_path_shape result;
  result.width = p.width;
  result.parameters = p;
  if (p.length < 12 || p.length > 4096 || p.width < 3 || p.width > 128 ||
      p.l1_per_mille > 1000 || p.l2_per_mille > 1000 ||
      p.bend < -4096 || p.bend > 4096 || p.mode > motif_path_mode::corner ||
      p.min_bend_length == 0 || p.min_bend_length > 4096) {
    result.refusal = "path: unsupported length, width, fractions or bend";
    return result;
  }
  const auto length = int32_t(p.length), width = int32_t(p.width);
  const auto lead = int32_t(p.length * p.l1_per_mille / 1000);
  const bool straight = p.bend == 0 || p.l1_per_mille == 1000 ||
                        (p.mode == motif_path_mode::offset && p.l1_per_mille + p.l2_per_mille >= 1000);
  if (straight) {
    result.axis2 = {{0, width}, {2 * length, width}};
    result.pieces.push_back({{}, {0, 0, length, width}});
  } else if (p.mode == motif_path_mode::offset) {
    const auto tail = int32_t(p.length * p.l2_per_mille / 1000);
    const auto middle = length - lead - tail;
    if (lead < width || tail < width || middle < int32_t(p.min_bend_length)) {
      result.refusal = "path: offset needs two width-sized straights and the declared minimum bend interval";
      return result;
    }
    result.axis2 = {{0, width}, {2 * lead, width}, {2 * (length - tail), width + 2 * p.bend}, {2 * length, width + 2 * p.bend}};
    result.pieces = {
      {{}, {0, 0, lead, width}},
      {{}, {lead, std::min(0, p.bend), middle, width + std::abs(p.bend)}, motif_east, p.bend > 0 ? motif_attach_align::start : motif_attach_align::end},
      {{}, {lead + middle, p.bend, tail, width}, motif_east, p.bend > 0 ? motif_attach_align::end : motif_attach_align::start}};
  } else {
    const auto tail = length - lead;
    if (lead < width || tail < width) {
      result.refusal = "path: corner needs two width-sized arms";
      return result;
    }
    const auto direction = p.bend > 0 ? 1 : -1;
    result.axis2 = {{0, width}, {2 * lead, width}, {2 * lead, width + 2 * direction * tail}};
    result.pieces = {
      {{}, {0, 0, lead + (width + 1) / 2, width}},
      {{}, {lead - width / 2, direction > 0 ? width : -(tail - width / 2), width, tail - width / 2}, uint8_t(direction > 0 ? motif_south : motif_north), motif_attach_align::end}};
  }
  const auto outlines = stroke(result.axis2, p.width);
  for (size_t i = 0; i < outlines.size(); ++i) {
    result.pieces[i].outline = outlines[i];
    for (size_t edge = 0; edge < outlines[i].size(); ++edge) {
      const auto a = outlines[i][edge];
      const auto b = outlines[i][(edge + 1) % outlines[i].size()];
      const auto c = outlines[i][(edge + 2) % outlines[i].size()];
      if (int64_t(b.x - a.x) * (c.y - b.y) - int64_t(b.y - a.y) * (c.x - b.x) <= 0) {
        result.refusal = "path: miter pinches a convex part; enlarge the arm or reduce the bend";
        return result;
      }
    }
    result.ports.push_back({uint32_t(i), 0, motif_path_port_kind::wall});
    result.ports.push_back({uint32_t(i), 2, motif_path_port_kind::wall});
  }
  result.ports.push_back({0, 3, motif_path_port_kind::start});
  result.ports.push_back({uint32_t(outlines.size() - 1), 1, motif_path_port_kind::end});
  return result;
}

bool convex_interiors_overlap(const std::span<const motif_path_point> a,
                              const std::span<const motif_path_point> b) noexcept {
  if (a.size() < 3 || b.size() < 3) return false;
  return !separated(a, a, b) && !separated(b, a, b);
}

motif_edge_port make_motif_edge_port(const std::span<const motif_path_point> outline,
                                     const uint32_t edge, const uint32_t width,
                                     const uint32_t position_per_mille, const uint32_t margin) {
  motif_edge_port result;
  if (outline.size() < 3 || edge >= outline.size() || width == 0 || width > 1'048'576 ||
      margin > 1'048'576 || position_per_mille > 1000) {
    result.refusal = "port: invalid edge, width or position";
    return result;
  }
  for (const auto point : outline) {
    if (std::abs(int64_t(point.x)) > 1'048'576 || std::abs(int64_t(point.y)) > 1'048'576) {
      result.refusal = "port: coordinates exceed the supported precision range";
      return result;
    }
  }
  const auto a = outline[edge];
  const auto b = outline[(edge + 1) % outline.size()];
  const auto dx = int64_t(b.x) - a.x;
  const auto dy = int64_t(b.y) - a.y;
  const auto steps = std::gcd(std::abs(dx), std::abs(dy));
  if (steps == 0) {
    result.refusal = "port: zero-length edge";
    return result;
  }
  const auto sx = dx / steps;
  const auto sy = dy / steps;
  const auto step_length = fixed_length(sx, sy);
  const auto span = (int64_t(width) * scale + step_length - 1) / step_length;
  const auto inset = (int64_t(margin) * scale + step_length - 1) / step_length;
  if (span + 2 * inset > steps) {
    result.refusal = "port: aperture and margins do not fit the actual edge";
    return result;
  }
  const auto first = inset + (steps - span - 2 * inset) * position_per_mille / 1000;
  result.a = {int32_t(a.x + first * sx), int32_t(a.y + first * sy)};
  result.b = {int32_t(result.a.x + span * sx), int32_t(result.a.y + span * sy)};
  return result;
}

motif_port_attachment attach_motif_rect(const motif_edge_port& port, const uint32_t frontage,
                                        const uint32_t depth, const uint32_t passage_depth) {
  motif_port_attachment result;
  if (!port.valid() || depth == 0 || frontage == 0 || depth > 1'048'576 ||
      frontage > 1'048'576 || passage_depth > 1'048'576 ||
      std::abs(int64_t(port.a.x)) > 1'048'576 || std::abs(int64_t(port.a.y)) > 1'048'576 ||
      std::abs(int64_t(port.b.x)) > 1'048'576 || std::abs(int64_t(port.b.y)) > 1'048'576) {
    result.refusal = "attachment: invalid port or body size: " + port.refusal;
    return result;
  }
  const auto dx = int64_t(port.b.x) - port.a.x;
  const auto dy = int64_t(port.b.y) - port.a.y;
  const auto steps = std::gcd(std::abs(dx), std::abs(dy));
  if (steps == 0) {
    result.refusal = "attachment: zero-length aperture";
    return result;
  }
  const auto sx = dx / steps;
  const auto sy = dy / steps;
  const auto step_length = fixed_length(sx, sy);
  const auto body_steps = (int64_t(frontage) * scale + step_length - 1) / step_length;
  if (body_steps < steps) {
    result.refusal = "attachment: aperture is wider than the child frontage";
    return result;
  }
  const auto normal_offset = [&](const uint32_t distance) {
    return motif_path_point{rounded((sy * scale * scale / step_length) * distance),
                            rounded((-sx * scale * scale / step_length) * distance)};
  };
  const auto gap = normal_offset(passage_depth);
  const auto reach = normal_offset(depth);
  const auto translate = [](const motif_path_point a, const motif_path_point b) {
    return motif_path_point{a.x + b.x, a.y + b.y};
  };
  if (passage_depth != 0) {
    if (gap == motif_path_point{}) {
      result.refusal = "attachment: passage depth is below coordinate precision";
      return result;
    }
    result.passage = {port.b, port.a, translate(port.a, gap), translate(port.b, gap)};
  }
  const auto extra = (body_steps - steps) / 2;
  const motif_path_point a{int32_t(port.a.x - extra * sx + gap.x),
                           int32_t(port.a.y - extra * sy + gap.y)};
  const motif_path_point b{int32_t(a.x + body_steps * sx), int32_t(a.y + body_steps * sy)};
  result.body = {b, a, translate(a, reach), translate(b, reach)};
  return result;
}

bool share_motif_boundary(const std::span<const motif_path_point> a,
                          const std::span<const motif_path_point> b) noexcept {
  return visit_shared_boundary(a, b, [](const motif_boundary_segment) { return true; });
}

std::vector<motif_boundary_segment> shared_motif_boundary(
  const std::span<const motif_path_point> a, const std::span<const motif_path_point> b) {
  std::vector<motif_boundary_segment> result;
  visit_shared_boundary(a, b, [&](const motif_boundary_segment segment) {
    result.push_back(segment);
    return false;
  });
  return result;
}

motif_path_trim trim_motif_path_end(const std::span<const motif_path_point> outline,
                                   const uint32_t cap_edge,
                                   const std::span<const motif_path_point> protected_points,
                                   const uint32_t clearance, const uint32_t minimum_length) {
  motif_path_trim result;
  if (outline.size() != 4 || cap_edge >= 4 || clearance > 1'048'576 ||
      minimum_length == 0 || minimum_length > 1'048'576) {
    result.refusal = "path trim: needs a quadrilateral cap and supported margins";
    return result;
  }
  for (const auto point : outline) {
    if (std::abs(int64_t(point.x)) > 1'048'576 || std::abs(int64_t(point.y)) > 1'048'576) {
      result.refusal = "path trim: coordinates exceed the supported precision range";
      return result;
    }
  }
  const auto a = outline[cap_edge];
  const auto b = outline[(cap_edge + 1) % 4];
  const auto vertical = a.x == b.x && a.y != b.y;
  const auto horizontal = a.y == b.y && a.x != b.x;
  if ((!vertical && !horizontal) ||
      (vertical && (outline[(cap_edge + 3) % 4].y != a.y || outline[(cap_edge + 2) % 4].y != b.y)) ||
      (horizontal && (outline[(cap_edge + 3) % 4].x != a.x || outline[(cap_edge + 2) % 4].x != b.x))) {
    result.refusal = "path trim: cap must be axial and its side walls parallel";
    return result;
  }
  for (size_t i = 0; i < 4; ++i) {
    const auto p = outline[i], q = outline[(i + 1) % 4], r = outline[(i + 2) % 4];
    if ((int64_t(q.x) - p.x) * (int64_t(r.y) - q.y) -
        (int64_t(q.y) - p.y) * (int64_t(r.x) - q.x) <= 0) {
      result.refusal = "path trim: outline must be strictly convex and CCW";
      return result;
    }
  }
  const int64_t direction = vertical ? (b.y > a.y ? 1 : -1) : (b.x > a.x ? -1 : 1);
  const auto along = [&](const motif_path_point point) {
    return direction * (vertical ? int64_t(point.x) : int64_t(point.y));
  };
  const auto cap = along(a);
  auto required = std::max(along(outline[(cap_edge + 2) % 4]),
                           along(outline[(cap_edge + 3) % 4])) + minimum_length;
  if (required > cap) {
    result.refusal = "path trim: original part cannot hold the declared minimum length";
    return result;
  }
  for (const auto point : protected_points) {
    for (size_t i = 0; i < 4; ++i) {
      const auto p = outline[i], q = outline[(i + 1) % 4];
      if ((int64_t(q.x) - p.x) * (int64_t(point.y) - p.y) -
          (int64_t(q.y) - p.y) * (int64_t(point.x) - p.x) < 0) {
        result.refusal = "path trim: protected point is outside the part";
        return result;
      }
    }
    required = std::max(required, along(point) + clearance);
  }
  result.removed = uint32_t(std::max(int64_t(0), cap - required));
  result.outline.assign(outline.begin(), outline.end());
  for (const auto index : {cap_edge, (cap_edge + 1) % 4}) {
    if (vertical) result.outline[index].x -= int32_t(direction * result.removed);
    else result.outline[index].y -= int32_t(direction * result.removed);
  }
  return result;
}

} // namespace devils_engine::originator
