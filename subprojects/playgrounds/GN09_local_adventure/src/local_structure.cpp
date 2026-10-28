#include "local_structure.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "devils_engine/utils/prng.h"

namespace gn09 {
namespace {

constexpr double dock_opening_radius = starter_sub_path_radius + 1.25;

double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

bool interior_overlap(const plan_rect a, const plan_rect b) {
  return std::max(a.u0, b.u0) < std::min(a.u1, b.u1) &&
         std::max(a.v0, b.v0) < std::min(a.v1, b.v1);
}

double box_field(const point p, const point centre, const point half) {
  const point q{std::abs(p.x - centre.x) - half.x, std::abs(p.y - centre.y) - half.y,
                std::abs(p.z - centre.z) - half.z};
  return std::hypot(std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0)) +
         std::min(std::max({q.x, q.y, q.z}), 0.0);
}

double segment_distance_2d(const double u, const double v, const double au,
                           const double av, const double bu, const double bv) {
  const double du = bu - au, dv = bv - av;
  const double t = std::clamp(((u - au) * du + (v - av) * dv) / (du * du + dv * dv),
                              0.0, 1.0);
  return std::hypot(u - au - t * du, v - av - t * dv);
}

// Ось лодки параллельна локальной оси v; её боковой шлюз смотрит на единственный
// шлюз станции на стороне u=12 м. Параллелограмм — весь путь центра лодки от
// поворотного узла до места стыковки, включая её 11-метровый осевой отрезок.
double docking_sweep_field(const point local, const double berth_v,
                           const int8_t dock_axis) {
  if (dock_axis != 0) {
    const double y0 = dock_axis > 0 ? 0.0 : -5.0;
    const double y1 = dock_axis > 0 ? 5.0 : 0.0;
    const double y_gap = std::max({y0 - local.y, local.y - y1, 0.0});
    const double v_gap = std::max(std::abs(local.z) - starter_sub_axis_half, 0.0);
    return std::hypot(local.x, y_gap, v_gap) - starter_sub_path_radius;
  }
  constexpr double berth_u = 6.5;
  constexpr double axis_half = 5.5;
  constexpr double radius = starter_sub_path_radius;
  double horizontal = 0.0;
  const double t = local.x / berth_u;
  if (t < 0.0 || t > 1.0 ||
      std::abs(local.z - t * berth_v) > axis_half) {
    horizontal = std::min({
      segment_distance_2d(local.x, local.z, 0.0, -axis_half, 0.0, axis_half),
      segment_distance_2d(local.x, local.z, 0.0, axis_half,
                          berth_u, berth_v + axis_half),
      segment_distance_2d(local.x, local.z, berth_u, berth_v + axis_half,
                          berth_u, berth_v - axis_half),
      segment_distance_2d(local.x, local.z, berth_u, berth_v - axis_half,
                          0.0, -axis_half)});
  }
  return std::hypot(horizontal, local.y) - radius;
}

bool shared_wall_contains(const plan_rect a, const plan_rect b, const plan_rect opening) {
  if (opening.u0 == opening.u1 && opening.v0 < opening.v1) {
    const int32_t line = opening.u0;
    return ((a.u1 == line && b.u0 == line) || (b.u1 == line && a.u0 == line)) &&
           opening.v0 >= std::max(a.v0, b.v0) &&
           opening.v1 <= std::min(a.v1, b.v1);
  }
  if (opening.v0 == opening.v1 && opening.u0 < opening.u1) {
    const int32_t line = opening.v0;
    return ((a.v1 == line && b.v0 == line) || (b.v1 == line && a.v0 == line)) &&
           opening.u0 >= std::max(a.u0, b.u0) &&
           opening.u1 <= std::min(a.u1, b.u1);
  }
  return false;
}

} // namespace

int8_t preferred_dock_axis(const uint64_t seed, const node& at) {
  const uint64_t pose = devils_engine::utils::mix(seed, at.key, 0x444f434bull) % 10u;
  const cave_biome biome = biome_at(seed, at.centre);
  const uint64_t side_limit = biome == cave_biome::karst ? 6u :
                              biome == cave_biome::rift ? 4u : 2u;
  return pose < side_limit ? 0 :
         pose < side_limit + (10u - side_limit) / 2u ? 1 : -1;
}

