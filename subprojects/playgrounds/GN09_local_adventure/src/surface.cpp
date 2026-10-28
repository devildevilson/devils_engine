#include "surface.h"
#include "local_structure.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>

namespace gn09 {
namespace {

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double s) { return {a.x * s, a.y * s, a.z * s}; }

constexpr std::array<std::array<int, 3>, 8> corners{{
  {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
}};
constexpr std::array<std::array<int, 4>, 6> tetrahedra{{
  {0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6},
  {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}
}};

size_t sample_index(const int x, const int y, const int z) {
  constexpr int side = chunk::cells + 1;
  return size_t((z * side + y) * side + x);
}

point normalized(const point p) {
  const double size = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
  return size > 0.0 ? scale(p, 1.0 / size) : point{0, 1, 0};
}

} // namespace

surface_mesh generate_surface(const adventure& world, const chunk& volume) {
  constexpr int cells = chunk::cells;
  const bool any_free = std::any_of(volume.samples.begin(), volume.samples.end(),
                                    [](const double v) { return v < 0.0; });
  const bool any_rock = std::any_of(volume.samples.begin(), volume.samples.end(),
                                    [](const double v) { return v >= 0.0; });
  if (!any_free || !any_rock) return {};
  const point origin{double(volume.x * cells), double(volume.y * cells),
                     double(volume.z * cells)};
  // Градиенты считаются в мировых координатах. Внутри чанка используются готовые сэмплы,
  // на крайнем слое — те же значения поля в соседнем узле, как у соседнего чанка.
  std::vector<point> gradients(size_t((cells + 1) * (cells + 1) * (cells + 1)));
  const auto value = [&](const int x, const int y, const int z) {
    if (x >= 0 && x <= cells && y >= 0 && y <= cells && z >= 0 && z <= cells) {
      return volume.at(x, y, z);
    }
    return density(world, add(origin, {double(x), double(y), double(z)}));
  };
  for (int z = 0; z <= cells; ++z) {
    for (int y = 0; y <= cells; ++y) {
      for (int x = 0; x <= cells; ++x) {
        gradients[sample_index(x, y, z)] = normalized({value(x + 1, y, z) - value(x - 1, y, z),
                                                       value(x, y + 1, z) - value(x, y - 1, z),
                                                       value(x, y, z + 1) - value(x, y, z - 1)});
      }
    }
  }

  surface_mesh mesh;
  for (int z = 0; z < cells; ++z) {
    for (int y = 0; y < cells; ++y) {
      for (int x = 0; x < cells; ++x) {
        std::array<point, 8> positions{};
        std::array<point, 8> normals{};
        std::array<double, 8> values{};
        for (int i = 0; i < 8; ++i) {
          const int sx = x + corners[i][0], sy = y + corners[i][1], sz = z + corners[i][2];
          positions[i] = add(origin, {double(sx), double(sy), double(sz)});
          normals[i] = gradients[sample_index(sx, sy, sz)];
          values[i] = volume.at(sx, sy, sz);
        }
        const auto crossing = [&](int a, int b) {
          // Канонический порядок мировых концов ребра даёт одинаковое округление на
          // общей грани двух чанков, независимо от обхода тетраэдра.
          const point pa = positions[a], pb = positions[b];
          if (pa.x > pb.x || (pa.x == pb.x && pa.y > pb.y) ||
              (pa.x == pb.x && pa.y == pb.y && pa.z > pb.z)) std::swap(a, b);
          const double t = values[a] / (values[a] - values[b]);
          return surface_vertex{subtract(add(positions[a], scale(subtract(positions[b], positions[a]), t)),
                                         origin),
                                normalized(add(scale(normals[a], 1.0 - t), scale(normals[b], t)))};
        };
        const auto triangle = [&](const surface_vertex a, const surface_vertex b,
                                  const surface_vertex c) {
          mesh.push_back(a);
          mesh.push_back(b);
          mesh.push_back(c);
        };
        for (const auto& tet : tetrahedra) {
          std::array<int, 4> inside{}, outside{};
          int in_count = 0, out_count = 0;
          for (const int corner : tet) {
            if (values[corner] < 0.0) inside[in_count++] = corner;
            else outside[out_count++] = corner;
          }
          if (in_count == 1) {
            triangle(crossing(inside[0], outside[0]), crossing(inside[0], outside[1]),
                     crossing(inside[0], outside[2]));
          } else if (in_count == 3) {
            triangle(crossing(outside[0], inside[0]), crossing(outside[0], inside[2]),
                     crossing(outside[0], inside[1]));
          } else if (in_count == 2) {
            const auto a = crossing(inside[0], outside[0]);
            const auto b = crossing(inside[0], outside[1]);
            const auto c = crossing(inside[1], outside[0]);
            const auto d = crossing(inside[1], outside[1]);
            triangle(a, b, c);
            triangle(b, d, c);
          }
        }
      }
    }
  }
  return mesh;
}

void append_outpost_surface(const adventure& world, const chunk& volume, surface_mesh& mesh) {
  constexpr double side = chunk::cells;
  const point chunk_low{double(volume.x) * side, double(volume.y) * side,
                        double(volume.z) * side};
  const point chunk_high{chunk_low.x + side, chunk_low.y + side, chunk_low.z + side};
  const auto component = [](const point p, const uint32_t axis) {
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
  };
  for (const node& at : world.route.nodes) {
    if (!is_base(at.kind)) continue;
    if (at.centre.x < chunk_low.x - 64.0 || at.centre.x > chunk_high.x + 64.0 ||
        at.centre.y < chunk_low.y - 32.0 || at.centre.y > chunk_high.y + 32.0 ||
        at.centre.z < chunk_low.z - 64.0 || at.centre.z > chunk_high.z + 64.0) continue;
    const local_structure structure = materialize_local_structure(world, at.id);
    for (const built_solid& solid : fit_outpost_solids(world, structure)) {
      const point first = structure_to_world(structure, double(solid.footprint.u0) / 100.0,
                                             double(solid.y0) / 100.0,
                                             double(solid.footprint.v0) / 100.0);
      const point last = structure_to_world(structure, double(solid.footprint.u1) / 100.0,
                                            double(solid.y1) / 100.0,
                                            double(solid.footprint.v1) / 100.0);
      const point low{std::min(first.x, last.x), std::min(first.y, last.y),
                      std::min(first.z, last.z)};
      const point high{std::max(first.x, last.x), std::max(first.y, last.y),
                       std::max(first.z, last.z)};
      if (low.x >= chunk_high.x || high.x <= chunk_low.x ||
          low.y >= chunk_high.y || high.y <= chunk_low.y ||
          low.z >= chunk_high.z || high.z <= chunk_low.z) continue;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const uint32_t a = (axis + 1) % 3, b = (axis + 2) % 3;
        const double a0 = std::max(component(low, a), component(chunk_low, a));
        const double a1 = std::min(component(high, a), component(chunk_high, a));
        const double b0 = std::max(component(low, b), component(chunk_low, b));
        const double b1 = std::min(component(high, b), component(chunk_high, b));
        if (a0 >= a1 || b0 >= b1) continue;
        for (uint32_t side_id = 0; side_id < 2; ++side_id) {
          const double plane = component(side_id == 0 ? low : high, axis);
          if (plane < component(chunk_low, axis) || plane >= component(chunk_high, axis)) continue;
          const double sign = side_id == 0 ? -1.0 : 1.0;
          const auto vertex = [&](const double av, const double bv) {
            point p{};
            point n{};
            if (axis == 0) { p = {plane, av, bv}; n.x = sign; }
            else if (axis == 1) { p = {bv, plane, av}; n.y = sign; }
            else { p = {av, bv, plane}; n.z = sign; }
            return surface_vertex{subtract(p, chunk_low), n, solid.shade};
          };
          const surface_vertex p0 = vertex(a0, b0), p1 = vertex(a1, b0);
          const surface_vertex p2 = vertex(a1, b1), p3 = vertex(a0, b1);
          mesh.insert(mesh.end(), {p0, p1, p2, p0, p2, p3});
        }
      }
    }
  }
}

