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
// Значения углов решётки чанка с поясом в одну клетку (индексы -1..cells+1): углы блоков пропуска,
// углы посчитанных клеток и соседи концов рёбер для нормалей.
struct lattice_values {
  int cells = 0;
  size_t side = 0;
  std::vector<double> value;
  std::vector<uint8_t> state; // 0 — неизвестно, 1 — известно, 2 — запрошено
  explicit lattice_values(int c) : cells(c), side(size_t(c) + 3), value(side * side * side, 0.0), state(side * side * side, 0) {}
  size_t index(int x, int y, int z) const { return size_t(x + 1) + side * (size_t(y + 1) + side * size_t(z + 1)); }
  void set(int x, int y, int z, double v) {
    const size_t i = index(x, y, z);
    value[i] = v;
    state[i] = 1;
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
  gtl::flat_hash_map<coord, bool, coord_hash> plains;
  // Грубая клетка, у которой ни один сосед по грани или ребру не уточнён: её рёбра не делятся, грани
  // режутся диагональю Куна.
  bool plain(const coord& c) {
    if (!o.kuhn_plain) return false;
    auto old = plains.find(c);
    if (old != plains.end()) return old->second;
    bool result = !fine(c);
    for (int dz = -1; dz <= 1 && result; ++dz)
      for (int dy = -1; dy <= 1 && result; ++dy)
        for (int dx = -1; dx <= 1 && result; ++dx) {
          if (std::abs(dx) + std::abs(dy) + std::abs(dz) == 3) continue;
          result = !fine({c[0] + dx, c[1] + dy, c[2] + dz});
        }
    plains.emplace(c, result);
    return result;
  }
  static constexpr int kuhn_corners[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  static constexpr int kuhn_tets[6][4] = {{0, 1, 3, 7}, {0, 3, 2, 7}, {0, 2, 6, 7}, {0, 6, 4, 7}, {0, 4, 5, 7}, {0, 5, 1, 7}};
  void kuhn_cell(const coord& c) {
    uint32_t v[8];
    for (int i = 0; i < 8; ++i)
      v[i] = point({(c[0] + kuhn_corners[i][0]) * q, (c[1] + kuhn_corners[i][1]) * q, (c[2] + kuhn_corners[i][2]) * q});
    for (const auto& t : kuhn_tets)
      add(v[t[0]], v[t[1]], v[t[2]], v[t[3]]);
  }
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
        } else if (plain(neighbour)) {
          // Сосед режет общую грань диагональю Куна: от меньшего угла к большему, без середины.
          auto v0 = at(0, 0), v1 = at(q, 0), v2 = at(0, q), v3 = at(q, q);
          add(centre, v0, v1, v3);
          add(centre, v0, v3, v2);
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
  if ((o.skip_block != 0 || o.lattice_normals) && !o.kuhn_plain)
    utils::error{}("originator field_surface: skip {} and lattice normals {} need plain = kuhn", o.skip_block, o.lattice_normals);
  if (!(o.band > 0))
    utils::error{}("originator field_surface: continuity band {} must be positive", o.band);
  if (o.skip_block != 0 && (o.skip_block > o.cells || o.cells % o.skip_block != 0))
    utils::error{}("originator field_surface: skip block {} must divide cells {}", o.skip_block, o.cells);
  const int n = int(o.refinement), q = n * 2, cells = int(o.cells);
  grid mesh{o, boxes, n, q, {}, {}, {}, {}, {}};
  std::vector<double> values;
  std::vector<vec> intersections;
  // Концы ребра вершины (после упорядочивания) и доля пути от первого: для нормалей из решётки.
  std::vector<std::array<uint32_t, 2>> edge_ends;
  std::vector<double> edge_t;
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
      edge_ends.push_back({a, b});
      edge_t.push_back(t);
    }
    if (triangles.size() >= o.vertex_capacity) utils::error{}("originator field_surface: output exceeds {} vertices", o.vertex_capacity);
    triangles.push_back(it->second);
  };
  auto march = [&](const tet& t) {
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
  };
  lattice_values lattice(cells);
  const double cell = o.cell_size;
  const auto lattice_point = [&](int x, int y, int z) {
    return vec{o.origin[0] + double(x * q) * (cell / q), o.origin[1] + double(y * q) * (cell / q), o.origin[2] + double(z * q) * (cell / q)};
  };
  // ПРОПУСК ПУСТЫХ БЛОКОВ. Поле, обрезанное до полосы [-band, band], непрерывно (формы обрываются на
  // коробках только за полосой) и в точке блока отличается от ближайшего угла не больше чем на наклон *
  // половину диагонали: углы одного знака и дальше от нуля — знак во всём блоке тот же.
  const int skip = int(o.skip_block), blocks = skip != 0 ? cells / skip : 0;
  std::vector<uint8_t> skipped(size_t(blocks) * size_t(blocks) * size_t(blocks), 0);
  std::vector<double> block_slope(skipped.size(), 0.0);
  std::vector<uint8_t> block_roles(skipped.size(), 0);
  const double infinite = std::numeric_limits<double>::infinity();
  const auto banded = [&](size_t b, double v) {
    return std::clamp(v, (block_roles[b] & 1) ? -o.band : -infinite, (block_roles[b] & 2) ? o.band : infinite);
  };
  const auto block_of = [&](int x, int y, int z) { return size_t(x / skip) + size_t(blocks) * (size_t(y / skip) + size_t(blocks) * size_t(z / skip)); };
  const auto decide_skips = [&](const std::vector<uint8_t>& eligible) {
    std::vector<vec> corners;
    std::vector<std::array<int, 3>> where;
    for (int bz = 0; bz < blocks; ++bz)
      for (int by = 0; by < blocks; ++by)
        for (int bx = 0; bx < blocks; ++bx) {
          if (!eligible[size_t(bx) + size_t(blocks) * (size_t(by) + size_t(blocks) * size_t(bz))]) continue;
          for (const auto& k : grid::kuhn_corners) {
            const int x = (bx + k[0]) * skip, y = (by + k[1]) * skip, z = (bz + k[2]) * skip;
            auto& state = lattice.state[lattice.index(x, y, z)];
            if (state != 0) continue;
            state = 2;
            corners.push_back(lattice_point(x, y, z));
            where.push_back({x, y, z});
          }
        }
    std::vector<double> got(corners.size());
    field.sample_points(corners, got, {}, 8);
    for (size_t i = 0; i < where.size(); ++i) lattice.set(where[i][0], where[i][1], where[i][2], got[i]);
    const vec far{o.origin[0] + cells * cell, o.origin[1] + cells * cell, o.origin[2] + cells * cell};
    field.prepare(o.origin, far).slopes(o.origin, skip * cell, {blocks, blocks, blocks}, block_slope, block_roles);
    const double half_diagonal = skip * cell * std::sqrt(3.0) / 2;
    for (int bz = 0; bz < blocks; ++bz)
      for (int by = 0; by < blocks; ++by)
        for (int bx = 0; bx < blocks; ++bx) {
          const size_t b = size_t(bx) + size_t(blocks) * (size_t(by) + size_t(blocks) * size_t(bz));
          if (!eligible[b] || !std::isfinite(block_slope[b])) continue;
          const double reach = block_slope[b] * half_diagonal;
          bool empty = true, first = true, negative = false;
          for (const auto& k : grid::kuhn_corners) {
            const double v = banded(b, lattice.value[lattice.index((bx + k[0]) * skip, (by + k[1]) * skip, (bz + k[2]) * skip)]);
            if (first) negative = v < 0;
            first = false;
            // Отрицательная сторона строго: точка со значением 0 уже порода.
            empty &= negative ? (v < 0 && -v > reach) : (v >= reach);
          }
          skipped[b] = empty;
        }
  };
  // Обещание наклона проверяется на посчитанных блоках: соседние углы решётки в полосе не расходятся
  // больше, чем наклон блока на шаг. Иначе пропущенный блок мог скрыть поверхность — громкий отказ.
  const auto check_slopes = [&](const std::vector<uint8_t>& eligible) {
    for (int bz = 0; bz < blocks; ++bz)
      for (int by = 0; by < blocks; ++by)
        for (int bx = 0; bx < blocks; ++bx) {
          const size_t b = size_t(bx) + size_t(blocks) * (size_t(by) + size_t(blocks) * size_t(bz));
          if (!eligible[b] || skipped[b] || !std::isfinite(block_slope[b])) continue;
          const double limit = block_slope[b] * cell * (1 + 1e-9) + 1e-9;
          for (int z = bz * skip; z <= (bz + 1) * skip; ++z)
            for (int y = by * skip; y <= (by + 1) * skip; ++y)
              for (int x = bx * skip; x <= (bx + 1) * skip; ++x)
                for (int axis = 0; axis < 3; ++axis) {
                  std::array<int, 3> c{x, y, z}, d = c;
                  if (++d[size_t(axis)] > (axis == 0 ? bx : axis == 1 ? by : bz) * skip + skip) continue;
                  const double a = banded(b, lattice.value[lattice.index(c[0], c[1], c[2])]), e = banded(b, lattice.value[lattice.index(d[0], d[1], d[2])]);
                  if (std::abs(a - e) > limit) {
                    const auto pa = lattice_point(c[0], c[1], c[2]), pe = lattice_point(d[0], d[1], d[2]);
                    utils::error{}("originator field_surface: field within band {} goes from {} at ({}, {}, {}) to {} at ({}, {}, {}), a change of {} over {} m, "
                                   "beyond the declared slope {} of its block: a shape slope is understated or a box cuts its shape inside the band",
                                   o.band, a, pa[0], pa[1], pa[2], e, pe[0], pe[1], pe[2], std::abs(a - e), cell, block_slope[b]);
                  }
                }
        }
  };
  if (boxes.empty() && o.kuhn_plain) {
    // Ни одной коробки и клетки Куна: только углы решётки.
    const size_t side = size_t(cells), total = (side + 1) * (side + 1) * (side + 1);
    if (total > o.point_capacity) utils::error{}("originator field_surface: point capacity {} exceeded", o.point_capacity);
    if (6 * side * side * side > o.tetrahedron_capacity) utils::error{}("originator field_surface: tetrahedron capacity {} exceeded", o.tetrahedron_capacity);
    const auto corner = [&](int x, int y, int z) { return uint32_t(size_t(x) + (side + 1) * (size_t(y) + (side + 1) * size_t(z))); };
    mesh.points.resize(total);
    for (int z = 0; z <= cells; ++z)
      for (int y = 0; y <= cells; ++y)
        for (int x = 0; x <= cells; ++x) {
          const coord c{x * q, y * q, z * q};
          for (int a = 0; a < 3; ++a)
            mesh.points[corner(x, y, z)][size_t(a)] = o.origin[size_t(a)] + double(c[size_t(a)]) * (o.cell_size / q);
        }
    values.resize(total);
    if (skip == 0) {
      field.sample_points(mesh.points, values, {}, 8);
    } else {
      const std::vector<uint8_t> eligible(skipped.size(), 1);
      decide_skips(eligible);
      // Углы непропущенных блоков: известные — из решётки, остальные — одной выборкой.
      std::vector<uint8_t> need(total, 0);
      for (int bz = 0; bz < blocks; ++bz)
        for (int by = 0; by < blocks; ++by)
          for (int bx = 0; bx < blocks; ++bx) {
            if (skipped[size_t(bx) + size_t(blocks) * (size_t(by) + size_t(blocks) * size_t(bz))]) continue;
            for (int z = bz * skip; z <= (bz + 1) * skip; ++z)
              for (int y = by * skip; y <= (by + 1) * skip; ++y)
                for (int x = bx * skip; x <= (bx + 1) * skip; ++x) need[corner(x, y, z)] = 1;
          }
      std::vector<vec> asked;
      std::vector<uint32_t> asked_index;
      for (int z = 0; z <= cells; ++z)
        for (int y = 0; y <= cells; ++y)
          for (int x = 0; x <= cells; ++x) {
            const uint32_t i = corner(x, y, z);
            const size_t l = lattice.index(x, y, z);
            if (lattice.state[l] == 1) values[i] = lattice.value[l];
            else if (need[i]) {
              asked.push_back(mesh.points[i]);
              asked_index.push_back(i);
            }
          }
      std::vector<double> got(asked.size());
      field.sample_points(asked, got, {}, 8);
      for (size_t k = 0; k < asked.size(); ++k) values[asked_index[k]] = got[k];
      for (int z = 0; z <= cells; ++z)
        for (int y = 0; y <= cells; ++y)
          for (int x = 0; x <= cells; ++x)
            if (need[corner(x, y, z)]) lattice.set(x, y, z, values[corner(x, y, z)]);
      check_slopes(eligible);
    }
    if (o.lattice_normals && skip == 0)
      for (int z = 0; z <= cells; ++z)
        for (int y = 0; y <= cells; ++y)
          for (int x = 0; x <= cells; ++x) lattice.set(x, y, z, values[corner(x, y, z)]);
    for (int z = 0; z < cells; ++z)
      for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x) {
          if (skip != 0 && skipped[block_of(x, y, z)]) continue;
          uint32_t v[8];
          for (int i = 0; i < 8; ++i)
            v[i] = corner(x + grid::kuhn_corners[i][0], y + grid::kuhn_corners[i][1], z + grid::kuhn_corners[i][2]);
          const bool first = values[v[0]] < 0;
          bool mixed = false;
          for (int i = 1; i < 8 && !mixed; ++i) mixed = (values[v[i]] < 0) != first;
          if (!mixed) continue;
          for (const auto& t : grid::kuhn_tets)
            march({v[t[0]], v[t[1]], v[t[2]], v[t[3]]});
        }
  } else if (boxes.empty()) {
    // НИ ОДНОЙ КОРОБКИ: каждая клетка грубая, а её сетка — неподвижная решётка из углов, центров клеток
    // и центров граней. Точки индексируются прямо, тетраэдры обходятся в том же порядке, что у общего
    // пути, без хранения, а клетка без смены знака среди своих 15 точек не даёт ни одной вершины и
    // пропускается. Значение точки от разбиения на группы выборки не зависит, поэтому сетка та же бит в бит.
    const size_t side = size_t(cells), corners = (side + 1) * (side + 1) * (side + 1), centres = side * side * side, faces = (side + 1) * side * side;
    const size_t total = corners + centres + 3 * faces;
    if (total > o.point_capacity) utils::error{}("originator field_surface: point capacity {} exceeded", o.point_capacity);
    if (24 * centres > o.tetrahedron_capacity) utils::error{}("originator field_surface: tetrahedron capacity {} exceeded", o.tetrahedron_capacity);
    const auto corner = [&](int x, int y, int z) { return uint32_t(size_t(x) + (side + 1) * (size_t(y) + (side + 1) * size_t(z))); };
    const auto centre = [&](int x, int y, int z) { return uint32_t(corners + size_t(x) + side * (size_t(y) + side * size_t(z))); };
    // Грань оси axis клетки c (со сдвигом side): решётка (cells+1) по оси и cells по двум другим.
    const auto face = [&](int axis, std::array<int, 3> c) {
      std::array<size_t, 3> dims{side, side, side};
      dims[size_t(axis)] = side + 1;
      return uint32_t(corners + centres + size_t(axis) * faces + size_t(c[0]) + dims[0] * (size_t(c[1]) + dims[1] * size_t(c[2])));
    };
    mesh.points.resize(total);
    const auto place = [&](uint32_t index, int x, int y, int z) {
      const coord c{x, y, z};
      for (int a = 0; a < 3; ++a)
        mesh.points[index][size_t(a)] = o.origin[size_t(a)] + double(c[size_t(a)]) * (o.cell_size / q);
    };
    for (int z = 0; z <= cells; ++z)
      for (int y = 0; y <= cells; ++y)
        for (int x = 0; x <= cells; ++x) {
          place(corner(x, y, z), x * q, y * q, z * q);
          if (x < cells && y < cells && z < cells) place(centre(x, y, z), x * q + n, y * q + n, z * q + n);
          for (int axis = 0; axis < 3; ++axis) {
            std::array<int, 3> c{x, y, z};
            bool inside = true;
            for (int a = 0; a < 3; ++a)
              if (a != axis && c[size_t(a)] == cells) inside = false;
            if (!inside) continue;
            std::array<int, 3> p{x * q + n, y * q + n, z * q + n};
            p[size_t(axis)] = c[size_t(axis)] * q;
            place(face(axis, c), p[0], p[1], p[2]);
          }
        }
    values.resize(total);
    field.sample_points(mesh.points, values, {}, 8);
    for (int z = 0; z < cells; ++z)
      for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x) {
          const std::array<int, 3> c{x, y, z};
          const uint32_t middle_of_cell = centre(x, y, z);
          std::array<uint32_t, 6> middles{};
          for (int axis = 0; axis < 3; ++axis)
            for (int s = 0; s < 2; ++s) {
              auto f = c;
              f[size_t(axis)] += s;
              middles[size_t(axis * 2 + s)] = face(axis, f);
            }
          const bool first = values[middle_of_cell] < 0;
          bool mixed = false;
          for (int i = 0; i < 8 && !mixed; ++i) mixed = (values[corner(x + (i & 1), y + (i >> 1 & 1), z + (i >> 2))] < 0) != first;
          for (int i = 0; i < 6 && !mixed; ++i) mixed = (values[middles[size_t(i)]] < 0) != first;
          if (!mixed) continue;
          for (int axis = 0; axis < 3; ++axis)
            for (int s = 0; s < 2; ++s) {
              const int u = (axis + 1) % 3, v = (axis + 2) % 3;
              auto base = c;
              base[size_t(axis)] += s;
              const uint32_t middle = middles[size_t(axis * 2 + s)];
              for (int edge = 0; edge < 4; ++edge) {
                const int along = edge < 2 ? u : v, fixed = edge < 2 ? v : u, end = edge % 2;
                auto a = base;
                a[size_t(fixed)] += end;
                auto b = a;
                b[size_t(along)] += 1;
                march({middle_of_cell, middle, corner(a[0], a[1], a[2]), corner(b[0], b[1], b[2])});
              }
            }
        }
  } else {
    // Пропускать можно только блок из одних простых клеток: у остальных точки не на решётке.
    std::vector<uint8_t> eligible(skipped.size(), 0);
    if (skip != 0) {
      for (int bz = 0; bz < blocks; ++bz)
        for (int by = 0; by < blocks; ++by)
          for (int bx = 0; bx < blocks; ++bx) {
            bool all = true;
            for (int z = bz * skip; z < (bz + 1) * skip && all; ++z)
              for (int y = by * skip; y < (by + 1) * skip && all; ++y)
                for (int x = bx * skip; x < (bx + 1) * skip && all; ++x) all = mesh.plain({x, y, z});
            eligible[size_t(bx) + size_t(blocks) * (size_t(by) + size_t(blocks) * size_t(bz))] = all;
          }
      decide_skips(eligible);
    }
    for (int z = 0; z < cells; ++z)
      for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x) {
          coord c{x, y, z};
          if (mesh.fine(c))
            mesh.fine_cell(c);
          else if (mesh.plain(c)) {
            if (skip != 0 && skipped[block_of(x, y, z)]) continue;
            mesh.kuhn_cell(c);
          } else
            mesh.coarse_cell(c);
        }
    values.resize(mesh.points.size());
    if (skip == 0) {
      field.sample_points(mesh.points, values, {}, 8);
    } else {
      std::vector<uint8_t> have(mesh.points.size(), 0);
      for (int z = 0; z <= cells; ++z)
        for (int y = 0; y <= cells; ++y)
          for (int x = 0; x <= cells; ++x) {
            const size_t l = lattice.index(x, y, z);
            if (lattice.state[l] != 1) continue;
            const auto found = mesh.vertices.find(coord{x * q, y * q, z * q});
            if (found == mesh.vertices.end()) continue;
            values[found->second] = lattice.value[l];
            have[found->second] = 1;
          }
      std::vector<vec> asked;
      std::vector<uint32_t> asked_index;
      for (uint32_t i = 0; i < mesh.points.size(); ++i)
        if (!have[i]) {
          asked.push_back(mesh.points[i]);
          asked_index.push_back(i);
        }
      std::vector<double> got(asked.size());
      field.sample_points(asked, got, {}, 8);
      for (size_t k = 0; k < asked.size(); ++k) values[asked_index[k]] = got[k];
    }
    if (skip != 0 || o.lattice_normals) {
      for (const auto& [c, i] : mesh.vertices)
        if (c[0] % q == 0 && c[1] % q == 0 && c[2] % q == 0) lattice.set(c[0] / q, c[1] / q, c[2] / q, values[i]);
    }
    if (skip != 0) check_slopes(eligible);
    for (const auto& t : mesh.tets) march(t);
  }
  std::vector<vec> queries;
  queries.reserve(intersections.size() * 7);
  // Нормаль из решётки: концы ребра — углы решётки (их координаты в клетках).
  std::vector<uint32_t> first_query(intersections.size());
  std::vector<std::array<int, 6>> lattice_ends(o.lattice_normals ? intersections.size() : 0);
  std::vector<uint8_t> from_lattice(intersections.size(), 0);
  const auto lattice_of = [&](uint32_t i, int* out) {
    for (size_t a = 0; a < 3; ++a) {
      const double u = (mesh.points[i][a] - o.origin[a]) / cell;
      const double k = std::round(u);
      if (std::abs(u - k) > 1e-6) return false;
      out[a] = int(k);
    }
    return true;
  };
  for (size_t index = 0; index < intersections.size(); ++index) {
    const auto& p = intersections[index];
    first_query[index] = uint32_t(queries.size());
    const bool detail = o.detail_normal_step > 0 && o.detail_normal_step != o.normal_step;
    double distance = o.normal_step;
    if (detail || o.lattice_normals) {
      for (const auto& box : boxes) {
        double squared = 0;
        for (size_t axis = 0; axis < 3; ++axis) {
          double d = std::max({box.low[axis] - p[axis], p[axis] - box.high[axis], 0.0});
          squared += d * d;
        }
        distance = std::min(distance, std::sqrt(squared));
      }
    }
    if (o.lattice_normals && distance >= o.normal_step && lattice_of(edge_ends[index][0], &lattice_ends[index][0]) &&
        lattice_of(edge_ends[index][1], &lattice_ends[index][3])) {
      from_lattice[index] = 1;
      queries.push_back(p);
      continue;
    }
    double step = o.normal_step;
    if (detail) {
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
  // Соседи концов рёбер, которых решётка ещё не знает (пропущенный блок, пояс за чанком), — той же выборкой.
  const size_t vertex_queries = queries.size();
  std::vector<std::array<int, 3>> asked;
  for (size_t i = 0; i < from_lattice.size(); ++i) {
    if (!from_lattice[i]) continue;
    for (int end = 0; end < 2; ++end)
      for (int axis = 0; axis < 3; ++axis)
        for (int sign : {-1, 1}) {
          std::array<int, 3> c{lattice_ends[i][end * 3], lattice_ends[i][end * 3 + 1], lattice_ends[i][end * 3 + 2]};
          c[size_t(axis)] += sign;
          auto& state = lattice.state[lattice.index(c[0], c[1], c[2])];
          if (state != 0) continue;
          state = 2;
          asked.push_back(c);
          queries.push_back(lattice_point(c[0], c[1], c[2]));
        }
  }
  std::vector<double> levels(queries.size());
  std::vector<uint32_t> tags(queries.size());
  field.sample_points(queries, levels, tags, 8);
  for (size_t k = 0; k < asked.size(); ++k) lattice.set(asked[k][0], asked[k][1], asked[k][2], levels[vertex_queries + k]);
  const auto gradient = [&](const int* c) {
    const auto at = [&](int dx, int dy, int dz) { return lattice.value[lattice.index(c[0] + dx, c[1] + dy, c[2] + dz)]; };
    return vec{at(1, 0, 0) - at(-1, 0, 0), at(0, 1, 0) - at(0, -1, 0), at(0, 0, 1) - at(0, 0, -1)};
  };
  std::vector<vec> normals(intersections.size());
  for (size_t i = 0; i < normals.size(); ++i) {
    const size_t f = first_query[i];
    if (from_lattice[i]) {
      const vec ga = gradient(&lattice_ends[i][0]), gb = gradient(&lattice_ends[i][3]);
      const double t = edge_t[i];
      normals[i] = normal({ga[0] + (gb[0] - ga[0]) * t, ga[1] + (gb[1] - ga[1]) * t, ga[2] + (gb[2] - ga[2]) * t});
    } else {
      normals[i] = normal({levels[f + 2] - levels[f + 1], levels[f + 4] - levels[f + 3], levels[f + 6] - levels[f + 5]});
    }
  }
  std::vector<field_surface_vertex> result;
  result.reserve(triangles.size());
  for (size_t i = 0; i < triangles.size(); i += 3) {
    auto a = triangles[i], b = triangles[i + 1], c = triangles[i + 2];
    auto face = cross(sub(intersections[b], intersections[a]), sub(intersections[c], intersections[a]));
    if (dot(face, face) < 1e-24) continue;
    if (dot(face, normals[a]) < 0) std::swap(b, c);
    for (auto v : {a, b, c})
      result.push_back({intersections[v], normals[v], tags[first_query[v]]});
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
  const auto& field = current_field(call.context(), call.params->string("field"));
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
  const auto plain = call.params->string("plain", "centred");
  if (plain != "centred" && plain != "kuhn") utils::error{}("originator step '{}': field_surface plain must be 'centred' or 'kuhn', got '{}'", call.step_name, plain);
  o.kuhn_plain = plain == "kuhn";
  const auto skip = call.params->integer("skip", 0);
  if (skip < 0 || skip > 128) utils::error{}("originator step '{}': field_surface skip must be 0..128 cells, got {}", call.step_name, skip);
  o.skip_block = uint32_t(skip);
  o.band = call.params->number("band", std::numeric_limits<double>::infinity());
  const auto normals = call.params->string("normals", "sampled");
  if (normals != "sampled" && normals != "lattice") utils::error{}("originator step '{}': field_surface normals must be 'sampled' or 'lattice', got '{}'", call.step_name, normals);
  o.lattice_normals = normals == "lattice";
  o.vertex_capacity = std::min({call.output(0).count(), call.output(1).count(), call.output(2).count()});
  std::vector<field_surface_vertex> result;
  try {
    result = make_field_surface(current_field(call.context(), call.params->string("field")), o, boxes);
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