local_structure materialize_local_structure(const adventure& world, const uint32_t node_id) {
  const site local = materialize_site(world, node_id);
  const node& at = world.route.nodes.at(node_id);
  local_structure result;
  result.node_id = node_id;
  result.kind = at.kind;
  result.centre = at.centre;

  if (!is_base(at.kind)) {
    for (uint32_t i = 1; i < local.rooms.size(); ++i) {
      const room& chamber = local.rooms[i];
      const uint64_t hash = devils_engine::utils::mix(world.seed, at.key, i, 0x524553ull);
      const double step = 0.3;
      const point centre{chamber.centre.x + (int(hash & 3u) - 1.5) * step,
                         chamber.centre.y - chamber.radius + 0.8,
                         chamber.centre.z + (int((hash >> 2) & 3u) - 1.5) * step};
      const char* kind = at.kind == theme::vent ? "thermal_crystal" :
                         (hash & 16u) ? "metal_ore" : "mineral_vein";
      result.deposits.push_back({i, centre, kind, 1u + uint32_t((hash >> 8) % 3u)});
    }
    return result;
  }

  result.dock_axis = preferred_dock_axis(world.seed, at);

  // Выбираем свободную четверть вокруг дока с учётом ВСЕХ близких маршрутов.
  // Маршрут может пройти рядом с базой, даже если не входит прямо в её порт.
  constexpr std::array<point, 4> directions{{{1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}}};
  const uint32_t first = uint32_t(devils_engine::utils::mix(world.seed, at.key, 0x535441ull) % 4u);
  double best = -1.0;
  int32_t chosen_offset = 750;
  if (world.outpost_layout.size() == world.route.nodes.size()) {
    const uint8_t choice = world.outpost_layout[node_id];
    result.dock_axis = choice / 12u == 0 ? 0 : choice / 12u == 1 ? 1 : -1;
    const uint8_t plan = choice % 12u;
    result.forward = directions[plan / 3u];
    chosen_offset = plan % 3u == 0 ? 750 : plan % 3u == 1 ? -750 : 0;
  } else for (uint32_t index = 0; index < 4; ++index) {
    const point candidate = directions[(first + index) % 4];
    const point lateral{-candidate.z, 0, candidate.x};
    for (const int32_t offset : {750, -750, 0}) {
      double clearance = 1e30;
      for (const portal& port : local.portals) {
        const point ray = subtract(port.centre, at.centre);
        const double toward = dot(ray, candidate);
        if (toward <= 0.0) continue;
        const double crossing = 12.0 * dot(ray, lateral) / toward;
        if (std::abs(crossing - double(offset) / 100.0) < 8.5) clearance = -1e30;
      }
      if (clearance < 0.0) continue;
      for (const double u : {14.0, 20.0, 30.0}) {
        for (const double v : {double(offset) / 100.0 - 4.5,
                               double(offset) / 100.0,
                               double(offset) / 100.0 + 4.5}) {
          const point probe{at.centre.x + candidate.x * u + lateral.x * v,
                            at.centre.y,
                            at.centre.z + candidate.z * u + lateral.z * v};
          for (const edge link : world.route.edges) {
            const point a = world.route.nodes[link.a].centre;
            const point b = world.route.nodes[link.b].centre;
            if (std::abs(a.x - at.centre.x) > 80.0 &&
                std::abs(b.x - at.centre.x) > 80.0) continue;
            if (std::abs(a.z - at.centre.z) > 80.0 &&
                std::abs(b.z - at.centre.z) > 80.0) continue;
            const point ab = subtract(b, a);
            const double t = std::clamp(dot(subtract(probe, a), ab) / dot(ab, ab), 0.0, 1.0);
            const point near{a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t};
            const point gap = subtract(probe, near);
            clearance = std::min(clearance, std::sqrt(dot(gap, gap)));
          }
        }
      }
      if (clearance > best) {
        best = clearance;
        result.forward = candidate;
        chosen_offset = offset;
      }
    }
  }

  result.level_cm = int32_t(result.dock_axis) * 1575;

  const point right{-result.forward.z, 0, result.forward.x};
  // Пересекаем запас проезда лодки с каждой стороной дока. Косой маршрут у угла
  // открывает обе стороны; один вход графа тогда представлен двумя отрезками стены.
  constexpr int32_t step = 25;
  for (const portal& port : local.portals) {
    const point delta = subtract(port.centre, at.centre);
    const double u = dot(delta, result.forward), v = dot(delta, right);
    const double horizontal = std::hypot(u, v);
    for (uint32_t side = 0; side < 4; ++side) {
      int32_t begin = INT32_MAX;
      const auto flush = [&](const int32_t end) {
        if (begin == INT32_MAX) return;
        const int32_t line = side % 2u == 0 ? -1200 : 1200;
        const plan_rect span = side < 2 ? plan_rect{line, begin, line, end}
                                        : plan_rect{begin, line, end, line};
        result.ports.push_back({port.edge_index, span});
        begin = INT32_MAX;
      };
      for (int32_t along = -1200; along < 1200; along += step) {
        const double local_u = side < 2 ? double(side == 0 ? -1200 : 1200) : double(along + step / 2);
        const double local_v = side < 2 ? double(along + step / 2) : double(side == 2 ? -1200 : 1200);
        const double projection = (local_u * u + local_v * v) / horizontal;
        const double separation = projection > 0.0
          ? std::abs(local_u * v - local_v * u) / horizontal
          : std::hypot(local_u, local_v);
        if (separation <= dock_opening_radius * 100.0) {
          if (begin == INT32_MAX) begin = along;
        } else flush(along);
      }
      flush(1200);
    }
  }

  const int32_t offset_v = result.dock_axis == 0 ? chosen_offset : 0;

  // 0: мокрый док; 1: шлюз; 2: технический коридор; 3..: модули.
  // Смежность и проёмы задаются точными сантиметровыми отрезками как у GN07.
  const uint32_t modules = uint32_t(local.rooms.size() - 1);
  const uint32_t slots = (modules + 1u) / 2u;
  const int32_t spine_begin = result.dock_axis == 0 ? 1600 : 400;
  const int32_t end = spine_begin + int32_t(slots) * 600;
  result.zones.push_back({{-1200, -1200, 1200, 1200}, "wet_dock"});
  result.zones.push_back({result.dock_axis == 0
    ? plan_rect{1200, offset_v - 200, 1600, offset_v + 200}
    : plan_rect{-400, -400, 400, 400}, "airlock"});
  result.zones.push_back({{spine_begin, offset_v - 200, end, offset_v + 200}, "spine"});
  if (result.dock_axis == 0) {
    result.openings.push_back({0, 1, {1200, offset_v - 125, 1200, offset_v + 125}});
  }
  result.openings.push_back({1, 2, {spine_begin, offset_v - 125,
                                     spine_begin, offset_v + 125}});
  for (uint32_t i = 0; i < modules; ++i) {
    const int32_t x0 = spine_begin + int32_t(i / 2u) * 600;
    const bool upper = i % 2u == 0;
    const int32_t z0 = offset_v + (upper ? 200 : -750);
    const int32_t z1 = offset_v + (upper ? 750 : -200);
    result.zones.push_back({{x0, z0, x0 + 600, z1}, local.rooms[i + 1].purpose});
    result.openings.push_back({2, i + 3u,
      {x0 + 200, offset_v + (upper ? 200 : -200),
       x0 + 400, offset_v + (upper ? 200 : -200)}});
  }
  return result;
}

