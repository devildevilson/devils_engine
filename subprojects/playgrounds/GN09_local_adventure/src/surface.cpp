#include "surface.h"
#include "local_structure.h"
#include "station.h"
#include "tunnel.h"
#include "world_chain.h"

#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/utils/core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string_view>

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
  return generate_surface(prepare_chunk_field(world, volume.x, volume.y, volume.z), volume);
}

surface_mesh generate_surface(const field_region& region, const chunk& volume) {
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
    return density(region, add(origin, {double(x), double(y), double(z)}));
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
        const size_t first_vertex = mesh.size();
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
        // Порода клетки — вид полости, чьё слагаемое поля даёт минимум у середины её
        // поверхности. Граница видов идёт по клеткам: там, где начинается другая пещера.
        if (mesh.size() > first_vertex) {
          point middle{};
          for (size_t i = first_vertex; i < mesh.size(); ++i) middle = add(middle, mesh[i].position);
          middle = add(origin, scale(middle, 1.0 / double(mesh.size() - first_vertex)));
          const uint8_t shade = uint8_t(rock_at(region, middle));
          for (size_t i = first_vertex; i < mesh.size(); ++i) mesh[i].shade = shade;
        }
      }
    }
  }
  return mesh;
}

namespace {

namespace originator = devils_engine::originator;

originator::instance_key key_at(const originator::const_field_accessor& keys, const size_t row) {
  return {{originator::read_key_word(keys, row, 0), originator::read_key_word(keys, row, 1),
           originator::read_key_word(keys, row, 2)}};
}

// Блок генератора chunk_field: треугольники станций якорей чанка (без проёмов их туннеля) и туннелей
// чанка, обрезанные по коробке чанка [low, high). Станции и туннели — экземпляры цепочки по ключам
// (вход 0 — станции якорей, 1 — туннели, 2 — зерно мира); их же поле чанка взяло fan-in-ом.
void tool_structure_surface(const originator::tool_call& call, const size_t, const size_t) {
  const auto stations = call.input(0).read(), tunnels = call.input(1).read();
  const uint64_t seed = originator::read_key_word(call.input(2).read(), 0);
  auto& chain = world_chain(call.params->number("clearance"));
  const double side = call.params->number("cells");
  const point low{call.params->number("x") * side, call.params->number("y") * side, call.params->number("z") * side};
  const point high{low.x + side, low.y + side, low.z + side};
  auto position = call.output(0).write(), normal = call.output(1).write(), shade = call.output(2).write();
  size_t made = 0;
  const auto emit = [&](const point* polygon, const size_t count, const point n, const uint8_t material) {
    for (size_t i = 1; i + 1 < count; ++i) {
      for (const point& p : {polygon[0], polygon[i], polygon[i + 1]}) {
        if (made >= position.count()) {
          devils_engine::utils::error{}("GN09 structure_surface: more than {} vertices", position.count());
        }
        position.set(made, p.x, 0); position.set(made, p.y, 1); position.set(made, p.z, 2);
        normal.set(made, n.x, 0); normal.set(made, n.y, 1); normal.set(made, n.z, 2);
        shade.set(made, double(material));
        ++made;
      }
    }
  };
  for (int64_t i = 0; i < call.params->integer("anchors"); ++i) {
    const auto key = key_at(stations, size_t(i));
    const auto station = station_view(*chain.get("station", seed, key));
    if (!station || !station_reaches(*station, low, high)) continue;
    // Проём туннеля: треугольники стены станции внутри его ноги не выводятся.
    const auto tunnel = tunnel_of_station(chain, seed, key);
    const uint64_t anchor = key.words[2];
    clip_triangles(station->mesh, low, high, emit, [&](const station_triangle& t) {
      return tunnel && in_tunnel_doorway(*tunnel, anchor, scale(add(add(t.a, t.b), t.c), 1.0 / 3.0));
    });
  }
  for (int64_t i = 0; i < call.params->integer("tunnels"); ++i) {
    const auto tunnel = tunnel_view(*chain.get("tunnel", seed, key_at(tunnels, size_t(i))));
    if (!tunnel || !tunnel_reaches(*tunnel, low, high)) continue;
    clip_triangles(tunnel->mesh, low, high, emit);
  }
  call.output(3).write().set(0, double(made));
}

} // namespace

void add_surface_tools(devils_engine::originator::tool_registry& tools) {
  tools.add(originator::tool_description{.name = "structure_surface", .shape = originator::aperture::sequential,
                                         .input_count = 3, .output_count = 4, .body = tool_structure_surface,
                                         .footprint = originator::no_temporary_memory});
}

