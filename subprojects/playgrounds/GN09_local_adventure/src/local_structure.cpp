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

struct outpost_profile {
  int32_t side_arm_end = 2800;
  int32_t slot = 600;
  int32_t spine_half = 200;
  int32_t module_depth = 550;
};

outpost_profile profile_for(const site_recipe recipe) {
  switch (recipe) {
    case site_recipe::mining_base: return {3400, 700, 180, 500};
    case site_recipe::residential_base: return {2000, 800, 300, 900};
    case site_recipe::military_base: return {2200, 500, 200, 500};
    default: return {};
  }
}

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

double segment_distance_3d(const point p, const point a, const point b) {
  const point ab = subtract(b, a);
  const double t = std::clamp(dot(subtract(p, a), ab) / dot(ab, ab), 0.0, 1.0);
  const point closest{a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t};
  const point gap = subtract(p, closest);
  return std::sqrt(dot(gap, gap));
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

const char* name(const hatch_condition condition) {
  switch (condition) {
    case hatch_condition::operable: return "operable";
    case hatch_condition::jammed: return "jammed";
    case hatch_condition::breached: return "breached";
  }
  return "unknown";
}

int8_t preferred_dock_axis(const uint64_t seed, const node& at) {
  const uint64_t pose = devils_engine::utils::mix(seed, at.key, 0x444f434bull) % 10u;
  const site_recipe recipe = recipe_of(seed, at);
  if (recipe == site_recipe::mining_base) return pose < 6u ? 0 : 1;
  if (recipe == site_recipe::residential_base) return pose < 8u ? -1 : 0;
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
  result.recipe = local.recipe;
  result.centre = at.centre;

  if (!is_base(at.kind)) {
    for (uint32_t i = 1; i < local.rooms.size(); ++i) {
      const room& chamber = local.rooms[i];
      const uint64_t hash = devils_engine::utils::mix(world.seed, at.key, i, 0x524553ull);
      const double step = 0.3;
      const point centre{chamber.centre.x + (int(hash & 3u) - 1.5) * step,
                         chamber.centre.y - chamber.radius + 0.8,
                         chamber.centre.z + (int((hash >> 2) & 3u) - 1.5) * step};
      const char* kind = local.recipe == site_recipe::nest_cave ? "egg_cluster" :
                         local.recipe == site_recipe::thermal_cave ? "thermal_crystal" :
                         (hash & 16u) ? "metal_ore" : "mineral_vein";
      result.features.push_back({i, centre, kind, 1u + uint32_t((hash >> 8) % 3u)});
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

  result.level_cm = result.dock_axis > 0 ? 2400 :
                    result.dock_axis < 0 ? -1575 : 0;
  if (at.kind == theme::derelict) {
    result.dock_hatch = devils_engine::utils::mix(world.seed, at.key, 0x4841544348ull) & 1u
      ? hatch_condition::jammed : hatch_condition::breached;
    if (world.outpost_breach.size() == world.route.nodes.size()) {
      result.roof_breach = world.outpost_breach[node_id];
    }
  }
  if (world.outpost_glazing.size() == world.route.nodes.size()) {
    result.glazed_roofs = world.outpost_glazing[node_id];
  }

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
  const outpost_profile profile = profile_for(result.recipe);

  // 0: мокрый док; 1: шлюз; 2: технический коридор; 3..: модули.
  // Смежность и проёмы задаются точными сантиметровыми отрезками как у GN07.
  const uint32_t modules = uint32_t(local.rooms.size() - 1);
  const uint32_t slots = (modules + 1u) / 2u;
  const int32_t spine_begin = result.dock_axis == 0 ? profile.side_arm_end : 400;
  const int32_t end = spine_begin + int32_t(slots) * profile.slot;
  result.zones.push_back({{-1200, -1200, 1200, 1200}, "wet_dock"});
  result.zones.push_back({result.dock_axis == 0
    ? plan_rect{1200, offset_v - 200, spine_begin, offset_v + 200}
    : plan_rect{-400, -400, 400, 400}, "airlock"});
  result.zones.push_back({{spine_begin, offset_v - profile.spine_half,
                           end, offset_v + profile.spine_half}, "spine"});
  if (result.dock_axis == 0) {
    result.openings.push_back({0, 1, {1200, offset_v - 125, 1200, offset_v + 125}});
  }
  result.openings.push_back({1, 2, {spine_begin, offset_v - 125,
                                     spine_begin, offset_v + 125}});
  for (uint32_t i = 0; i < modules; ++i) {
    const int32_t x0 = spine_begin + int32_t(i / 2u) * profile.slot;
    const bool upper = i % 2u == 0;
    const int32_t z0 = offset_v + (upper ? profile.spine_half :
                                          -profile.spine_half - profile.module_depth);
    const int32_t z1 = offset_v + (upper ? profile.spine_half + profile.module_depth :
                                          -profile.spine_half);
    result.zones.push_back({{x0, z0, x0 + profile.slot, z1}, local.rooms[i + 1].purpose});
    const int32_t door_u0 = x0 + (profile.slot - 200) / 2;
    result.openings.push_back({2, i + 3u,
      {door_u0, offset_v + (upper ? profile.spine_half : -profile.spine_half),
       door_u0 + 200, offset_v + (upper ? profile.spine_half : -profile.spine_half)}});
  }
  for (uint32_t i = 0; i < 2; ++i) {
    const plan_rect room = result.zones[3 + i].box;
    const bool upper = i == 0;
    const int32_t wall = upper ? room.v1 : room.v0;
    const hatch_condition condition = at.kind == theme::outpost
      ? hatch_condition::operable
      : upper ? hatch_condition::jammed : hatch_condition::breached;
    const int32_t door_u0 = room.u0 + (room.u1 - room.u0 - 200) / 2;
    result.emergency_exits.push_back({3 + i,
      {door_u0, wall, door_u0 + 200, wall}, condition});
  }
  return result;
}

std::string verify_local_structure(const adventure& world, const site& local,
                                   const local_structure& structure) {
  if (structure.node_id != local.node_id || structure.kind != local.kind ||
      structure.recipe != local.recipe ||
      structure.centre.x != local.centre.x || structure.centre.y != local.centre.y ||
      structure.centre.z != local.centre.z) return "local structure disagrees with site";
  const bool built_recipe = local.recipe == site_recipe::mining_base ||
    local.recipe == site_recipe::residential_base ||
    local.recipe == site_recipe::military_base;
  if (is_base(local.kind) != built_recipe) return "local recipe disagrees with site kind";
  if (!is_base(local.kind)) {
    if (!structure.zones.empty() || !structure.openings.empty() ||
        !structure.ports.empty() || !structure.emergency_exits.empty() ||
        structure.features.size() != local.rooms.size() - 1) {
      return "natural site has an invalid feature plan";
    }
    for (const local_feature& feature : structure.features) {
      if (feature.room == 0 || feature.room >= local.rooms.size() || feature.amount == 0 ||
          feature.kind.empty()) return "local feature lacks a chamber or count";
      const point delta = subtract(feature.centre, local.rooms[feature.room].centre);
      if (dot(delta, delta) > local.rooms[feature.room].radius * local.rooms[feature.room].radius ||
          density(world, feature.centre) >= 0.0) return "local feature lies outside its chamber";
    }
    return {};
  }
  if (!structure.features.empty() || structure.zones.size() != local.rooms.size() + 2 ||
      structure.ports.size() < local.portals.size() ||
      structure.emergency_exits.size() != 2 ||
      structure.openings.size() != structure.zones.size() -
                                   (structure.dock_axis == 0 ? 1u : 2u)) {
    return "outpost has an invalid zone or opening count";
  }
  if ((structure.dock_axis == 0 && structure.level_cm != 0) ||
      (structure.dock_axis != 0 &&
       (std::abs(structure.dock_axis) != 1 ||
        structure.level_cm != (structure.dock_axis > 0 ? 2400 : -1575) ||
        structure.zones[1].box.u0 != -400 || structure.zones[1].box.u1 != 400 ||
        structure.zones[1].box.v0 != -400 || structure.zones[1].box.v1 != 400))) {
    return "outpost has an invalid docking face";
  }
  if ((local.kind == theme::outpost && structure.dock_hatch != hatch_condition::operable) ||
      (local.kind == theme::derelict &&
       structure.dock_hatch == hatch_condition::operable)) {
    return "outpost docking hatch has an invalid condition";
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
  int32_t first_u = INT32_MAX, last_u = INT32_MIN;
  int32_t first_v = INT32_MAX, last_v = INT32_MIN;
  for (size_t i = 2; i < structure.zones.size(); ++i) {
    const plan_rect box = structure.zones[i].box;
    first_u = std::min(first_u, box.u0); last_u = std::max(last_u, box.u1);
    first_v = std::min(first_v, box.v0); last_v = std::max(last_v, box.v1);
  }
  const int32_t body_length = last_u - first_u, body_width = last_v - first_v;
  if ((structure.recipe == site_recipe::mining_base &&
       (body_length < 2000 || body_width > 1600)) ||
      (structure.recipe == site_recipe::residential_base && body_width < 2200) ||
      (structure.recipe == site_recipe::military_base &&
       (body_length > 1500 || body_width > 1600))) {
    return "outpost recipe does not make its declared body shape";
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
  for (size_t i = 0; i < structure.emergency_exits.size(); ++i) {
    const emergency_exit& door = structure.emergency_exits[i];
    if (door.zone < 3 || door.zone >= structure.zones.size() ||
        (i > 0 && door.zone == structure.emergency_exits[0].zone)) {
      return "emergency exit lacks a distinct module";
    }
    const plan_rect room = structure.zones[door.zone].box;
    if (door.span.v0 != door.span.v1 ||
        (door.span.v0 != room.v0 && door.span.v0 != room.v1) ||
        door.span.u0 < room.u0 || door.span.u1 > room.u1 ||
        door.span.u1 - door.span.u0 != 200) {
      return "emergency exit is not on an outer module wall";
    }
    if (local.kind == theme::outpost && door.condition != hatch_condition::operable) {
      return "active outpost has a failed emergency door";
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
  double field = 3.0;
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
    // Узкая шахта соединяет вертикальный люк с уже существующей пещерой;
    // мокрый док при этом не вырезается формой аванпоста.
    field = std::min(field, box_field(local,
      {0, structure.dock_axis > 0 ? 16.0 : -11.5, 0},
      {3.6, structure.dock_axis > 0 ? 6.0 : 3.5, 3.6}));
  }
  // Два узких наружных пути к аварийным дверям. Они идут снаружи модулей
  // обратно к естественному доку и не открывают весь корпус станции.
  for (const emergency_exit& door : structure.emergency_exits) {
    const double u = double(door.span.u0 + door.span.u1) / 200.0;
    const double v = double(door.span.v0) / 100.0;
    const double side = door.span.v0 == structure.zones[door.zone].box.v0 ? -1.0 : 1.0;
    const point start{u, double(structure.level_cm) / 100.0, v + side * 1.2};
    const point mouth{0.0, 0.0, side * 12.0};
    field = std::min(field, segment_distance_3d(local, start, mouth) - 3.0);
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

bool outpost_envelope_reaches(const local_structure& structure, const point low,
                              const point high) {
  if (!is_base(structure.kind)) return false;
  // Локальная оболочка всех слагаемых outpost_envelope_field. Каждое из них —
  // честное расстояние до коробки или отрезка, поэтому дальше 3 м от оболочки
  // функция возвращает ровно стартовое 3.0.
  const double level = double(structure.level_cm) / 100.0;
  double u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0, y0 = level - 4.5, y1 = level + 4.5;
  const auto take = [&](const double u, const double v) {
    u0 = std::min(u0, u); u1 = std::max(u1, u);
    v0 = std::min(v0, v); v1 = std::max(v1, v);
  };
  for (const built_zone& zone : structure.zones) {
    take(double(zone.box.u0) / 100.0, double(zone.box.v0) / 100.0);
    take(double(zone.box.u1) / 100.0, double(zone.box.v1) / 100.0);
  }
  if (structure.dock_axis != 0) {
    take(-3.6, -3.6);
    take(3.6, 3.6);
    y0 = std::min(y0, structure.dock_axis > 0 ? 10.0 : -15.0);
    y1 = std::max(y1, structure.dock_axis > 0 ? 22.0 : -8.0);
  }
  for (const emergency_exit& door : structure.emergency_exits) {
    take(double(door.span.u0) / 100.0, double(door.span.v0) / 100.0 - 1.2);
    take(double(door.span.u1) / 100.0, double(door.span.v0) / 100.0 + 1.2);
    take(0.0, -12.0);
    take(0.0, 12.0);
    y0 = std::min(y0, 0.0);
    y1 = std::max(y1, 0.0);
  }
  // Проёмы лежат на стенах зон; их вырез выступает за стену на 1.25+0.35 м.
  const double margin = 3.0 + 1.6 + 1.0;
  point world_low{1e30, structure.centre.y + y0 - margin, 1e30};
  point world_high{-1e30, structure.centre.y + y1 + margin, -1e30};
  for (const double u : {u0 - margin, u1 + margin}) {
    for (const double v : {v0 - margin, v1 + margin}) {
      const point p = structure_to_world(structure, u, 0.0, v);
      world_low.x = std::min(world_low.x, p.x); world_high.x = std::max(world_high.x, p.x);
      world_low.z = std::min(world_low.z, p.z); world_high.z = std::max(world_high.z, p.z);
    }
  }
  return world_low.x <= high.x && world_high.x >= low.x &&
         world_low.y <= high.y && world_high.y >= low.y &&
         world_low.z <= high.z && world_high.z >= low.z;
}

uint32_t outpost_body_rock_percent(const adventure& world,
                                   const local_structure& structure) {
  if (!is_base(structure.kind) || structure.zones.size() < 3) return 0;
  uint64_t rock = 0, total = 0;
  // Только главный корпус: узкий шлюзовой рукав намеренно выходит в пещеру.
  // Область оценивается ДО вырезания интерьера станции.
  for (size_t i = 2; i < structure.zones.size(); ++i) {
    const plan_rect box = structure.zones[i].box;
    const uint64_t area = uint64_t(box.u1 - box.u0) * uint64_t(box.v1 - box.v0);
    for (int u = 1; u <= 5; u += 2) for (int v = 1; v <= 5; v += 2) {
      const double local_u = double(box.u0) / 100.0 +
                             double(box.u1 - box.u0) * u / 600.0;
      const double local_v = double(box.v0) / 100.0 +
                             double(box.v1 - box.v0) * v / 600.0;
      const point sample = structure_to_world(structure, local_u,
                                               double(structure.level_cm) / 100.0,
                                               local_v);
      total += area;
      if (cave_density(world, sample) > 0.0) rock += area;
    }
  }
  return total ? uint32_t(rock * 100 / total) : 0;
}

namespace {

// Верх крыши зоны над её серединой в локальных метрах (см. slab ниже).
double roof_top(const local_structure& structure) {
  const double embed = structure.dock_axis == 0 ? 0.6 : 2.4;
  return double(structure.level_cm) / 100.0 + 3.0 + embed;
}

bool roof_hatch(const local_structure& structure, const uint32_t zone) {
  return zone == 1 && structure.dock_axis < 0;
}

} // namespace

std::string verify_outpost_environment(const adventure& world,
                                       const local_structure& structure) {
  if (!is_base(structure.kind)) return {};
  // 24x24x7 м мокрого дока — свойство природной пещеры, а не вырез станции.
  // Проверяем углы и внутренние точки; сам гарантированный объём следует из
  // базовой сферы радиусом 19.5 м (> радиус дока + surface_guard).
  for (const double u : {-12.0, 0.0, 12.0}) {
    for (const double y : {-3.5, 0.0, 3.5}) {
      for (const double v : {-12.0, 0.0, 12.0}) {
        if (cave_density(world, structure_to_world(structure, u, y, v)) >= 0.0) {
          return "outpost wet dock is not naturally open";
        }
      }
    }
  }
  const double berth_v = double(structure.zones[1].box.v0 + 200) / 100.0;
  for (int u = -12; u <= 12; u += 4) for (int y = -8; y <= 8; y += 4) {
    for (int v = -12; v <= 12; v += 4) {
      const point local{double(u), double(y), double(v)};
      if (docking_sweep_field(local, berth_v, structure.dock_axis) > 0.0) continue;
      if (cave_density(world, structure_to_world(structure, local.x, local.y,
                                                 local.z)) >= 0.0) {
        return "outpost docking sweep is not naturally open";
      }
    }
  }
  if (structure.dock_axis >= 0 && outpost_body_rock_percent(world, structure) < 80) {
    return "outpost body has too little surrounding rock";
  }
  const uint16_t glazed_mask = structure.glazed_roofs;
  if (structure.roof_breach != 0 || glazed_mask != 0) {
    if (structure.roof_breach != 0 &&
        (structure.kind != theme::derelict || structure.roof_breach >= structure.zones.size() ||
         roof_hatch(structure, structure.roof_breach))) return "roof breach names an invalid zone";
    if ((glazed_mask & 7u) != 0 || (glazed_mask >> std::min<size_t>(16, structure.zones.size())) != 0 ||
        (structure.roof_breach != 0 && ((glazed_mask >> structure.roof_breach) & 1u))) {
      return "glazed roof names an invalid module";
    }
    // Независимо от выбора: решётка 5x5 над всей панелью 3x3 м и на двух высотах.
    // Окно проверки может обрезать галерею соседнего сектора, поэтому воздух
    // ищется в абсолютном окружении базы, как в настоящем мире у этих чанков.
    const adventure context = canonical_context(world.seed, structure.centre,
                                                world.clearance_radius);
    const auto exposed = [&](const uint32_t zone) {
      const plan_rect box = structure.zones[zone].box;
      const double u = double(box.u0 + box.u1) / 200.0, v = double(box.v0 + box.v1) / 200.0;
      for (int a = -2; a <= 2; ++a) for (int b = -2; b <= 2; ++b) {
        for (const double lift : {0.6, 1.8}) {
          if (cave_density(context, structure_to_world(structure, u + 0.7 * a,
                                                       roof_top(structure) + lift,
                                                       v + 0.7 * b)) >= 0.0) return false;
        }
      }
      return true;
    };
    if (structure.roof_breach != 0 && !exposed(structure.roof_breach)) {
      return "roof breach opens into solid rock";
    }
    for (uint32_t zone = 3; zone < structure.zones.size() && zone < 16; ++zone) {
      if (((glazed_mask >> zone) & 1u) && !exposed(zone)) return "glazed roof faces solid rock";
    }
  }
  for (const emergency_exit& door : structure.emergency_exits) {
    const double side = door.span.v0 == structure.zones[door.zone].box.v0 ? -1.0 : 1.0;
    const point start{double(door.span.u0 + door.span.u1) / 200.0,
                      double(structure.level_cm) / 100.0,
                      double(door.span.v0) / 100.0 + side * 1.2};
    const point mouth{0, 0, side * 12.0};
    if (cave_density(world, structure_to_world(structure, mouth.x, mouth.y,
                                                mouth.z)) >= 0.0) {
      return "emergency exit does not reach the natural cave";
    }
    for (int i = 0; i <= 8; ++i) {
      const double t = double(i) / 8.0;
      const point p{start.x + (mouth.x - start.x) * t,
                    start.y + (mouth.y - start.y) * t,
                    start.z + (mouth.z - start.z) * t};
      if (density(world, structure_to_world(structure, p.x, p.y, p.z)) >= 0.0) {
        return "emergency exit route is blocked by rock";
      }
    }
  }
  return {};
}

double glazing_chance(const site_recipe recipe) {
  return recipe == site_recipe::residential_base ? 0.8 :
         recipe == site_recipe::mining_base ? 0.3 :
         recipe == site_recipe::military_base ? 0.2 : 0.0;
}

uint16_t open_roof_mask(const adventure& world, const local_structure& structure) {
  if (!is_base(structure.kind)) return 0;
  const double top = roof_top(structure);
  uint16_t mask = 0;
  for (uint32_t zone = 1; zone < structure.zones.size() && zone < 16; ++zone) {
    if (roof_hatch(structure, zone)) continue;
    const plan_rect box = structure.zones[zone].box;
    const double u = double(box.u0 + box.u1) / 200.0, v = double(box.v0 + box.v1) / 200.0;
    bool open = true;
    // Решётка 4x4 чуть шире панели 3x3 м на трёх высотах до 2.5 м: выбор строже
    // проверки, которая берёт другие точки внутри той же области.
    for (int a = 0; a < 4 && open; ++a) for (int b = 0; b < 4 && open; ++b) {
      for (const double lift : {0.3, 1.2, 2.5}) {
        if (!open) break;
        open = cave_density(world, structure_to_world(structure, u - 1.8 + 1.2 * a,
                                                      top + lift, v - 1.8 + 1.2 * b)) < 0.0;
      }
    }
    if (open) mask = uint16_t(mask | (1u << zone));
  }
  return mask;
}

roof_finish choose_roof_finish(const adventure& world, const local_structure& structure) {
  roof_finish result;
  if (!is_base(structure.kind)) return result;
  const uint16_t open = open_roof_mask(world, structure);
  const node& at = world.route.nodes.at(structure.node_id);
  if (structure.kind == theme::derelict) {
    const uint32_t count = uint32_t(structure.zones.size()) - 1u;
    const uint32_t first = uint32_t(devils_engine::utils::mix(world.seed, at.key,
                                                              0x524f4f46ull) % count);
    for (uint32_t step = 0; step < count && result.breach == 0; ++step) {
      const uint32_t zone = 1u + (first + step) % count;
      if (open & (1u << zone)) result.breach = zone;
    }
  }
  // Стекло — удача расположения, а не обещание рецепта: только открытый модуль и
  // только с вероятностью рецепта. Пролом заменяет панель, стекла на нём нет.
  const double chance = glazing_chance(structure.recipe);
  for (uint32_t zone = 3; zone < structure.zones.size() && zone < 16; ++zone) {
    if (!(open & (1u << zone)) || zone == result.breach) continue;
    const uint64_t roll = devils_engine::utils::mix(world.seed, at.key, zone, 0x474c415aull);
    if (double(roll >> 11) * 0x1p-53 < chance) {
      result.glazed = uint16_t(result.glazed | (1u << zone));
    }
  }
  return result;
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
                          const bool hatch, const bool roof) {
      if (!hatch) {
        const bool glazed = roof && i < 16 && ((structure.glazed_roofs >> i) & 1u);
        const bool broken = roof && structure.kind == theme::derelict &&
          i == structure.roof_breach;
        if (!glazed && !broken) {
          solids.push_back({zone, y0, y1, floor_shade});
          return;
        }
        // Застеклённый модуль имеет отдельную «стеклянную» панель; у пролома
        // заброшенной базы центральный кусок отсутствует.
        const int32_t u0 = (zone.u0 + zone.u1 - 300) / 2;
        const int32_t v0 = (zone.v0 + zone.v1 - 300) / 2;
        const int32_t u1 = u0 + 300, v1 = v0 + 300;
        solids.push_back({{zone.u0, zone.v0, u0, zone.v1}, y0, y1, floor_shade});
        solids.push_back({{u1, zone.v0, zone.u1, zone.v1}, y0, y1, floor_shade});
        solids.push_back({{u0, zone.v0, u1, v0}, y0, y1, floor_shade});
        solids.push_back({{u0, v1, u1, zone.v1}, y0, y1, floor_shade});
        if (!broken) solids.push_back({{u0, v0, u1, v1}, y0, y1, 121});
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
    slab(bottom - embed, bottom + 40, i == 1 && structure.dock_axis > 0, false);
    slab(top - 40, top + embed, i == 1 && structure.dock_axis < 0, true);
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
      for (const emergency_exit& door : structure.emergency_exits) {
        if (door.zone == i) add_gap(door.span);
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
  for (const emergency_exit& door : structure.emergency_exits) {
    const int32_t wall = door.span.v0;
    const plan_rect footprint{door.span.u0, wall - 20,
                              door.span.u1, wall + 20};
    solids.push_back({footprint, structure.level_cm - 260,
                      structure.level_cm - 120, wall_shade});
    solids.push_back({footprint, structure.level_cm + 120,
                      structure.level_cm + 260, wall_shade});
    if (door.condition != hatch_condition::breached) {
      solids.push_back({footprint, structure.level_cm - 120,
                        structure.level_cm + 120,
                        door.condition == hatch_condition::operable ? uint8_t{126} : uint8_t{115},
                        true});
    }
  }
  if (structure.dock_hatch == hatch_condition::jammed) {
    if (structure.dock_axis == 0) {
      const plan_rect span = structure.openings.front().span;
      solids.push_back({{span.u0, span.v0, span.u0 + 20, span.v1},
                        structure.level_cm - 120, structure.level_cm + 120, 115, true});
    } else {
      const int32_t face = structure.level_cm + (structure.dock_axis > 0 ? -300 : 300);
      solids.push_back({{-300, -300, 300, 300}, face - 10, face + 10, 115, true});
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
    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-32 -32 64 64\" "
              "width=\"960\" height=\"960\">\n"
              "<rect x=\"-32\" y=\"-32\" width=\"64\" height=\"64\" fill=\"#1b2024\"/>\n";
    const site local = materialize_site(world, node_id);
    for (const room& chamber : local.rooms) {
      output << "<circle cx=\"" << chamber.centre.x - structure.centre.x << "\" cy=\""
             << chamber.centre.z - structure.centre.z << "\" r=\"" << chamber.radius
             << "\" fill=\"#314f5e\" stroke=\"#8ab5c7\" stroke-width=\"0.1\"/>\n";
    }
    for (const local_feature& feature : structure.features) {
      output << "<circle cx=\"" << feature.centre.x - structure.centre.x << "\" cy=\""
             << feature.centre.z - structure.centre.z << "\" r=\"0.45\" fill=\"#e4c46b\"/>\n"
             << "<text x=\"" << feature.centre.x - structure.centre.x + 0.7 << "\" y=\""
             << feature.centre.z - structure.centre.z << "\" fill=\"white\" font-size=\"0.7\">"
             << feature.kind << ':' << feature.amount << "</text>\n";
    }
  } else {
    int32_t far_u = 0;
    for (const built_zone& zone : structure.zones) far_u = std::max(far_u, zone.box.u1);
    const double width = double(far_u) / 100.0 + 16.0;
    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-13 -18 "
           << width << " 36\" width=\"1200\" height=\"720\">\n"
           << "<rect x=\"-13\" y=\"-18\" width=\"" << width
           << "\" height=\"36\" fill=\"#1b2024\"/>\n";
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
      if (solid.shade != 121 || solid.y0 < structure.level_cm + 260) continue;
      output << "<rect x=\"" << double(solid.footprint.u0) / 100.0 << "\" y=\""
             << double(solid.footprint.v0) / 100.0 << "\" width=\""
             << double(solid.footprint.u1 - solid.footprint.u0) / 100.0
             << "\" height=\"" << double(solid.footprint.v1 - solid.footprint.v0) / 100.0
             << "\" fill=\"#81c8da\" fill-opacity=\"0.55\"/>\n";
    }
    if (structure.roof_breach != 0) {
      const plan_rect box = structure.zones[structure.roof_breach].box;
      output << "<rect x=\"" << double(box.u0 + box.u1 - 300) / 200.0 << "\" y=\""
             << double(box.v0 + box.v1 - 300) / 200.0
             << "\" width=\"3\" height=\"3\" fill=\"none\" stroke=\"#d9604c\" "
                "stroke-width=\"0.2\" stroke-dasharray=\"0.4 0.3\"/>\n";
    }
    for (const emergency_exit& door : structure.emergency_exits) {
      const double side = door.span.v0 == structure.zones[door.zone].box.v0 ? -1.0 : 1.0;
      output << "<line x1=\"" << double(door.span.u0 + door.span.u1) / 200.0
             << "\" y1=\"" << double(door.span.v0) / 100.0 + side * 1.2
             << "\" x2=\"0\" y2=\"" << side * 12.0
             << "\" stroke=\"#65d3e3\" stroke-width=\"0.15\" "
                "stroke-dasharray=\"0.5 0.4\"/>\n";
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
    for (const emergency_exit& door : structure.emergency_exits) {
      const char* colour = door.condition == hatch_condition::operable ? "#65d3e3" :
                           door.condition == hatch_condition::jammed ? "#e59651" : "#d66d73";
      output << "<line x1=\"" << double(door.span.u0) / 100.0 << "\" y1=\""
             << double(door.span.v0) / 100.0 << "\" x2=\""
             << double(door.span.u1) / 100.0 << "\" y2=\""
             << double(door.span.v1) / 100.0 << "\" stroke=\"" << colour
             << "\" stroke-width=\"0.55\"/>\n";
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