std::string verify_local_structure(const adventure& world, const site& local,
                                   const local_structure& structure) {
  if (structure.node_id != local.node_id || structure.kind != local.kind ||
      structure.centre.x != local.centre.x || structure.centre.y != local.centre.y ||
      structure.centre.z != local.centre.z) return "local structure disagrees with site";
  if (!is_base(local.kind)) {
    if (!structure.zones.empty() || !structure.openings.empty() || !structure.ports.empty() ||
        structure.deposits.size() != local.rooms.size() - 1) {
      return "resource cave has an invalid deposit plan";
    }
    for (const resource_deposit& deposit : structure.deposits) {
      if (deposit.room == 0 || deposit.room >= local.rooms.size() || deposit.amount == 0 ||
          deposit.kind.empty()) return "resource deposit lacks a chamber or yield";
      const point delta = subtract(deposit.centre, local.rooms[deposit.room].centre);
      if (dot(delta, delta) > local.rooms[deposit.room].radius * local.rooms[deposit.room].radius ||
          density(world, deposit.centre) >= 0.0) return "resource deposit lies outside its chamber";
    }
    return {};
  }
  if (!structure.deposits.empty() || structure.zones.size() != local.rooms.size() + 2 ||
      structure.ports.size() < local.portals.size() ||
      structure.openings.size() != structure.zones.size() -
                                   (structure.dock_axis == 0 ? 1u : 2u)) {
    return "outpost has an invalid zone or opening count";
  }
  if ((structure.dock_axis == 0 && structure.level_cm != 0) ||
      (structure.dock_axis != 0 &&
       (std::abs(structure.dock_axis) != 1 ||
        structure.level_cm != int32_t(structure.dock_axis) * 1575 ||
        structure.zones[1].box.u0 != -400 || structure.zones[1].box.u1 != 400 ||
        structure.zones[1].box.v0 != -400 || structure.zones[1].box.v1 != 400))) {
    return "outpost has an invalid docking face";
  }
  for (uint32_t i = 0; i < structure.zones.size(); ++i) {
    const plan_rect a = structure.zones[i].box;
    if (a.u0 >= a.u1 || a.v0 >= a.v1 || structure.zones[i].purpose.empty()) {
      return "outpost has an invalid rectangle";
    }
    for (uint32_t j = 0; j < i; ++j) {
      if (structure.dock_axis != 0 && j == 0) continue; // док ниже/выше построенного этажа
      if (interior_overlap(a, structure.zones[j].box)) return "outpost zones overlap";
    }
  }
  std::vector<uint8_t> reached(structure.zones.size(), 0);
  reached[structure.dock_axis == 0 ? 0 : 1] = 1;
  for (const built_opening& opening : structure.openings) {
    if (opening.a >= structure.zones.size() || opening.b >= structure.zones.size() ||
        opening.a == opening.b ||
        !shared_wall_contains(structure.zones[opening.a].box,
                              structure.zones[opening.b].box, opening.span)) {
      return "outpost opening does not lie on a shared wall";
    }
  }
  const plan_rect dock = structure.zones[0].box;
  const point right{-structure.forward.z, 0, structure.forward.x};
  for (const portal& port : local.portals) {
    const auto found = std::find_if(structure.ports.begin(), structure.ports.end(),
      [&](const built_port& item) { return item.edge_index == port.edge_index; });
    if (found == structure.ports.end()) {
      return "outpost dock port disagrees with a graph link";
    }
  }
  for (const built_port& built : structure.ports) {
    const auto found = std::find_if(local.portals.begin(), local.portals.end(),
      [&](const portal& item) { return item.edge_index == built.edge_index; });
    if (found == local.portals.end()) return "outpost dock port disagrees with a graph link";
    const plan_rect span = built.span;
    const bool vertical = span.u0 == span.u1 &&
      (span.u0 == dock.u0 || span.u0 == dock.u1) &&
      span.v0 >= dock.v0 && span.v1 <= dock.v1 && span.v1 > span.v0;
    const bool horizontal = span.v0 == span.v1 &&
      (span.v0 == dock.v0 || span.v0 == dock.v1) &&
      span.u0 >= dock.u0 && span.u1 <= dock.u1 && span.u1 > span.u0;
    const point direction = subtract(found->centre, structure.centre);
    const double u = dot(direction, structure.forward), v = dot(direction, right);
    const double midpoint_u = double(span.u0 + span.u1) / 200.0;
    const double midpoint_v = double(span.v0 + span.v1) / 200.0;
    const double ray_distance = std::abs(midpoint_u * v - midpoint_v * u) / std::hypot(u, v);
    if ((!vertical && !horizontal) || midpoint_u * u + midpoint_v * v <= 0.0 ||
        ray_distance > dock_opening_radius + 0.2) {
      return "outpost dock port disagrees with a graph link";
    }
  }
  for (size_t pass = 0; pass < structure.zones.size(); ++pass) {
    for (const built_opening& opening : structure.openings) {
      if (reached[opening.a]) reached[opening.b] = 1;
      if (reached[opening.b]) reached[opening.a] = 1;
    }
  }
  if (std::find(reached.begin() + (structure.dock_axis == 0 ? 0 : 1),
                reached.end(), 0) != reached.end()) {
    return "outpost has an unreachable module";
  }
  return {};
}

