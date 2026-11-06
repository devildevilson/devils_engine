#include "devils_engine/originator/field_surface.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <gtl/phmap.hpp>
#include "devils_engine/utils/prng.h"

// ПОВЕРХНОСТЬ С ЛОКАЛЬНЫМ УТОЧНЕНИЕМ: мелкие ходы нельзя восстанавливать по
// крупной решётке. Внутри выбранных клеток — обычные тетраэдры мелкой сетки;
// переходная клетка соединяет центр с согласованным разбиением граней. Даже у
// неуточнённой грани ребро может требовать мелких вершин от третьего соседа.
// Решение зависит от мировых коробок в поясе, а не от наличия соседнего чанка.
// Значения и нормали спрашиваются у того же поля; предметных правил здесь нет.
namespace devils_engine::originator {
namespace {
using vec = field_vector;
using coord = std::array<int, 3>;
using tet = std::array<uint32_t, 4>;
vec sub(const vec& a, const vec& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
vec cross(const vec& a, const vec& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const vec& a, const vec& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
vec normal(vec v) {
  double l = std::sqrt(dot(v, v));
  if (l < 1e-15) return {0, 1, 0};
  for (auto& x : v)
    x /= l;
  return v;
}
struct coord_hash {
  size_t operator()(const coord& p) const noexcept {
    return size_t(utils::mix(uint64_t(p[0]), uint64_t(p[1]), uint64_t(p[2])));
  }
};
struct grid {
  const field_surface_options& o;
  std::span<const field_box> boxes;
  int n, q;
  std::vector<vec> points;
  std::vector<tet> tets;
  gtl::flat_hash_map<coord, uint32_t, coord_hash> vertices;
  gtl::flat_hash_map<coord, bool, coord_hash> refined;
  uint32_t point(const coord& c) {
    auto [it, added] = vertices.emplace(c, uint32_t(points.size()));
    if (added) {
      if (points.size() >= o.point_capacity) utils::error{}("originator field_surface: point capacity {} exceeded", o.point_capacity);
      vec p{};
      for (int a = 0; a < 3; ++a)
        p[a] = o.origin[a] + double(c[a]) * (o.cell_size / q);
      points.push_back(p);
    }
    return it->second;
  }
  bool fine(const coord& c) {
    auto old = refined.find(c);
    if (old != refined.end()) return old->second;
    bool hit = false;
    for (const auto& box : boxes) {
      bool overlap = true;
      for (int a = 0; a < 3; ++a) {
        double low = o.origin[a] + c[a] * o.cell_size;
        overlap &= low <= box.high[a] && low + o.cell_size >= box.low[a];
      }
      if (overlap) {
        hit = true;
        break;
      }
    }
    refined.emplace(c, hit);
    return hit;
  }
  void add(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (tets.size() >= o.tetrahedron_capacity) utils::error{}("originator field_surface: tetrahedron capacity {} exceeded", o.tetrahedron_capacity);
    tets.push_back({a, b, c, d});
  }
  void fine_cell(const coord& c) {
    constexpr int corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    constexpr int indices[6][4] = {{0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7}, {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};
    for (int z = 0; z < n; ++z)
      for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
          uint32_t v[8];
          for (int i = 0; i < 8; ++i)
            v[i] = point({c[0] * q + (x + corners[i][0]) * 2, c[1] * q + (y + corners[i][1]) * 2, c[2] * q + (z + corners[i][2]) * 2});
          for (const auto& t : indices)
            add(v[t[0]], v[t[1]], v[t[2]], v[t[3]]);
        }
  }
  void coarse_cell(const coord& c) {
    const uint32_t centre = point({c[0] * q + n, c[1] * q + n, c[2] * q + n});
    for (int axis = 0; axis < 3; ++axis)
      for (int side = 0; side < 2; ++side) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        coord neighbour = c;
        neighbour[axis] += side ? 1 : -1;
        coord face = c;
        for (int a = 0; a < 3; ++a)
          face[a] *= q;
        face[axis] += side * q;
        auto at = [&](int a, int b) {
          coord p = face;
          p[u] += a;
          p[v] += b;
          return point(p);
        };
        if (fine(neighbour)) {
          for (int b = 0; b < n; ++b)
            for (int a = 0; a < n; ++a) {
              auto v0 = at(a * 2, b * 2), v1 = at((a + 1) * 2, b * 2), v2 = at(a * 2, (b + 1) * 2), v3 = at((a + 1) * 2, (b + 1) * 2);
              add(centre, v0, v1, v3);
              add(centre, v0, v3, v2);
            }
        } else {
          auto middle = at(n, n);
          for (int edge = 0; edge < 4; ++edge) {
            int along = edge < 2 ? u : v, fixed = edge < 2 ? v : u, end = edge % 2;
            bool split = false;
            for (int a = -1; a <= 0; ++a)
              for (int b = -1; b <= 0; ++b) {
                coord adjacent = c;
                adjacent[axis] = c[axis] + side + a;
                adjacent[fixed] = c[fixed] + end + b;
                split |= fine(adjacent);
              }
            int count = split ? n : 1;
            for (int i = 0; i < count; ++i) {
              coord a = face, b = face;
              a[fixed] += end * q;
              b[fixed] += end * q;
              a[along] += i * q / count;
              b[along] += (i + 1) * q / count;
              add(centre, middle, point(a), point(b));
            }
          }
        }
      }
  }
};
} // namespace
std::vector<field_surface_vertex> make_field_surface(const primitive_field& field, const field_surface_options& o, std::span<const field_box> boxes) {
  if (o.point_capacity > std::numeric_limits<uint32_t>::max() || o.cells == 0 || o.cells > 128 || !(o.cell_size > 0) || !std::isfinite(o.cell_size) || !(o.normal_step > 0) || !std::isfinite(o.normal_step) || !std::isfinite(o.detail_normal_step) || o.detail_normal_step < 0 || o.refinement < 2 || o.refinement > 8 || (o.refinement & (o.refinement - 1)))
    utils::error{}("originator field_surface: positive cells <= 128, spacing/normal_step and refinement 2/4/8 required");
  for (double v : o.origin)
    if (!std::isfinite(v)) utils::error{}("originator field_surface: nonfinite origin");
  grid mesh{o, boxes, int(o.refinement), int(o.refinement * 2), {}, {}, {}, {}};
  for (int z = 0; z < int(o.cells); ++z)
    for (int y = 0; y < int(o.cells); ++y)
      for (int x = 0; x < int(o.cells); ++x) {
        coord c{x, y, z};
        if (mesh.fine(c))
          mesh.fine_cell(c);
        else
          mesh.coarse_cell(c);
      }
  std::vector<double> values(mesh.points.size());
  field.sample_points(mesh.points, values, {}, 8);
  std::vector<vec> intersections;
  gtl::flat_hash_map<uint64_t, uint32_t> edges;
  std::vector<uint32_t> triangles;
  auto emit = [&](uint32_t a, uint32_t b) {
    uint64_t key = uint64_t(std::min(a, b)) << 32 | std::max(a, b);
    auto [it, added] = edges.emplace(key, uint32_t(intersections.size()));
    if (added) {
      if (mesh.points[b] < mesh.points[a]) std::swap(a, b);
      double t = values[a] / (values[a] - values[b]);
      vec p{};
      for (int axis = 0; axis < 3; ++axis)
        p[axis] = mesh.points[a][axis] + (mesh.points[b][axis] - mesh.points[a][axis]) * t;
      intersections.push_back(p);
    }
    if (triangles.size() >= o.vertex_capacity) utils::error{}("originator field_surface: output exceeds {} vertices", o.vertex_capacity);
    triangles.push_back(it->second);
  };
  for (const auto& t : mesh.tets) {
    uint32_t in[4], out[4];
    int ni = 0, no = 0;
    for (auto i : t) {
      if (values[i] < 0)
        in[ni++] = i;
      else
        out[no++] = i;
    }
    if (ni == 1) {
      emit(in[0], out[0]);
      emit(in[0], out[1]);
      emit(in[0], out[2]);
    } else if (ni == 3) {
      emit(out[0], in[0]);
      emit(out[0], in[2]);
      emit(out[0], in[1]);
    } else if (ni == 2) {
      emit(in[0], out[0]);
      emit(in[0], out[1]);
      emit(in[1], out[0]);
      emit(in[0], out[1]);
      emit(in[1], out[1]);
      emit(in[1], out[0]);
    }
  }
  std::vector<vec> queries;
  queries.reserve(intersections.size() * 7);
  for (const auto& p : intersections) {
    double step = o.normal_step;
    if (o.detail_normal_step > 0 && o.detail_normal_step != o.normal_step) {
      double distance = o.normal_step;
      for (const auto& box : boxes) {
        double squared = 0;
        for (size_t axis = 0; axis < 3; ++axis) {
          double d = std::max({box.low[axis] - p[axis], p[axis] - box.high[axis], 0.0});
          squared += d * d;
        }
        distance = std::min(distance, std::sqrt(squared));
      }
      double t = std::clamp(distance / o.normal_step, 0.0, 1.0);
      t = t * t * (3 - 2 * t);
      step = o.detail_normal_step + (o.normal_step - o.detail_normal_step) * t;
    }
    queries.push_back(p);
    for (int axis = 0; axis < 3; ++axis)
      for (int sign : {-1, 1}) {
        auto q = p;
        q[axis] += sign * step;
        queries.push_back(q);
      }
  }
  std::vector<double> levels(queries.size());
  std::vector<uint32_t> tags(queries.size());
  field.sample_points(queries, levels, tags, 8);
  std::vector<vec> normals(intersections.size());
  for (size_t i = 0; i < normals.size(); ++i)
    normals[i] = normal({levels[i * 7 + 2] - levels[i * 7 + 1], levels[i * 7 + 4] - levels[i * 7 + 3], levels[i * 7 + 6] - levels[i * 7 + 5]});
  std::vector<field_surface_vertex> result;
  result.reserve(triangles.size());
  for (size_t i = 0; i < triangles.size(); i += 3) {
    auto a = triangles[i], b = triangles[i + 1], c = triangles[i + 2];
    auto face = cross(sub(intersections[b], intersections[a]), sub(intersections[c], intersections[a]));
    if (dot(face, face) < 1e-24) continue;
    if (dot(face, normals[a]) < 0) std::swap(b, c);
    for (auto v : {a, b, c})
      result.push_back({intersections[v], normals[v], tags[v * 7]});
  }
  return result;
}
// Верхняя оценка одновременно живых векторов, хеш-таблиц и выборки поля.
// Запас хеш-таблиц включает неполную загрузку и переразмещение; векторов — рост.
size_t field_surface_footprint(const tool_call& call) {
  const long double cells = std::clamp<int64_t>(call.params->integer("cells", 16), 0, 128);
  const long double refinement = std::clamp<int64_t>(call.params->integer("refinement", 4), 2, 8);
  const long double side = 2 * cells * refinement + 1;
  const long double points = 1 + std::min<long double>(side * side * side, call.params->integer("point_capacity", 2097152));
  const long double tets = std::min<long double>(6 * cells * cells * cells * refinement * refinement * refinement, call.params->integer("tetrahedron_capacity", 8388608));
  const long double vertices = std::min({call.output(0).count(), call.output(1).count(), call.output(2).count()}) + 1;
  const long double sample = std::max(points, vertices * 7);
  const auto& field = current_field(call.params->string("field"));
  const long double bytes = points * 256 + tets * 48 + vertices * 512 + (cells + 2) * (cells + 2) * (cells + 2) * 128 + sample * (sizeof(field_point) * 4 + 256) + field.shape_count() * 32 + call.input(0).count() * sizeof(field_box) + 16384;
  return size_t(std::min(bytes, static_cast<long double>(std::numeric_limits<size_t>::max())));
}
void tool_field_surface(const tool_call& call, size_t, size_t) {
  if (call.input(0).type().components < 3 || call.input(1).type().components < 3 || call.output(0).type().components < 3 || call.output(1).type().components < 3)
    utils::error{}("originator step '{}': field_surface needs 3-component box/position/normal fields", call.step_name);

  const auto low = call.input(0).read(), high = call.input(1).read();
  const auto count = call.params->integer("count", int64_t(low.count()));
  if (count < 0 || size_t(count) > std::min(low.count(), high.count())) utils::error{}("originator step '{}': field_surface invalid box count for '{}.{}'", call.step_name, call.input(0).buffer_name(), call.input(0).field_name());
  std::vector<field_box> boxes;
  for (size_t i = 0; i < size_t(count); ++i) {
    field_box b;
    for (uint32_t a = 0; a < 3; ++a) {
      b.low[a] = low.get(i, a);
      b.high[a] = high.get(i, a);
      if (!std::isfinite(b.low[a]) || !std::isfinite(b.high[a]) || b.low[a] > b.high[a])
        utils::error{}("originator step '{}': field_surface invalid refinement box {}", call.step_name, i);
    }
    boxes.push_back(b);
  }
  field_surface_options o;
  for (uint32_t a = 0; a < 3; ++a)
    o.origin[a] = call.params->number(a == 0 ? "origin_x" : a == 1 ? "origin_y"
                                                                   : "origin_z",
                                      0);
  o.cells = uint32_t(call.params->integer("cells", 16));
  o.cell_size = call.params->number("cell_size", 1);
  o.refinement = uint32_t(call.params->integer("refinement", 4));
  o.normal_step = call.params->number("normal_step", 0.05);
  o.detail_normal_step = call.params->number("detail_normal_step", 0);
  if (call.params->integer("point_capacity", 2097152) < 1 || call.params->integer("tetrahedron_capacity", 8388608) < 1)
    utils::error{}("originator step '{}': field_surface capacities must be positive", call.step_name);
  o.point_capacity = size_t(call.params->integer("point_capacity", 2097152));
  o.tetrahedron_capacity = size_t(call.params->integer("tetrahedron_capacity", 8388608));
  o.vertex_capacity = std::min({call.output(0).count(), call.output(1).count(), call.output(2).count()});
  std::vector<field_surface_vertex> result;
  try {
    result = make_field_surface(current_field(call.params->string("field")), o, boxes);
  } catch (const std::exception& error) {
    utils::error{}("originator step '{}': field_surface writing '{}.{}': {}", call.step_name,
                   call.output(0).buffer_name(), call.output(0).field_name(), error.what());
  }
  auto p = call.output(0).write(), n = call.output(1).write(), tag = call.output(2).write();
  for (size_t i = 0; i < result.size(); ++i) {
    for (uint32_t a = 0; a < 3; ++a) {
      p.set(i, result[i].position[a], a);
      n.set(i, result[i].normal[a], a);
    }
    tag.set(i, result[i].tag);
  }
  call.output(3).write().set(0, double(result.size()));
}
} // namespace devils_engine::originator