surface_mesh surface_from_chain(const chunk_instance& made) {
  surface_mesh mesh;
  const auto& instance = *made.instance;
  const auto counts = [&](const std::string_view name) { return size_t(instance.field("surface_counts", name).get(0)); };
  const point origin{double(made.volume.x * chunk::cells), double(made.volume.y * chunk::cells),
                     double(made.volume.z * chunk::cells)};
  const auto read = [](const originator::const_field_accessor& f, const size_t i) {
    return point{f.get(i, 0), f.get(i, 1), f.get(i, 2)};
  };
  {
    const auto position = instance.field("surface", "position"), normal = instance.field("surface", "normal"),
               shade = instance.field("surface", "shade");
    const size_t count = counts("vertices");
    mesh.reserve(count + counts("structure"));
    for (size_t i = 0; i < count; ++i) {
      mesh.push_back({subtract(read(position, i), origin), read(normal, i), uint8_t(shade.get(i))});
    }
  }
  const auto position = instance.field("structure_surface", "position"),
             normal = instance.field("structure_surface", "normal"), shade = instance.field("structure_surface", "shade");
  for (size_t i = 0, count = counts("structure"); i < count; ++i) {
    mesh.push_back({subtract(read(position, i), origin), read(normal, i), uint8_t(shade.get(i))});
  }
  return mesh;
}

void append_outpost_surface(const adventure& world, const chunk& volume, surface_mesh& mesh) {
  constexpr double side = chunk::cells;
  const point chunk_low{double(volume.x) * side, double(volume.y) * side,
                        double(volume.z) * side};
  const point chunk_high{chunk_low.x + side, chunk_low.y + side, chunk_low.z + side};
  const auto emit = [&](const point* polygon, const size_t count, const point normal, const uint8_t shade) {
    for (size_t i = 1; i + 1 < count; ++i) {
      mesh.push_back({subtract(polygon[0], chunk_low), normal, shade});
      mesh.push_back({subtract(polygon[i], chunk_low), normal, shade});
      mesh.push_back({subtract(polygon[i + 1], chunk_low), normal, shade});
    }
  };
  for (const station_anchor& at : station_anchors(world)) {
    if (std::abs(at.centre.x - chunk_low.x) > station_reach + side ||
        std::abs(at.centre.y - chunk_low.y) > station_reach + side ||
        std::abs(at.centre.z - chunk_low.z) > station_reach + side) continue;
    const auto station = station_for(world, at);
    if (!station || !station_reaches(*station, chunk_low, chunk_high)) continue;
    // Проём туннеля: треугольники стены станции внутри его ноги не выводятся.
    const auto tunnel = tunnel_for(world, at);
    clip_triangles(station->mesh, chunk_low, chunk_high, emit, [&](const station_triangle& t) {
      return tunnel && in_tunnel_doorway(*tunnel, at.key, scale(add(add(t.a, t.b), t.c), 1.0 / 3.0));
    });
  }
  for (const auto& tunnel : tunnels_near(world, chunk_low, chunk_high)) {
    clip_triangles(tunnel->mesh, chunk_low, chunk_high, emit);
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

// Швы станции: каждый треугольник её сетки разрезается по чанкам, и куски всех
// чанков вместе должны иметь ровно ту же площадь — ни один не потерян на грани и
// не попал в два соседних чанка (грань на верхней плоскости достаётся соседу).
surface_seam_result verify_outpost_surface_seam(const adventure& world) {
  const auto area = [](const point a, const point b, const point c) {
    const point ab = subtract(b, a), ac = subtract(c, a);
    const point n{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
    return 0.5 * std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  };
  size_t triangles = 0, pieces = 0;
  const auto conserved = [&](const std::vector<station_triangle>& mesh, const point box_low, const point box_high) {
    double whole = 0.0, cut = 0.0;
    for (const station_triangle& t : mesh) whole += area(t.a, t.b, t.c);
    constexpr double side = chunk::cells;
    for (int z = int(std::floor(box_low.z / side)); z <= int(std::floor(box_high.z / side)); ++z) {
      for (int y = int(std::floor(box_low.y / side)); y <= int(std::floor(box_high.y / side)); ++y) {
        for (int x = int(std::floor(box_low.x / side)); x <= int(std::floor(box_high.x / side)); ++x) {
          const point low{x * side, y * side, z * side};
          clip_triangles(mesh, low, {low.x + side, low.y + side, low.z + side},
            [&](const point* polygon, const size_t count, point, uint8_t) {
              ++pieces;
              for (size_t i = 1; i + 1 < count; ++i) cut += area(polygon[0], polygon[i], polygon[i + 1]);
            });
        }
      }
    }
    triangles += mesh.size();
    return std::abs(whole - cut) <= 1e-6 * std::max(1.0, whole);
  };
  std::vector<uint64_t> seen_tunnels;
  for (const station_anchor& at : station_anchors(world)) {
    const auto station = station_for(world, at);
    if (!station) continue;
    if (!conserved(station->mesh, station->low, station->high)) {
      return {false, "station mesh area changes when cut by chunks", triangles, pieces};
    }
    const auto tunnel = tunnel_for(world, at);
    if (!tunnel || std::find(seen_tunnels.begin(), seen_tunnels.end(), tunnel->key) != seen_tunnels.end()) continue;
    seen_tunnels.push_back(tunnel->key);
    if (!conserved(tunnel->mesh, tunnel->low, tunnel->high)) {
      return {false, "tunnel mesh area changes when cut by chunks", triangles, pieces};
    }
  }
  if (triangles == 0) return {false, "no station mesh to cut by chunks"};
  return {true, {}, triangles, pieces};
}

} // namespace gn09