point structure_to_world(const local_structure& structure, const double u,
                         const double y, const double v) {
  const point right{-structure.forward.z, 0, structure.forward.x};
  return {structure.centre.x + u * structure.forward.x + v * right.x,
          structure.centre.y + y,
          structure.centre.z + u * structure.forward.z + v * right.z};
}

point docking_axis_point(const local_structure& structure, const double progress,
                         const double along) {
  if (structure.dock_axis != 0) {
    return structure_to_world(structure, 0, 5.0 * structure.dock_axis * progress, along);
  }
  const double berth_v = double(structure.zones[1].box.v0 + 200) / 100.0;
  return structure_to_world(structure, 6.5 * progress, 0, berth_v * progress + along);
}

double outpost_envelope_field(const local_structure& structure, const point position) {
  if (!is_base(structure.kind)) return 3.0;
  const point delta = subtract(position, structure.centre);
  const point right{-structure.forward.z, 0, structure.forward.x};
  const point local{dot(delta, structure.forward), delta.y, dot(delta, right)};
  const double berth_v = double(structure.zones[1].box.v0 + 200) / 100.0;
  double field = docking_sweep_field(local, berth_v, structure.dock_axis);
  // Док — природная полость вокруг узла и линии стыковки, а не вырезанный
  // прямоугольный ангар. Для жилых зон вырезаем внутренность, а наружные
  // стены, пол и крыша заходят в скалу на толщину своей обшивки.
  for (size_t i = 1; i < structure.zones.size(); ++i) {
    const plan_rect box = structure.zones[i].box;
    const double u0 = double(box.u0) / 100.0 + 0.12;
    const double u1 = double(box.u1) / 100.0 - 0.12;
    const double v0 = double(box.v0) / 100.0 + 0.12;
    const double v1 = double(box.v1) / 100.0 - 0.12;
    field = std::min(field, box_field(local,
      {(u0 + u1) / 2.0, double(structure.level_cm) / 100.0, (v0 + v1) / 2.0},
      {(u1 - u0) / 2.0, 4.5, (v1 - v0) / 2.0}));
  }
  if (structure.dock_axis != 0) {
    // Короткая шахта соединяет вертикальный люк со свободным объёмом лодки.
    field = std::min(field, box_field(local,
      {0, 11.5 * structure.dock_axis, 0}, {3.6, 3.5, 3.6}));
  }
  // Внутренности зон чуть утоплены в стену. Отдельные короткие вырезы на
  // точных люках открывают сквозной проход, не раздвигая всю скалу от корпуса.
  for (const built_opening& opening : structure.openings) {
    const plan_rect span = opening.span;
    if (span.u0 == span.u1) {
      const double u = double(span.u0) / 100.0;
      const double v0 = double(span.v0) / 100.0;
      const double v1 = double(span.v1) / 100.0;
      field = std::min(field, box_field(local,
                                       {u, double(structure.level_cm) / 100.0,
                                        (v0 + v1) / 2.0},
                                       {1.25, 4.5, (v1 - v0) / 2.0 + 0.35}));
    } else {
      const double v = double(span.v0) / 100.0;
      const double u0 = double(span.u0) / 100.0;
      const double u1 = double(span.u1) / 100.0;
      field = std::min(field, box_field(local,
                                       {(u0 + u1) / 2.0,
                                        double(structure.level_cm) / 100.0, v},
                                       {(u1 - u0) / 2.0 + 0.35, 4.5, 1.25}));
    }
  }
  return field;
}