surface_seam_result verify_surface_seam(const adventure& world) {
  struct key { int x, y, z; };
  const auto chunk_of = [](const point p) {
    return key{int(std::floor(p.x / chunk::cells)), int(std::floor(p.y / chunk::cells)),
               int(std::floor(p.z / chunk::cells))};
  };
  for (const edge link : world.route.edges) {
    const point a = world.route.nodes[link.a].centre;
    const point b = world.route.nodes[link.b].centre;
    const point delta = subtract(b, a);
    const int steps = int(std::ceil(std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) * 4));
    key previous = chunk_of(a);
    for (int i = 1; i <= steps; ++i) {
      const key next = chunk_of(add(a, scale(delta, double(i) / double(steps))));
      const int dx = next.x - previous.x, dy = next.y - previous.y, dz = next.z - previous.z;
      if (std::abs(dx) + std::abs(dy) + std::abs(dz) == 1) {
        const chunk first = generate_chunk(world, previous.x, previous.y, previous.z);
        const chunk second = generate_chunk(world, next.x, next.y, next.z);
        const surface_mesh left = generate_surface(world, first);
        const surface_mesh right = generate_surface(world, second);
        const int axis = dx != 0 ? 0 : (dy != 0 ? 1 : 2);
        const double plane = double((axis == 0 ? std::max(previous.x, next.x) :
                                     axis == 1 ? std::max(previous.y, next.y) :
                                                 std::max(previous.z, next.z)) * chunk::cells);
        const auto on_face = [&](const surface_mesh& mesh, const key at) {
          std::map<std::pair<int64_t, int64_t>, std::array<int, 3>> vertices;
          const point origin{double(at.x * chunk::cells), double(at.y * chunk::cells),
                             double(at.z * chunk::cells)};
          for (const surface_vertex& vertex : mesh) {
            const point p = add(vertex.position, origin);
            const double coordinate = axis == 0 ? p.x : axis == 1 ? p.y : p.z;
            if (std::abs(coordinate - plane) > 1e-8) continue;
            const double u = axis == 0 ? p.y : p.x;
            const double v = axis == 2 ? p.y : p.z;
            vertices.emplace(
              std::pair{int64_t(std::llround(u * 1000000.0)),
                        int64_t(std::llround(v * 1000000.0))},
              std::array{int(std::lround(vertex.normal.x * 127.0)),
                         int(std::lround(vertex.normal.y * 127.0)),
                         int(std::lround(vertex.normal.z * 127.0))});
          }
          return vertices;
        };
        const auto first_face = on_face(left, previous);
        const auto second_face = on_face(right, next);
        if (first_face.empty()) continue;
        if (first_face != second_face) {
          return {false, "surface positions or normals disagree on a shared chunk face",
                  (left.size() + right.size()) / 3, first_face.size()};
        }
        return {true, {}, (left.size() + right.size()) / 3, first_face.size()};
      }
      previous = next;
    }
  }
  return {false, "no surface crosses a sampled chunk face"};
}