std::vector<built_solid> materialize_outpost_solids(const local_structure& structure) {
  std::vector<built_solid> solids;
  if (!is_base(structure.kind)) return solids;
  const uint8_t wall_shade = structure.kind == theme::derelict ? 115 : 123;
  const uint8_t floor_shade = structure.kind == theme::derelict ? 112 : 119;
  for (uint32_t i = 0; i < structure.zones.size(); ++i) {
    if (i == 0) continue; // мокрый док — сама пещера, без квадратного ограждения
    const plan_rect zone = structure.zones[i].box;
    const int32_t bottom = structure.level_cm - 300;
    const int32_t top = structure.level_cm + 300;
    const auto slab = [&](const int32_t y0, const int32_t y1,
                          const bool hatch) {
      if (!hatch) {
        solids.push_back({zone, y0, y1, floor_shade});
        return;
      }
      // Вертикальный единственный шлюз: отверстие 6x6 м сквозь толстое
      // перекрытие, четыре полосы рамки лежат в одной плоскости.
      constexpr int32_t r = 300;
      const uint8_t frame_shade = 126;
      solids.push_back({{zone.u0, zone.v0, -r, zone.v1}, y0, y1, frame_shade});
      solids.push_back({{r, zone.v0, zone.u1, zone.v1}, y0, y1, frame_shade});
      solids.push_back({{-r, zone.v0, r, -r}, y0, y1, frame_shade});
      solids.push_back({{-r, r, r, zone.v1}, y0, y1, frame_shade});
    };
    const int32_t embed = structure.dock_axis == 0 ? 60 : 240;
    slab(bottom - embed, bottom + 40, i == 1 && structure.dock_axis > 0);
    slab(top - 40, top + embed, i == 1 && structure.dock_axis < 0);
    for (uint32_t side = 0; side < 4; ++side) {
      const bool u_side = side < 2;
      const int32_t line = side == 0 ? zone.u0 : side == 1 ? zone.u1 :
                           side == 2 ? zone.v0 : zone.v1;
      const int32_t start = u_side ? zone.v0 : zone.u0;
      const int32_t finish = u_side ? zone.v1 : zone.u1;
      std::vector<std::pair<int32_t, int32_t>> gaps;
      const auto add_gap = [&](const plan_rect span) {
        if (u_side && span.u0 == line && span.u1 == line) {
          const int32_t a = std::max(start, span.v0), b = std::min(finish, span.v1);
          if (a < b) gaps.emplace_back(a, b);
        } else if (!u_side && span.v0 == line && span.v1 == line) {
          const int32_t a = std::max(start, span.u0), b = std::min(finish, span.u1);
          if (a < b) gaps.emplace_back(a, b);
        }
      };
      for (const built_opening& opening : structure.openings) {
        if (opening.a == i || opening.b == i) add_gap(opening.span);
      }
      std::sort(gaps.begin(), gaps.end());
      const auto emit = [&](const int32_t a, const int32_t b) {
        if (a >= b) return;
        const plan_rect footprint = side == 0 ? plan_rect{line, a, line + 40, b} :
                                    side == 1 ? plan_rect{line - 40, a, line, b} :
                                    side == 2 ? plan_rect{a, line, b, line + 40} :
                                                plan_rect{a, line - 40, b, line};
        const uint8_t shade = i == 1 && side == 0 && structure.dock_axis == 0
          ? 126 : wall_shade;
        solids.push_back({footprint, bottom + 40, top - 40, shade});
      };
      int32_t cursor = start;
      for (const auto [a, b] : gaps) {
        emit(cursor, std::max(cursor, a));
        cursor = std::max(cursor, b);
      }
      emit(cursor, finish);
    }
  }
  return solids;
}

std::string verify_outpost_solids(const adventure& world, const local_structure& structure,
                                  const std::vector<built_solid>& solids) {
  if (!is_base(structure.kind)) return solids.empty() ? std::string{} : "natural site has built solids";
  if (solids.empty()) return "outpost has no solids";
  for (const built_solid& solid : solids) {
    if (solid.footprint.u0 >= solid.footprint.u1 ||
        solid.footprint.v0 >= solid.footprint.v1 || solid.y0 >= solid.y1) {
      return "outpost has an invalid solid";
    }
    // Независимо проверяем, что материальная стенка не заняла объём лодки
    // между поворотным узлом и единственным стыковочным шлюзом.
    const double berth_v = double(structure.zones[1].box.v0 + 200) / 100.0;
    const double u0 = double(solid.footprint.u0) / 100.0;
    const double u1 = double(solid.footprint.u1) / 100.0;
    const double v0 = double(solid.footprint.v0) / 100.0;
    const double v1 = double(solid.footprint.v1) / 100.0;
    double docking_gap_squared = 0.0;
    if (structure.dock_axis != 0) {
      const double y0 = double(solid.y0) / 100.0;
      const double y1 = double(solid.y1) / 100.0;
      const double sweep_y0 = structure.dock_axis > 0 ? 0.0 : -5.0;
      const double sweep_y1 = structure.dock_axis > 0 ? 5.0 : 0.0;
      const double u_gap = std::max({u0, -u1, 0.0});
      const double v_gap = std::max({v0 - starter_sub_axis_half,
                                     -starter_sub_axis_half - v1, 0.0});
      const double y_gap = std::max({sweep_y0 - y1, y0 - sweep_y1, 0.0});
      docking_gap_squared = u_gap * u_gap + v_gap * v_gap + y_gap * y_gap;
    } else {
      const double y_gap = std::max({double(solid.y0) / 100.0,
                                     -double(solid.y1) / 100.0, 0.0});
      const auto berth_gap_squared = [&](const double t) {
        const double u = 6.5 * t, v = berth_v * t;
        const double u_gap = std::max({u0 - u, u - u1, 0.0});
        const double v_gap = std::max({v0 - v - starter_sub_axis_half,
                                       v - starter_sub_axis_half - v1, 0.0});
        return u_gap * u_gap + v_gap * v_gap + y_gap * y_gap;
      };
      double left = 0.0, right = 1.0;
      for (uint32_t step = 0; step < 24; ++step) {
        const double a = left + (right - left) / 3.0;
        const double b = right - (right - left) / 3.0;
        if (berth_gap_squared(a) < berth_gap_squared(b)) right = b;
        else left = a;
      }
      docking_gap_squared = std::min({berth_gap_squared(0.0), berth_gap_squared(1.0),
                                      berth_gap_squared((left + right) / 2.0)});
    }
    if (docking_gap_squared <
        (starter_sub_path_radius + 0.02) * (starter_sub_path_radius + 0.02)) {
      return "outpost solid intersects docking sweep";
    }
    const point first = structure_to_world(structure, double(solid.footprint.u0) / 100.0,
                                           double(solid.y0) / 100.0,
                                           double(solid.footprint.v0) / 100.0);
    const point last = structure_to_world(structure, double(solid.footprint.u1) / 100.0,
                                          double(solid.y1) / 100.0,
                                          double(solid.footprint.v1) / 100.0);
    const point low{std::min(first.x, last.x), std::min(first.y, last.y), std::min(first.z, last.z)};
    const point high{std::max(first.x, last.x), std::max(first.y, last.y), std::max(first.z, last.z)};
    const point mid{(low.x + high.x) / 2, (low.y + high.y) / 2, (low.z + high.z) / 2};
    const point half{(high.x - low.x) / 2, (high.y - low.y) / 2, (high.z - low.z) / 2};
    for (const node& at : world.route.nodes) {
      if (std::abs(at.centre.x - mid.x) > half.x + starter_sub_turn_radius ||
          std::abs(at.centre.y - mid.y) > half.y + starter_sub_turn_radius ||
          std::abs(at.centre.z - mid.z) > half.z + starter_sub_turn_radius) continue;
      if (box_field(at.centre, mid, half) < starter_sub_turn_radius + 0.01) {
        return "outpost solid intersects submarine turn at node " + std::to_string(at.id) +
               " beside base " + std::to_string(structure.node_id) + " at (" +
               std::to_string(mid.x) + "," + std::to_string(mid.y) + "," +
               std::to_string(mid.z) + ")";
      }
    }
    for (const edge link : world.route.edges) {
      const point a = world.route.nodes[link.a].centre;
      const point b = world.route.nodes[link.b].centre;
      if (std::max(a.x, b.x) < low.x - 5.2 || std::min(a.x, b.x) > high.x + 5.2 ||
          std::max(a.y, b.y) < low.y - 5.2 || std::min(a.y, b.y) > high.y + 5.2 ||
          std::max(a.z, b.z) < low.z - 5.2 || std::min(a.z, b.z) > high.z + 5.2) continue;
      const point delta = subtract(b, a);
      const uint32_t steps = std::max(1u, uint32_t(std::ceil(std::hypot(delta.x, delta.y, delta.z) * 4.0)));
      for (uint32_t step = 0; step <= steps; ++step) {
        const point centre{a.x + delta.x * double(step) / steps,
                           a.y + delta.y * double(step) / steps,
                           a.z + delta.z * double(step) / steps};
        if (box_field(centre, mid, half) < starter_sub_path_radius + 0.13) {
          return "outpost solid intersects submarine route beside base " +
                 std::to_string(structure.node_id) + " on edge " +
                 std::to_string(link.a) + "--" + std::to_string(link.b) +
                 " at (" + std::to_string(mid.x) + "," + std::to_string(mid.y) +
                 "," + std::to_string(mid.z) + ") local [" +
                 std::to_string(solid.footprint.u0) + "," +
                 std::to_string(solid.footprint.v0) + ".." +
                 std::to_string(solid.footprint.u1) + "," +
                 std::to_string(solid.footprint.v1) + "]";
        }
      }
    }
  }
  return {};
}