surface_seam_result verify_outpost_surface_seam(const adventure& world) {
  for (const node& at : world.route.nodes) {
    if (!is_base(at.kind)) continue;
    const int cx = int(std::floor(at.centre.x / chunk::cells));
    const int cy = int(std::floor(at.centre.y / chunk::cells));
    const int cz = int(std::floor(at.centre.z / chunk::cells));
    for (int z = cz - 2; z <= cz + 2; ++z) {
      for (int y = cy - 1; y <= cy + 1; ++y) {
        for (int x = cx - 2; x <= cx + 2; ++x) {
          const chunk left{x, y, z, {}}, right{x + 1, y, z, {}};
          surface_mesh first, second;
          append_outpost_surface(world, left, first);
          append_outpost_surface(world, right, second);
          const double plane = double((x + 1) * chunk::cells);
          const auto border = [&](const surface_mesh& mesh, const chunk& owner) {
            std::set<std::array<int64_t, 6>> vertices;
            const point origin{double(owner.x * chunk::cells), double(owner.y * chunk::cells),
                               double(owner.z * chunk::cells)};
            for (const surface_vertex& vertex : mesh) {
              const point p = add(vertex.position, origin);
              if (std::abs(p.x - plane) > 1e-8 || std::abs(vertex.normal.x) > 0.5) continue;
              vertices.insert({int64_t(std::llround(p.y * 1000000.0)),
                               int64_t(std::llround(p.z * 1000000.0)),
                               int64_t(std::lround(vertex.normal.x * 127.0)),
                               int64_t(std::lround(vertex.normal.y * 127.0)),
                               int64_t(std::lround(vertex.normal.z * 127.0)),
                               vertex.shade});
            }
            return vertices;
          };
          const auto a = border(first, left), b = border(second, right);
          if (a.empty() && b.empty()) continue;
          if (a != b) return {false, "outpost mesh disagrees across chunk face",
                              (first.size() + second.size()) / 3, a.size()};
          return {true, {}, (first.size() + second.size()) / 3, a.size()};
        }
      }
    }
  }
  return {false, "no outpost mesh crosses a sampled chunk face"};
}

} // namespace gn09