std::vector<built_solid> fit_outpost_solids(const adventure& world,
                                           const local_structure& structure) {
  std::vector<built_solid> fitted;
  const auto add = [&](const auto& self, const built_solid solid) -> void {
    if (verify_outpost_solids(world, structure, {solid}).empty()) {
      fitted.push_back(solid);
      return;
    }
    const int32_t du = solid.footprint.u1 - solid.footprint.u0;
    const int32_t dv = solid.footprint.v1 - solid.footprint.v0;
    const int32_t dy = solid.y1 - solid.y0;
    const int32_t longest = std::max({du, dv, dy});
    if (longest <= 100) return; // предел разбиения: панель 1 м
    built_solid first = solid, second = solid;
    if (du == longest) {
      const int32_t cut = solid.footprint.u0 + du / 2;
      first.footprint.u1 = cut;
      second.footprint.u0 = cut;
    } else if (dv == longest) {
      const int32_t cut = solid.footprint.v0 + dv / 2;
      first.footprint.v1 = cut;
      second.footprint.v0 = cut;
    } else {
      const int32_t cut = solid.y0 + dy / 2;
      first.y1 = cut;
      second.y0 = cut;
    }
    self(self, first);
    self(self, second);
  };
  for (const built_solid& solid : materialize_outpost_solids(structure)) add(add, solid);
  return fitted;
}

bool write_structure_svg(const adventure& world, const uint32_t node_id, const std::string& path) {
  if (node_id >= world.route.nodes.size() || world.route.nodes[node_id].kind == theme::empty) return false;
  const local_structure structure = materialize_local_structure(world, node_id);
  std::ofstream output(path);
  if (!output) return false;
  if (!is_base(structure.kind)) {
    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-24 -24 48 48\" "
              "width=\"960\" height=\"960\">\n"
              "<rect x=\"-24\" y=\"-24\" width=\"48\" height=\"48\" fill=\"#1b2024\"/>\n";
    const site local = materialize_site(world, node_id);
    for (const room& chamber : local.rooms) {
      output << "<circle cx=\"" << chamber.centre.x - structure.centre.x << "\" cy=\""
             << chamber.centre.z - structure.centre.z << "\" r=\"" << chamber.radius
             << "\" fill=\"#314f5e\" stroke=\"#8ab5c7\" stroke-width=\"0.1\"/>\n";
    }
    for (const resource_deposit& deposit : structure.deposits) {
      output << "<circle cx=\"" << deposit.centre.x - structure.centre.x << "\" cy=\""
             << deposit.centre.z - structure.centre.z << "\" r=\"0.45\" fill=\"#e4c46b\"/>\n"
             << "<text x=\"" << deposit.centre.x - structure.centre.x + 0.7 << "\" y=\""
             << deposit.centre.z - structure.centre.z << "\" fill=\"white\" font-size=\"0.7\">"
             << deposit.kind << ':' << deposit.amount << "</text>\n";
    }
  } else {
    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-13 -16 50 32\" "
              "width=\"1200\" height=\"768\">\n"
              "<rect x=\"-13\" y=\"-16\" width=\"50\" height=\"32\" fill=\"#1b2024\"/>\n";
    for (const built_zone& zone : structure.zones) {
      output << "<rect x=\"" << double(zone.box.u0) / 100.0 << "\" y=\""
             << double(zone.box.v0) / 100.0 << "\" width=\""
             << double(zone.box.u1 - zone.box.u0) / 100.0 << "\" height=\""
             << double(zone.box.v1 - zone.box.v0) / 100.0
             << "\" fill=\"#425869\"/>\n"
             << "<text x=\"" << double(zone.box.u0 + zone.box.u1) / 200.0
             << "\" y=\"" << double(zone.box.v0 + zone.box.v1) / 200.0
             << "\" text-anchor=\"middle\" fill=\"white\" font-size=\"0.6\">"
             << zone.purpose << "</text>\n";
    }
    for (const built_solid& solid : fit_outpost_solids(world, structure)) {
      if (solid.y1 <= structure.level_cm - 260 ||
          solid.y0 >= structure.level_cm + 260) continue; // пол/крыша, не стены плана
      output << "<rect x=\"" << double(solid.footprint.u0) / 100.0 << "\" y=\""
             << double(solid.footprint.v0) / 100.0 << "\" width=\""
             << double(solid.footprint.u1 - solid.footprint.u0) / 100.0
             << "\" height=\"" << double(solid.footprint.v1 - solid.footprint.v0) / 100.0
             << "\" fill=\"#b8c4ca\"/>\n";
    }
    for (const built_opening& opening : structure.openings) {
      output << "<line x1=\"" << double(opening.span.u0) / 100.0 << "\" y1=\""
             << double(opening.span.v0) / 100.0 << "\" x2=\""
             << double(opening.span.u1) / 100.0 << "\" y2=\""
             << double(opening.span.v1) / 100.0
             << "\" stroke=\"#e5c36b\" stroke-width=\"0.28\"/>\n";
    }
    if (structure.dock_axis != 0) {
      output << "<rect x=\"-3\" y=\"-3\" width=\"6\" height=\"6\" "
                "fill=\"none\" stroke=\"#e5c36b\" stroke-width=\"0.2\"/>\n"
             << "<text x=\"0\" y=\"2.4\" text-anchor=\"middle\" "
                "fill=\"#e5c36b\" font-size=\"0.7\">"
             << (structure.dock_axis > 0 ? "floor hatch" : "roof hatch")
             << "</text>\n";
    }
    for (const built_port& port : structure.ports) {
      output << "<line x1=\"" << double(port.span.u0) / 100.0 << "\" y1=\""
             << double(port.span.v0) / 100.0 << "\" x2=\""
             << double(port.span.u1) / 100.0 << "\" y2=\""
             << double(port.span.v1) / 100.0
             << "\" stroke=\"#65d3e3\" stroke-width=\"0.32\"/>\n";
    }
  }
  output << "</svg>\n";
  return bool(output);
}

} // namespace gn09
