#include "adventure.h"
#include "local_structure.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <fstream>
#include <numeric>
#include <stdexcept>

#include "devils_engine/utils/prng.h"

namespace gn09 {
namespace {

constexpr double site_radius = 8.0;

double random01(const uint64_t seed, const uint64_t a, const uint64_t b = 0) {
  return devils_engine::utils::prng_normalize(devils_engine::utils::mix(seed, a, b));
}

site_recipe recipe_for(const uint64_t seed, const node& at) {
  const uint64_t choice = devils_engine::utils::mix(seed, at.key, 0x524543495045ull);
  if (is_base(at.kind)) {
    return site_recipe(uint8_t(site_recipe::mining_base) + choice % 3u);
  }
  return at.kind == theme::vent ? site_recipe::thermal_cave :
    choice % 2u ? site_recipe::nest_cave : site_recipe::ore_cave;
}

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double amount) { return {a.x * amount, a.y * amount, a.z * amount}; }
double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const point a) { return std::sqrt(dot(a, a)); }
point cross(const point a, const point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

point port_position(const node& from, const node& toward) {
  if (from.kind == theme::empty) return from.centre;
  const point direction = subtract(toward.centre, from.centre);
  return add(from.centre, scale(direction, site_radius / length(direction)));
}

double segment_distance(const point p, const point a, const point b) {
  const point ab = subtract(b, a);
  const double t = std::clamp(dot(subtract(p, a), ab) / dot(ab, ab), 0.0, 1.0);
  return length(subtract(p, add(a, scale(ab, t))));
}

// Небольшой 3D value noise. В отличие от случайности по чанку, его ключ — мировой узел решётки.
// Шум меняет край тоннеля; гарантированное свободное ядро задаётся минимальным радиусом ниже.
double value_noise(const uint64_t seed, const point p, const double period) {
  const double x = p.x / period, y = p.y / period, z = p.z / period;
  const int64_t ix = int64_t(std::floor(x)), iy = int64_t(std::floor(y)), iz = int64_t(std::floor(z));
  const auto smooth = [](const double t) { return t * t * (3.0 - 2.0 * t); };
  const double fx = smooth(x - double(ix)), fy = smooth(y - double(iy)), fz = smooth(z - double(iz));
  const auto sample = [&](const int64_t dx, const int64_t dy, const int64_t dz) {
    const auto hash = devils_engine::utils::mix(seed, uint64_t(ix + dx), uint64_t(iy + dy), uint64_t(iz + dz));
    return 2.0 * devils_engine::utils::prng_normalize(hash) - 1.0;
  };
  const auto lerp = [](const double a, const double b, const double t) { return a + (b - a) * t; };
  const double bottom = lerp(lerp(sample(0, 0, 0), sample(1, 0, 0), fx),
                             lerp(sample(0, 1, 0), sample(1, 1, 0), fx), fy);
  const double top = lerp(lerp(sample(0, 0, 1), sample(1, 0, 1), fx),
                          lerp(sample(0, 1, 1), sample(1, 1, 1), fx), fy);
  return lerp(bottom, top, fz);
}

struct disjoint_set {
  std::vector<uint32_t> parent;
  explicit disjoint_set(const size_t count) : parent(count) { std::iota(parent.begin(), parent.end(), 0u); }
  uint32_t root(uint32_t n) {
    while (parent[n] != n) n = parent[n] = parent[parent[n]];
    return n;
  }
  bool join(const uint32_t a, const uint32_t b) {
    const uint32_t ra = root(a), rb = root(b);
    if (ra == rb) return false;
    parent[rb] = ra;
    return true;
  }
};

uint64_t node_key(const int x, const int z, const uint32_t direction,
                  const uint32_t step, const uint32_t branch = 0) {
  return devils_engine::utils::mix(0x474e3039ull, uint64_t(int64_t(x)),
                                   uint64_t(int64_t(z)), direction, step, branch);
}

point base_position(const uint64_t seed, const int x, const int z) {
  const uint64_t key = node_key(x, z, 0, 0);
  const int jx = int(devils_engine::utils::mix(seed, key, 11) % 49) - 24;
  const int jz = int(devils_engine::utils::mix(seed, key, 29) % 49) - 24;
  const int jy = (int(devils_engine::utils::mix(seed, key, 37) % 3) - 1) * 8;
  return {double(x * sector_size + sector_size / 2 + jx), double(jy),
          double(z * sector_size + sector_size / 2 + jz)};
}

uint64_t solid_volume(const std::vector<built_solid>& solids) {
  uint64_t total = 0;
  for (const built_solid& solid : solids) {
    total += uint64_t(solid.footprint.u1 - solid.footprint.u0) *
             uint64_t(solid.footprint.v1 - solid.footprint.v0) *
             uint64_t(solid.y1 - solid.y0);
  }
  return total;
}

uint64_t hatch_panel_volume(const std::vector<built_solid>& solids) {
  uint64_t total = 0;
  for (const built_solid& solid : solids) {
    if (!solid.hatch_panel) continue;
    total += uint64_t(solid.footprint.u1 - solid.footprint.u0) *
             uint64_t(solid.footprint.v1 - solid.footprint.v0) *
             uint64_t(solid.y1 - solid.y0);
  }
  return total;
}

double bend_distance(const point centre, const point a, const point b) {
  return segment_distance(centre, a, b);
}

// Ребро остаётся одним топологическим соединением. Ширина меняется вдоль него, а
// две смещённые полости дают асимметричные залы без дополнительных узлов графа.
double cave_edge_field(const adventure& world, const node& from, const node& to,
                       const point position, const double wall_noise) {
  const point a = port_position(from, to), b = port_position(to, from);
  const point ab = subtract(b, a);
  const double distance = length(ab);
  const double core = segment_distance(position, a, b) -
                      (world.clearance_radius + surface_guard);
  if (core > 24.0) return core;
  const point middle = scale(add(from.centre, to.centre), 0.5);
  const cave_biome biome = biome_at(world.seed, middle);
  const double vertical_gain = biome == cave_biome::karst ? 0.0 :
                               biome == cave_biome::rift ? 4.5 : 8.0;

  const uint64_t first = std::min(from.key, to.key), second = std::max(from.key, to.key);
  const uint64_t key = devils_engine::utils::mix(world.seed, first, second, 0x43415645ull);
  const bool constriction = devils_engine::utils::mix(key, 1) % 6 == 0;
  const point axis = scale(ab, 1.0 / distance);
  const point reference = std::abs(axis.y) < 0.85 ? point{0, 1, 0} : point{1, 0, 0};
  const point right = scale(cross(axis, reference), 1.0 / length(cross(axis, reference)));
  const point up = cross(right, axis);
  const double along = dot(subtract(position, a), axis);
  const double t = std::clamp(along / distance, 0.0, 1.0);
  const point nearest = add(a, scale(axis, std::clamp(along, 0.0, distance)));
  const point offset = subtract(position, nearest);
  const double side = dot(offset, right), height = dot(offset, up);
  const double beyond = std::max({-along, along - distance, 0.0});
  const auto pulse = [](const double t, const double centre, const double span) {
    const double q = std::max(0.0, 1.0 - std::abs(t - centre) / span);
    return q * q * (3.0 - 2.0 * q);
  };
  const double first_t = 0.25 + 0.12 * random01(world.seed, key, 2);
  const double second_t = 0.65 + 0.12 * random01(world.seed, key, 3);
  const double first_pulse = pulse(t, first_t, 0.27);
  const double second_pulse = pulse(t, second_t, 0.27);
  const double width = (constriction ? 5.7 : 9.0 + 2.0 * random01(world.seed, key, 4)) +
                       (3.0 + 2.0 * random01(world.seed, key, 5)) * first_pulse +
                       (3.0 + 2.0 * random01(world.seed, key, 6)) * second_pulse;
  const double roof = (constriction ? 5.3 : 7.5 + 1.5 * random01(world.seed, key, 7)) +
                      2.0 * first_pulse + 2.5 * second_pulse + vertical_gain;
  const double biome_width = width * (biome == cave_biome::chimney ? 0.82 : 1.0);
  const double gallery = (std::sqrt((side / biome_width) * (side / biome_width) +
                                    (height / roof) * (height / roof) +
                                    (beyond / std::min(biome_width, roof)) *
                                    (beyond / std::min(biome_width, roof))) - 1.0) *
                         std::min(biome_width, roof) + (constriction ? 0.25 : 1.0) * wall_noise;
  double field = std::min(core, gallery);
  for (uint64_t i = 0; i < 2; ++i) {
    const double place = i == 0 ? first_t : second_t;
    const double shift = (i == 0 ? 1.0 : -1.0) *
                         (3.0 + 2.5 * random01(world.seed, key, 10 + i));
    const double rise = (random01(world.seed, key, 12 + i) - 0.5) *
                        (4.0 + vertical_gain);
    const point centre = add(add(a, scale(axis, place * distance)),
                             add(scale(right, shift), scale(up, rise)));
    const point q = subtract(position, centre);
    const double axial = dot(q, axis) / (11.0 + 4.0 * random01(world.seed, key, 14 + i));
    const double lateral = dot(q, right) / (10.0 + 3.0 * random01(world.seed, key, 16 + i));
    const double vertical = dot(q, up) /
      (7.0 + 2.0 * random01(world.seed, key, 18 + i) + vertical_gain * 0.8);
    const double lobe = (std::sqrt(axial * axial + lateral * lateral + vertical * vertical) - 1.0) *
                        7.0 + wall_noise;
    field = std::min(field, lobe);
  }
  return field;
}

} // namespace

site_recipe recipe_of(const uint64_t seed, const node& at) {
  return recipe_for(seed, at);
}

const char* name(const theme value) {
  switch (value) {
    case theme::empty: return "empty";
    case theme::outpost: return "outpost";
    case theme::derelict: return "derelict";
    case theme::grotto: return "grotto";
    case theme::vent: return "vent";
  }
  return "unknown";
}

bool is_base(const theme value) { return value == theme::outpost || value == theme::derelict; }

const char* name(const cave_biome value) {
  switch (value) {
    case cave_biome::karst: return "karst";
    case cave_biome::rift: return "rift";
    case cave_biome::chimney: return "chimney";
  }
  return "unknown";
}

const char* name(const site_recipe value) {
  switch (value) {
    case site_recipe::ore_cave: return "ore_cave";
    case site_recipe::nest_cave: return "nest_cave";
    case site_recipe::thermal_cave: return "thermal_cave";
    case site_recipe::mining_base: return "mining_base";
    case site_recipe::residential_base: return "residential_base";
    case site_recipe::military_base: return "military_base";
  }
  return "unknown";
}

cave_biome biome_at(const uint64_t seed, const point position) {
  constexpr double span = 2.0 * sector_size;
  const int64_t bx = int64_t(std::floor(position.x / span));
  const int64_t bz = int64_t(std::floor(position.z / span));
  return cave_biome(devils_engine::utils::mix(seed, uint64_t(bx), uint64_t(bz),
                                               0x42494f4d45ull) % 3u);
}

std::optional<edge_pocket> materialize_edge_pocket(const uint64_t seed,
                                                    const node& a, const node& b) {
  const node& first = a.key < b.key ? a : b;
  const node& second = a.key < b.key ? b : a;
  const uint64_t key = devils_engine::utils::mix(seed, first.key, second.key,
                                                 0x504f434b4554ull);
  if (key % 3u != 0) return std::nullopt;
  const point ab = subtract(second.centre, first.centre);
  const double t = 0.35 + 0.3 * random01(seed, key, 1);
  const point mouth = add(first.centre, scale(ab, t));
  point side{-ab.z, 0, ab.x};
  if (length(side) < 0.1) side = {1, 0, 0};
  side = scale(side, 1.0 / length(side));
  const double sign = (key & 8u) ? 1.0 : -1.0;
  const point centre = add(mouth, add(scale(side, sign *
    (8.0 + 3.0 * random01(seed, key, 2))),
    {0, (random01(seed, key, 3) - 0.5) * 3.0, 0}));
  return edge_pocket{mouth, centre, 3.8 + 1.2 * random01(seed, key, 4)};
}

graph generate_graph(const uint64_t seed, const graph::window area) {
  if (area.max_x - area.min_x < 2 || area.max_z - area.min_z < 2) {
    throw std::invalid_argument("graph window must contain at least 2x2 sectors");
  }
  graph out;
  out.area = area;
  out.entry = 0;
  const int columns = area.max_x - area.min_x;
  const auto base_id = [&](const int x, const int z) {
    return uint32_t((z - area.min_z) * columns + (x - area.min_x));
  };
  const auto append = [&](const point position, const theme kind, const uint8_t rank,
                          const uint64_t key) {
    const uint32_t id = uint32_t(out.nodes.size());
    out.nodes.push_back({id, key, position, kind, rank});
    return id;
  };
  const auto connect = [&](const uint32_t a, const uint32_t b) { out.edges.push_back({a, b}); };
  // База в каждом секторе. Координаты и темы зависят от абсолютного адреса сектора,
  // поэтому один и тот же участок совпадает в разных окнах запроса.
  for (int z = area.min_z; z < area.max_z; ++z) {
    for (int x = area.min_x; x < area.max_x; ++x) {
      const uint64_t key = node_key(x, z, 0, 0);
      const point position = base_position(seed, x, z);
      const uint8_t rank = uint8_t((x > area.min_x) + (x + 1 < area.max_x) +
                                   (z > area.min_z) + (z + 1 < area.max_z));
      const theme kind = devils_engine::utils::mix(seed, key, 41) % 2 == 0
        ? theme::outpost : theme::derelict;
      append(position, kind, rank, key);
    }
  }
  out.destination = base_id(area.max_x - 1, area.max_z - 1);

  const auto route = [&](const int x, const int z, const uint32_t direction) {
    const uint32_t from = base_id(x, z);
    const uint32_t to = base_id(x + (direction == 1), z + (direction == 2));
    const point a = out.nodes[from].centre, b = out.nodes[to].centre;
    const point delta = subtract(b, a);
    const double horizontal = std::hypot(delta.x, delta.z);
    const point sideways{-delta.z / horizontal, 0, delta.x / horizontal};
    const uint64_t key = node_key(x, z, direction, 0);
    const double sign = devils_engine::utils::mix(seed, key, 7) % 2 ? 1.0 : -1.0;
    const double first_bend = sign * (12.0 + 7.0 * random01(seed, key, 101));
    const double second_bend = -sign * (11.0 + 7.0 * random01(seed, key, 102));
    const double first_t = 0.27 + 0.06 * random01(seed, key, 103);
    const double second_t = 0.68 + 0.06 * random01(seed, key, 104);
    const double first_y = a.y + (b.y - a.y) * first_t +
                           (direction == 1 ? (random01(seed, key, 105) - 0.5) * 8.0
                                           : sign * (6.0 + 4.0 * random01(seed, key, 105)));
    point first = add(add(a, scale(delta, first_t)), scale(sideways, first_bend));
    first.y = first_y;
    uint32_t previous = from;
    const uint32_t first_id = append(first, theme::empty, 3, node_key(x, z, direction, 1));
    connect(previous, first_id);
    previous = first_id;

    // Восточное соединение содержит настоящее вертикальное ребро (тот же X/Z).
    // Оно поднимается в пределах конечного диапазона Y, затем путь уходит в сторону.
    if (direction == 1) {
      point upper = first;
      upper.y += 12.0;
      const uint32_t shaft = append(upper, theme::empty, 2, node_key(x, z, direction, 2));
      connect(previous, shaft);
      previous = shaft;
    }
    point second = add(add(a, scale(delta, second_t)), scale(sideways, second_bend));
    second.y = b.y + (a.y - b.y) * (1.0 - second_t) -
               sign * (6.0 + 4.0 * random01(seed, key, 106));
    const uint32_t landmark_step = direction == 1 ? 3 : 2;
    const theme landmark = devils_engine::utils::mix(seed, key, 107) % 2 == 0
      ? theme::grotto : theme::vent;
    const uint32_t second_id = append(second, landmark, 2,
                                       node_key(x, z, direction, landmark_step));
    connect(previous, second_id);
    connect(second_id, to);

    const uint32_t branch_length = 1u + uint32_t(devils_engine::utils::mix(seed, key, 108) % 3);
    const auto branch_clearance = [&](const double branch_sign) {
      double nearest = 1e30;
      for (uint32_t step = 1; step <= branch_length; ++step) {
        point candidate = add(first, scale(sideways, branch_sign * (12.0 + 13.0 * double(step))));
        candidate = add(candidate, scale(delta, (step % 2 ? 1.0 : -1.0) * 0.05));
        for (int bz = z - 1; bz <= z + 2; ++bz) {
          for (int bx = x - 1; bx <= x + 2; ++bx) {
            nearest = std::min(nearest, length(subtract(candidate, base_position(seed, bx, bz))));
          }
        }
      }
      return nearest;
    };
    const double branch_sign = branch_clearance(-sign) > branch_clearance(sign) ? -sign : sign;
    previous = first_id;
    for (uint32_t step = 1; step <= branch_length; ++step) {
      point position = add(first, scale(sideways, branch_sign * (12.0 + 13.0 * double(step))));
      position = add(position, scale(delta, (step % 2 ? 1.0 : -1.0) * 0.05));
      position.y = std::clamp(first.y + (step % 2 ? 5.0 : -4.0),
                              double(world_y_min + 6), double(world_y_max - 6));
      const uint32_t id = append(position, theme::empty,
                                 uint8_t(step == branch_length ? 1 : 2),
                                 node_key(x, z, direction, 10, step));
      connect(previous, id);
      previous = id;
    }
  };
  for (int z = area.min_z; z < area.max_z; ++z) {
    for (int x = area.min_x; x < area.max_x; ++x) {
      if (x + 1 < area.max_x) route(x, z, 1);
      if (z + 1 < area.max_z) route(x, z, 2);
    }
  }
  return out;
}

site generate_site(const uint64_t seed, const node& at, std::vector<portal> ports,
                   const double clearance_radius) {
  if (at.kind == theme::empty) throw std::invalid_argument("empty route node has no local site");
  const site_recipe recipe = recipe_for(seed, at);
  site out{at.id, at.kind, recipe, at.centre, std::move(ports), {}, {}};
  out.rooms.push_back({at.centre, std::max(starter_sub_turn_radius +
                                            (recipe == site_recipe::nest_cave ? 1.5 : 0.0),
                                          clearance_radius + surface_guard + 0.5),
                       is_base(at.kind) ? "dock" : "main_chamber"});
  const uint32_t side_rooms = recipe == site_recipe::mining_base ? 6u :
    recipe == site_recipe::residential_base ? 6u :
    recipe == site_recipe::military_base ? 4u :
    recipe == site_recipe::nest_cave ? 4u :
    recipe == site_recipe::thermal_cave ? 3u : 3u;
  constexpr std::array<const char*, 6> ore_uses{
    "ore_vein", "crystal_shelf", "mineral_pool", "cache", "fault", "ore_pocket"};
  constexpr std::array<const char*, 6> nest_uses{
    "brood", "nursery", "feeding", "molt", "eggs", "shelter"};
  constexpr std::array<const char*, 6> thermal_uses{
    "warm_pool", "crystal", "shaft", "shelter", "hive", "cache"};
  constexpr std::array<const char*, 6> mining_uses{
    "ore_processing", "excavator", "ore_store", "power", "crew", "control"};
  constexpr std::array<const char*, 6> residential_uses{
    "commons", "hab", "hydroponics", "clinic", "lounge", "galley"};
  constexpr std::array<const char*, 6> military_uses{
    "barracks", "armory", "command", "reactor", "surveillance", "medbay"};
  for (uint32_t i = 0; i < side_rooms; ++i) {
    const double angle = 6.283185307179586 * (double(i) + 0.25 + 0.5 * random01(seed, at.key, i + 200)) /
                         double(side_rooms);
    const double offset = (recipe == site_recipe::nest_cave ? 18.0 :
                           recipe == site_recipe::ore_cave ? 22.0 : 20.0) +
                          2.0 * random01(seed, at.key, i + 300);
    const double rise = recipe == site_recipe::thermal_cave
      ? (random01(seed, at.key, i + 350) - 0.5) * 9.0
      : recipe == site_recipe::nest_cave
        ? (random01(seed, at.key, i + 350) - 0.5) * 3.0 : 0.0;
    const point centre = is_base(at.kind) ? at.centre :
      add(at.centre, {std::cos(angle) * offset, rise, std::sin(angle) * offset});
    const double radius = is_base(at.kind) ? 0.0 : clearance_radius + surface_guard +
      (recipe == site_recipe::nest_cave ? 1.1 : 0.3) +
      0.6 * random01(seed, at.key, i + 400);
    const char* purpose = recipe == site_recipe::ore_cave ? ore_uses[i] :
      recipe == site_recipe::nest_cave ? nest_uses[i] :
      recipe == site_recipe::thermal_cave ? thermal_uses[i] :
      recipe == site_recipe::mining_base ? mining_uses[i] :
      recipe == site_recipe::residential_base ? residential_uses[i] : military_uses[i];
    const std::string worn_purpose = at.kind == theme::derelict
      ? std::string("ruined_") + purpose : purpose;
    out.rooms.push_back({centre, radius, worn_purpose});
    const uint32_t index = uint32_t(out.rooms.size() - 1);
    if (!is_base(at.kind)) {
      const uint32_t parent = i > 0 && random01(seed, at.key, i + 500) < 0.5 ? index - 1 : 0;
      out.links.push_back({parent, index});
    }
  }
  if (!is_base(at.kind) && side_rooms >= 3 && random01(seed, at.key, 600) < 0.5) {
    out.links.push_back({1, side_rooms});
  }
  return out;
}

site materialize_site(const adventure& world, const uint32_t node_id) {
  const node& at = world.route.nodes.at(node_id);
  if (at.kind == theme::empty) throw std::invalid_argument("empty route node has no local site");
  std::vector<portal> ports;
  for (uint32_t i = 0; i < world.route.edges.size(); ++i) {
    const edge link = world.route.edges[i];
    if (link.a == node_id || link.b == node_id) {
      const node& other = world.route.nodes[link.a == node_id ? link.b : link.a];
      ports.push_back({i, port_position(at, other)});
    }
  }
  return generate_site(world.seed, at, std::move(ports), world.clearance_radius);
}

double local_cave_field(const uint64_t seed, const site& local,
                        const double clearance_radius, const point position) {
  if (is_base(local.kind) ||
      std::abs(position.x - local.centre.x) > 36.0 ||
      std::abs(position.y - local.centre.y) > 36.0 ||
      std::abs(position.z - local.centre.z) > 36.0) return 3.0;
  const double broad = value_noise(seed + 1, position, 11.0);
  double field = 3.0;
  for (const room& chamber : local.rooms) {
    const point q = subtract(position, chamber.centre);
    const double core = length(q) - chamber.radius;
    const double lateral = local.recipe == site_recipe::nest_cave ? 1.25 :
                           local.recipe == site_recipe::ore_cave ? 1.15 : 1.05;
    const double vertical = local.recipe == site_recipe::thermal_cave ? 1.65 :
                            local.recipe == site_recipe::ore_cave ? 0.8 : 1.05;
    const double shaped = std::sqrt((q.x / lateral) * (q.x / lateral) +
                                    (q.y / vertical) * (q.y / vertical) +
                                    (q.z / lateral) * (q.z / lateral)) -
                          chamber.radius - 0.35 * broad;
    field = std::min(field, std::min(core, shaped));
  }
  for (const portal& port : local.portals) {
    field = std::min(field, segment_distance(position, local.centre, port.centre) -
                             (clearance_radius + surface_guard));
  }
  for (const room_link link : local.links) {
    field = std::min(field, segment_distance(position, local.rooms[link.a].centre,
                                              local.rooms[link.b].centre) -
                             (clearance_radius + surface_guard));
  }
  return field;
}

adventure generate(const uint64_t seed, const graph::window area, const double clearance_radius) {
  if (!std::isfinite(clearance_radius) || clearance_radius < 0.5 || clearance_radius > 3.0) {
    throw std::invalid_argument("sphere clearance radius must be in [0.5,3] metres");
  }
  adventure world{seed, generate_graph(seed, area), clearance_radius, {}};
  // Каждая база выбирает план из одного и того же абсолютного окружения 3x3,
  // независимо от окна, которое запросил чанк или камера.
  std::vector<uint8_t> chosen(world.route.nodes.size(), 0);
  for (uint32_t i = 0; i < world.route.nodes.size(); ++i) {
    if (!is_base(world.route.nodes[i].kind)) continue;
    const node& at = world.route.nodes[i];
    const int sx = int(std::floor(at.centre.x / sector_size));
    const int sz = int(std::floor(at.centre.z / sector_size));
    adventure context{seed, generate_graph(seed, {sx - 1, sz - 1, sx + 2, sz + 2}),
                      clearance_radius, {}};
    const auto found = std::find_if(context.route.nodes.begin(), context.route.nodes.end(),
      [&](const node& candidate) { return candidate.key == at.key; });
    if (found == context.route.nodes.end()) throw std::runtime_error("canonical outpost node missing");
    context.outpost_layout.resize(context.route.nodes.size(), 0);
    const uint8_t first = uint8_t(devils_engine::utils::mix(seed, at.key, 0x535441ull) % 12u);
    uint64_t best_kept = 0, best_total = 1;
    const int8_t preferred = preferred_dock_axis(seed, at);
    const std::array<int8_t, 3> poses{preferred,
      preferred == 0 ? int8_t{1} : int8_t{0},
      preferred == -1 ? int8_t{1} : int8_t{-1}};
    const uint64_t minimum_percent = at.kind == theme::derelict ? 80 : 85;
    for (const int8_t pose : poses) {
      const uint8_t pose_slot = pose == 0 ? 0u : pose > 0 ? 1u : 2u;
      const uint8_t attempts = pose == 0 ? 12u : 4u;
      for (uint8_t attempt = 0; attempt < attempts; ++attempt) {
        const uint8_t plan = pose == 0 ? uint8_t((first + attempt) % 12u) :
          uint8_t((((first / 3u) + attempt) % 4u) * 3u + 2u);
        const uint8_t choice = uint8_t(pose_slot * 12u + plan);
        context.outpost_layout[found->id] = choice;
        const local_structure candidate = materialize_local_structure(context, found->id);
        if (pose >= 0 && outpost_body_rock_percent(context, candidate) < 80) continue;
        const std::vector<built_solid> raw = materialize_outpost_solids(candidate);
        const std::vector<built_solid> fitted = fit_outpost_solids(context, candidate);
        if (hatch_panel_volume(raw) != hatch_panel_volume(fitted)) continue;
        const uint64_t total = solid_volume(raw);
        const uint64_t kept = solid_volume(fitted);
        if (kept * best_total > best_kept * total) {
          best_kept = kept;
          best_total = total;
          chosen[i] = choice;
        }
        if (kept == total) break;
      }
      if (best_kept * 100 >= best_total * minimum_percent) break;
    }
  }
  world.outpost_layout = std::move(chosen);
  return world;
}

adventure generate_for_view(const uint64_t seed, const point observer, const double visibility,
                            const double clearance_radius) {
  if (!std::isfinite(visibility) || visibility <= 0.0 ||
      observer.y < world_y_min || observer.y > world_y_max) {
    throw std::invalid_argument("invalid view or observer height");
  }
  const int padding = int(std::ceil(visibility / sector_size)) + 2;
  const int cx = int(std::floor(observer.x / sector_size));
  const int cz = int(std::floor(observer.z / sector_size));
  return generate(seed, {cx - padding, cz - padding, cx + padding + 1, cz + padding + 1},
                  clearance_radius);
}

double cave_density(const adventure& world, const point position) {
  if (position.y < world_y_min || position.y > world_y_max) return 3.0;
  const double broad = value_noise(world.seed + 1, position, 11.0);
  const double detail = value_noise(world.seed + 2, position, 4.0);
  const double irregularity = 0.30 * broad + 0.16 * detail;
  double field = 3.0 + irregularity; // фоновая скала

  const double wall_noise = 0.85 * broad + 0.35 * detail;
  for (const edge link : world.route.edges) {
    const node& a = world.route.nodes[link.a];
    const node& b = world.route.nodes[link.b];
    field = std::min(field, cave_edge_field(world, a, b, position, wall_noise));
    // Сметённый объём капсульного корпуса при поступательном движении по ребру.
    // Участки корпуса за концами ребра попадают в поворотные сферы узлов.
    const double route_distance = segment_distance(position, a.centre, b.centre);
    field = std::min(field, route_distance - starter_sub_path_radius);
    if (route_distance <= 24.0) {
      if (const auto pocket = materialize_edge_pocket(world.seed, a, b)) {
        const double connector = segment_distance(position, pocket->mouth,
                                                   pocket->centre) -
                                 (world.clearance_radius + surface_guard);
        const double chamber = length(subtract(position, pocket->centre)) -
                               pocket->radius - 0.25 * broad;
        field = std::min(field, std::min(connector, chamber));
      }
    }
  }
  for (const node& at : world.route.nodes) {
    // В сфере помещается корпус при ЛЮБОЙ ориентации вокруг центра узла.
    const double radius = is_base(at.kind) ? base_cavern_radius :
      at.kind == theme::empty && at.rank != 1
        ? starter_sub_turn_radius + 2.5 * random01(world.seed, at.key, 601)
        : starter_sub_turn_radius;
    field = std::min(field, length(subtract(position, at.centre)) - radius);
  }
  for (const node& at : world.route.nodes) {
    if (at.kind == theme::empty || is_base(at.kind)) continue;
    if (std::abs(position.x - at.centre.x) > 36.0 ||
        std::abs(position.y - at.centre.y) > 36.0 ||
        std::abs(position.z - at.centre.z) > 36.0) continue;
    const site local = materialize_site(world, at.id);
    field = std::min(field, local_cave_field(world.seed, local,
                                             world.clearance_radius, position));
  }
  return field;
}

double density(const adventure& world, const point position) {
  double field = cave_density(world, position);
  if (position.y < world_y_min || position.y > world_y_max) return field;
  for (const node& at : world.route.nodes) {
    if (!is_base(at.kind)) continue;
    // План появляется по запросу возле узла. Решение зависит только от seed, узла и портов;
    // ранее опубликованный чанк уже получил бы ровно ту же форму.
    const double reach = 65.0;
    if (std::abs(position.x - at.centre.x) > reach ||
        std::abs(position.y - at.centre.y) > reach ||
        std::abs(position.z - at.centre.z) > reach) continue;
    // Монтажный объём станции, отделённый от естественной пещеры.
    field = std::min(field, outpost_envelope_field(materialize_local_structure(world, at.id),
                                                   position));
  }
  return field;
}

double chunk::at(const int local_x, const int local_y, const int local_z) const {
  constexpr int side = cells + 1;
  return samples[size_t((local_z * side + local_y) * side + local_x)];
}

chunk generate_chunk(const adventure& world, const int x, const int y, const int z) {
  chunk out{x, y, z, {}};
  constexpr int side = chunk::cells + 1;
  out.samples.reserve(size_t(side * side * side));
  for (int iz = 0; iz < side; ++iz) {
    for (int iy = 0; iy < side; ++iy) {
      for (int ix = 0; ix < side; ++ix) {
        out.samples.push_back(density(world, {double(x * chunk::cells + ix),
                                              double(y * chunk::cells + iy),
                                              double(z * chunk::cells + iz)}));
      }
    }
  }
  return out;
}

verification verify(const adventure& world) {
  const graph& route = world.route;
  if (!std::isfinite(world.clearance_radius) || world.clearance_radius < 0.5 ||
      world.clearance_radius > 3.0) return {false, "invalid spherical clearance radius"};
  if (route.nodes.size() < 8 || route.entry >= route.nodes.size() ||
      route.destination >= route.nodes.size()) return {false, "invalid graph size or endpoints"};
  for (const node& at : route.nodes) {
    if (at.centre.y < world_y_min + starter_sub_turn_radius ||
        at.centre.y > world_y_max - starter_sub_turn_radius) {
      return {false, "node exceeds finite Y submarine turning clearance"};
    }
  }
  uint32_t checked_samples = 0;
  const double probe_radius = world.clearance_radius + std::sqrt(3.0);
  const auto free_profile = [&](const point centre) {
    if (centre.y < world_y_min + probe_radius ||
        centre.y > world_y_max - probe_radius) return false;
    ++checked_samples;
    if (density(world, centre) >= 0.0) return false;
    for (const point axis : {point{1, 0, 0}, point{-1, 0, 0}, point{0, 1, 0},
                             point{0, -1, 0}, point{0, 0, 1}, point{0, 0, -1}}) {
      ++checked_samples;
      if (density(world, add(centre, scale(axis, probe_radius))) >= 0.0) return false;
    }
    return true;
  };
  // 26 направлений на узлах/в комнатах; вдоль отрезка достаточно осей, потому что
  // вся сферическая оболочка следует непосредственно из радиуса вырезанной капсулы.
  const auto free_sphere = [&](const point centre) {
    if (!free_profile(centre)) return false;
    for (int z = -1; z <= 1; ++z) {
      for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
          if (x * x + y * y + z * z < 2) continue;
          const double inv_length = probe_radius /
                                    std::sqrt(double(x * x + y * y + z * z));
          ++checked_samples;
          if (density(world, add(centre, {x * inv_length, y * inv_length, z * inv_length})) >= 0.0) {
            return false;
          }
        }
      }
    }
    return true;
  };
  const auto free_segment = [&](const point a, const point b) {
    const point delta = subtract(b, a);
    const uint32_t steps = std::max(1u, uint32_t(std::ceil(length(delta) * 2.0)));
    for (uint32_t step = 0; step <= steps; ++step) {
      if (!free_profile(add(a, scale(delta, double(step) / double(steps))))) return false;
    }
    return true;
  };
  const double hull_probe_radius = starter_sub_beam / 2.0 + std::sqrt(3.0);
  const double turn_probe_radius = starter_sub_length / 2.0 + std::sqrt(3.0);
  const auto free_submarine_edge = [&](const point a, const point b) {
    const point delta = subtract(b, a);
    const double span = length(delta);
    if (span <= 0.0) return false;
    const point axis = scale(delta, 1.0 / span);
    const point reference = std::abs(axis.y) < 0.85 ? point{0, 1, 0} : point{1, 0, 0};
    const point side = scale(cross(axis, reference), 1.0 / length(cross(axis, reference)));
    const point up = cross(side, axis);
    const uint32_t steps = std::max(1u, uint32_t(std::ceil(span / 2.0)));
    for (uint32_t step = 0; step <= steps; ++step) {
      const point centre = add(a, scale(delta, double(step) / double(steps)));
      for (const double station : {-starter_sub_axis_half, 0.0, starter_sub_axis_half}) {
        const point at = add(centre, scale(axis, station));
        for (const point normal : {side, scale(side, -1.0), up, scale(up, -1.0)}) {
          ++checked_samples;
          if (density(world, add(at, scale(normal, hull_probe_radius))) >= 0.0) return false;
        }
      }
      for (const double sign : {-1.0, 1.0}) {
        ++checked_samples;
        if (density(world, add(centre, scale(axis, sign *
            (starter_sub_axis_half + hull_probe_radius)))) >= 0.0) return false;
      }
    }
    return true;
  };
  const auto free_submarine_turn = [&](const point centre) {
    if (centre.y < world_y_min + starter_sub_turn_radius ||
        centre.y > world_y_max - starter_sub_turn_radius) return false;
    for (int z = -1; z <= 1; ++z) {
      for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
          const int squared = x * x + y * y + z * z;
          if (squared == 0) continue;
          const double factor = turn_probe_radius / std::sqrt(double(squared));
          ++checked_samples;
          if (density(world, add(centre, {x * factor, y * factor, z * factor})) >= 0.0) {
            return false;
          }
        }
      }
    }
    return true;
  };
  disjoint_set connected(route.nodes.size());
  std::vector<uint32_t> degree(route.nodes.size(), 0);
  std::vector<std::vector<uint32_t>> neighbours(route.nodes.size());
  double measured_route = 0.0, wide_route = 0.0;
  for (size_t edge_id = 0; edge_id < route.edges.size(); ++edge_id) {
    const edge link = route.edges[edge_id];
    if (link.a >= route.nodes.size() || link.b >= route.nodes.size() || link.a == link.b) {
      return {false, "invalid graph edge"};
    }
    for (size_t previous = 0; previous < edge_id; ++previous) {
      const edge other = route.edges[previous];
      if ((other.a == link.a && other.b == link.b) || (other.a == link.b && other.b == link.a)) {
        return {false, "duplicate graph edge"};
      }
    }
    ++degree[link.a];
    ++degree[link.b];
    neighbours[link.a].push_back(link.b);
    neighbours[link.b].push_back(link.a);
    connected.join(link.a, link.b);
    const point a = port_position(route.nodes[link.a], route.nodes[link.b]);
    const point b = port_position(route.nodes[link.b], route.nodes[link.a]);
    if (!free_segment(a, b)) return {false, "graph edge has a sealed 3D core"};
    if (!free_submarine_edge(route.nodes[link.a].centre, route.nodes[link.b].centre)) {
      return {false, "graph edge blocks the 17 x 6 m submarine"};
    }
    if (const auto pocket = materialize_edge_pocket(world.seed,
          route.nodes[link.a], route.nodes[link.b])) {
      if (!free_segment(pocket->mouth, pocket->centre) ||
          !free_sphere(pocket->centre)) {
        return {false, "decorative edge pocket has no small clear route"};
      }
    }
    const point ab = subtract(b, a);
    const double span = length(ab);
    const point axis = scale(ab, 1.0 / span);
    const point reference = std::abs(axis.y) < 0.85 ? point{0, 1, 0} : point{1, 0, 0};
    const point side = scale(cross(axis, reference), 1.0 / length(cross(axis, reference)));
    const point up = cross(side, axis);
    for (int sample = 1; sample <= 9; ++sample) {
      const point centre = add(a, scale(ab, double(sample) / 10.0));
      measured_route += span / 9.0;
      // Доля длины с реальным свободным сечением 16 x 12 м, а не доля пустого объёма.
      if (density(world, add(centre, scale(side, 8.0))) < 0.0 &&
          density(world, add(centre, scale(side, -8.0))) < 0.0 &&
          density(world, add(centre, scale(up, 6.0))) < 0.0 &&
          density(world, add(centre, scale(up, -6.0))) < 0.0) {
        wide_route += span / 9.0;
      }
    }
  }
  if (measured_route <= 0.0 || wide_route < measured_route * 0.70) {
    return {false, "wide cave passages cover less than 70 percent of route length"};
  }
  std::vector<uint32_t> base_ids;
  bool has_upward_edge = false;
  for (uint32_t i = 0; i < route.nodes.size(); ++i) {
    if (route.nodes[i].id != i || route.nodes[i].rank < 1 || route.nodes[i].rank > 10 ||
        degree[i] != route.nodes[i].rank) return {false, "declared rank disagrees with degree"};
    for (uint32_t j = 0; j < i; ++j) {
      if (route.nodes[j].key == route.nodes[i].key) return {false, "duplicate stable node key"};
    }
    if (connected.root(i) != connected.root(route.entry)) return {false, "graph is disconnected"};
    if (is_base(route.nodes[i].kind)) base_ids.push_back(i);
    if (route.nodes[i].kind == theme::empty && degree[i] == 2) {
      const point a = route.nodes[neighbours[i][0]].centre;
      const point b = route.nodes[neighbours[i][1]].centre;
      if (bend_distance(route.nodes[i].centre, a, b) < 3.0) {
        return {false, "redundant nearly straight route node"};
      }
    }
  }
  if (base_ids.size() < 2 || !is_base(route.nodes[route.entry].kind) ||
      !is_base(route.nodes[route.destination].kind)) return {false, "missing base endpoints"};

  // Неключевое место должно лежать хотя бы на одном кратчайшем пути МЕЖДУ двумя базами.
  // Его наличие само по себе не оправдывает длинный путь туда и обратно.
  std::vector<std::vector<uint32_t>> from_base;
  for (const uint32_t base : base_ids) {
    std::vector<uint32_t> distance(route.nodes.size(), UINT32_MAX);
    std::deque<uint32_t> frontier{base};
    distance[base] = 0;
    while (!frontier.empty()) {
      const uint32_t current = frontier.front();
      frontier.pop_front();
      for (const uint32_t neighbour : neighbours[current]) {
        if (distance[neighbour] != UINT32_MAX) continue;
        distance[neighbour] = distance[current] + 1;
        frontier.push_back(neighbour);
      }
    }
    from_base.push_back(std::move(distance));
  }
  for (size_t a = 0; a < base_ids.size(); ++a) {
    for (size_t b = a + 1; b < base_ids.size(); ++b) {
      if (from_base[a][base_ids[b]] <= min_base_intermediates) {
        return {false, "bases have fewer than two intermediate nodes"};
      }
    }
  }
  for (const edge link : route.edges) {
    const point a = route.nodes[link.a].centre, b = route.nodes[link.b].centre;
    if (std::abs(a.x - b.x) < 1e-9 && std::abs(a.z - b.z) < 1e-9 &&
        std::abs(a.y - b.y) >= 8.0) has_upward_edge = true;
  }
  if (!has_upward_edge) return {false, "graph has no vertical route"};
  for (uint32_t i = 0; i < route.nodes.size(); ++i) {
    if (route.nodes[i].kind == theme::empty || is_base(route.nodes[i].kind)) continue;
    bool between = false;
    for (size_t a = 0; a < base_ids.size(); ++a) {
      for (size_t b = a + 1; b < base_ids.size(); ++b) {
        if (from_base[a][i] + from_base[b][i] == from_base[a][base_ids[b]]) between = true;
      }
    }
    if (!between) return {false, "non-base site is not between two bases"};
  }

  // Снимаем неконтентные листья до неподвижной точки. Оставшаяся сеть содержит все базы и
  // альтернативные пути между ними. Каждый снятый компонент — именно тупик без базы.
  std::vector<uint8_t> active(route.nodes.size(), 1);
  std::vector<uint32_t> remaining_degree = degree;
  std::vector<uint32_t> parent(route.nodes.size(), UINT32_MAX);
  std::vector<uint32_t> removed;
  std::deque<uint32_t> leaves;
  for (uint32_t i = 0; i < route.nodes.size(); ++i) {
    if (!is_base(route.nodes[i].kind) && remaining_degree[i] <= 1) leaves.push_back(i);
  }
  while (!leaves.empty()) {
    const uint32_t leaf = leaves.front();
    leaves.pop_front();
    if (!active[leaf] || remaining_degree[leaf] > 1) continue;
    active[leaf] = 0;
    removed.push_back(leaf);
    for (const uint32_t neighbour : neighbours[leaf]) {
      if (!active[neighbour]) continue;
      parent[leaf] = neighbour;
      --remaining_degree[neighbour];
      if (!is_base(route.nodes[neighbour].kind) && remaining_degree[neighbour] <= 1) {
        leaves.push_back(neighbour);
      }
    }
  }
  std::vector<uint32_t> depth(route.nodes.size(), 0);
  uint32_t longest_deadend = 0;
  for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
    const uint32_t id = *it;
    if (parent[id] == UINT32_MAX) return {false, "dead end has no attachment"};
    depth[id] = depth[parent[id]] + 1;
    longest_deadend = std::max(longest_deadend, depth[id]);
    if (depth[id] > 5) return {false, "non-base dead end exceeds five nodes"};
  }

  for (uint32_t i = 0; i < route.nodes.size(); ++i) {
    if (route.nodes[i].kind == theme::empty) continue;
    const site local = materialize_site(world, i);
    const local_structure structure = materialize_local_structure(world, i);
    if (const std::string refusal = verify_local_structure(world, local, structure);
        !refusal.empty()) return {false, refusal};
    if (is_base(local.kind)) {
      if (const std::string refusal = verify_outpost_environment(world, structure);
          !refusal.empty()) return {false, refusal};
      const std::vector<built_solid> raw = materialize_outpost_solids(structure);
      const std::vector<built_solid> fitted = fit_outpost_solids(world, structure);
      if (hatch_panel_volume(raw) != hatch_panel_volume(fitted)) {
        return {false, "outpost route fitting removed a hatch panel"};
      }
      const uint64_t minimum_percent = local.kind == theme::derelict ? 80 : 85;
      if (solid_volume(fitted) * 100 < solid_volume(raw) * minimum_percent) {
        return {false, "outpost " + std::to_string(i) + " kept " +
          std::to_string(100 * solid_volume(fitted) / solid_volume(raw)) +
          " percent of its construction after route clearance (minimum " +
          std::to_string(minimum_percent) + ")"};
      }
      if (const std::string refusal = verify_outpost_solids(world, structure, fitted);
          !refusal.empty()) {
        return {false, refusal};
      }
      // Ось лодки параллельна v; у бокового, верхнего и нижнего стыковочных
      // вариантов путь центра разный. Полный объём задаётся непрерывным полем.
      constexpr double hull_guard = 3.0 + 1.7320508075688772;
      constexpr std::array<point, 6> probes{{{1, 0, 0}, {-1, 0, 0},
                                              {0, 1, 0}, {0, -1, 0},
                                              {0, 0, 1}, {0, 0, -1}}};
      for (uint32_t step = 0; step <= 4; ++step) {
        const double t = double(step) / 4.0;
        for (const double along : {-5.5, 0.0, 5.5}) {
          const point centre = docking_axis_point(structure, t, along);
          for (const point axis : probes) {
            if (density(world, add(centre, scale(axis, hull_guard))) >= 0.0) {
              return {false, "outpost docking approach is blocked by cave"};
            }
          }
        }
      }
    }
    const size_t minimum = is_base(local.kind) ? 5 : 3;
    const size_t maximum = is_base(local.kind) ? 7 : 5;
    if (local.node_id != i || local.rooms.size() < minimum || local.rooms.size() > maximum ||
        (!is_base(local.kind) && local.links.size() < local.rooms.size() - 1)) {
      return {false, "site does not satisfy local structure contract"};
    }
    if (!is_base(local.kind)) {
      std::vector<uint8_t> connected_rooms(local.rooms.size(), 0);
      connected_rooms[0] = 1;
      for (size_t pass = 0; pass < local.rooms.size(); ++pass) {
        for (const room_link link : local.links) {
          if (link.a >= local.rooms.size() || link.b >= local.rooms.size()) {
            return {false, "site has an invalid room link"};
          }
          if (connected_rooms[link.a]) connected_rooms[link.b] = 1;
          if (connected_rooms[link.b]) connected_rooms[link.a] = 1;
        }
      }
      if (std::find(connected_rooms.begin(), connected_rooms.end(), 0) != connected_rooms.end()) {
        return {false, "site room graph is disconnected"};
      }
    }
    uint32_t port_count = 0;
    for (uint32_t e = 0; e < route.edges.size(); ++e) {
      const edge link = route.edges[e];
      if (link.a != i && link.b != i) continue;
      ++port_count;
      const point expected = port_position(route.nodes[i], route.nodes[link.a == i ? link.b : link.a]);
      const auto found = std::find_if(local.portals.begin(), local.portals.end(),
                                      [&](const portal p) { return p.edge_index == e; });
      if (found == local.portals.end() || length(subtract(found->centre, expected)) > 1e-9) {
        return {false, "site port disagrees with graph edge"};
      }
    }
    if (port_count != local.portals.size()) return {false, "site has an unrequested port"};
    if (!is_base(local.kind)) for (const room& chamber : local.rooms) {
      if (!free_sphere(chamber.centre)) return {false, "site room has a sealed 3D core"};
    }
    for (const portal& port : local.portals) {
      if (!free_segment(local.centre, port.centre)) {
        return {false, "site port has a sealed 3D connector"};
      }
    }
    if (!is_base(local.kind)) for (const room_link link : local.links) {
      if (!free_segment(local.rooms[link.a].centre, local.rooms[link.b].centre)) {
        return {false, "site rooms have a sealed 3D connector"};
      }
    }
  }

  // Общие грани сравниваются по фактически сгенерированным чанкам, включая вертикальную грань.
  const point entry = route.nodes[route.entry].centre;
  const int cx = int(std::floor(entry.x / chunk::cells));
  const int cy = int(std::floor(entry.y / chunk::cells));
  const int cz = int(std::floor(entry.z / chunk::cells));
  const chunk centre = generate_chunk(world, cx, cy, cz);
  const chunk east = generate_chunk(world, cx + 1, cy, cz);
  const chunk north = generate_chunk(world, cx, cy, cz + 1);
  const chunk above = generate_chunk(world, cx, cy + 1, cz);
  for (int a = 0; a <= chunk::cells; ++a) {
    for (int b = 0; b <= chunk::cells; ++b) {
      if (centre.at(chunk::cells, a, b) != east.at(0, a, b) ||
          centre.at(a, b, chunk::cells) != north.at(a, b, 0) ||
          centre.at(a, chunk::cells, b) != above.at(a, 0, b)) {
        return {false, "adjacent chunk samples disagree"};
      }
    }
  }
  if (generate_chunk(world, cx, cy, cz).samples != centre.samples) {
    return {false, "chunk depends on generation order"};
  }
  uint32_t rooms = 0;
  for (uint32_t i = 0; i < route.nodes.size(); ++i) {
    if (!free_sphere(route.nodes[i].centre)) return {false, "graph node has a sealed 3D core"};
    if (!free_submarine_turn(route.nodes[i].centre)) {
      return {false, "graph node blocks submarine rotation"};
    }
    if (route.nodes[i].kind != theme::empty) rooms += uint32_t(materialize_site(world, i).rooms.size());
  }
  return {true, {}, rooms, rooms, checked_samples, longest_deadend,
          uint32_t(std::lround(100.0 * wide_route / measured_route))};
}

bool write_svg(const adventure& world, const std::string& path) {
  std::ofstream output(path);
  if (!output) return false;
  const int left = world.route.area.min_x * sector_size;
  const int top = world.route.area.min_z * sector_size;
  const int width = (world.route.area.max_x - world.route.area.min_x) * sector_size;
  const int height = (world.route.area.max_z - world.route.area.min_z) * sector_size;
  output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\""
         << left << ' ' << top << ' ' << width << ' ' << height
         << "\" width=\"1152\" height=\"1152\">\n"
         << "<rect x=\"" << left << "\" y=\"" << top << "\" width=\""
         << width << "\" height=\"" << height << "\" fill=\"#1b2024\"/>\n";
  for (int z = top; z < top + height; ++z) {
    for (int x = left; x < left + width; ++x) {
      if (density(world, {double(x), 0.0, double(z)}) < 0.0) {
        output << "<rect x=\"" << x << "\" y=\"" << z
               << "\" width=\"1\" height=\"1\" fill=\"#b4a891\"/>\n";
      }
    }
  }
  for (const edge link : world.route.edges) {
    const point a = world.route.nodes[link.a].centre;
    const point b = world.route.nodes[link.b].centre;
    if (std::abs(a.x - b.x) < 1e-9 && std::abs(a.z - b.z) < 1e-9) {
      output << "<circle cx=\"" << a.x << "\" cy=\"" << a.z
             << "\" r=\"2.2\" fill=\"none\" stroke=\"#f0d66e\" "
             << "stroke-width=\"0.5\"/>\n"
             << "<text x=\"" << a.x + 2.5 << "\" y=\"" << a.z + 2.0
             << "\" fill=\"#f0d66e\" font-size=\"2.6\">"
             << (b.y > a.y ? "up " : "down ") << std::abs(int(std::lround(b.y - a.y)))
             << "</text>\n";
    } else {
      output << "<line x1=\"" << a.x << "\" y1=\"" << a.z << "\" x2=\""
             << b.x << "\" y2=\"" << b.z << "\" stroke=\"#64717b\" "
             << "stroke-width=\"0.35\"/>\n";
    }
  }
  for (const node& at : world.route.nodes) {
    const char* colour = at.kind == theme::outpost ? "#54cfb6" :
                         at.kind == theme::derelict ? "#e6a46d" :
                         at.kind == theme::grotto ? "#80b9d8" :
                         at.kind == theme::vent ? "#b297e1" : "#60717c";
    output << "<circle cx=\"" << at.centre.x << "\" cy=\"" << at.centre.z
           << "\" r=\"" << (at.kind == theme::empty ? 0.85 : 1.7)
           << "\" fill=\"" << colour << "\"/>\n";
    if (at.kind != theme::empty) {
      output << "<text x=\"" << at.centre.x + 2 << "\" y=\"" << at.centre.z - 2
             << "\" fill=\"white\" font-size=\"3\">" << at.id << ':'
             << int(std::lround(at.centre.y)) << "</text>\n";
    }
  }
  output << "</svg>\n";
  return bool(output);
}

bool write_site_svg(const adventure& world, const uint32_t node_id, const std::string& path) {
  if (node_id >= world.route.nodes.size() || world.route.nodes[node_id].kind == theme::empty) return false;
  if (is_base(world.route.nodes[node_id].kind)) return write_structure_svg(world, node_id, path);
  std::ofstream output(path);
  if (!output) return false;
  const site local = materialize_site(world, node_id);
  output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\" "
            "width=\"960\" height=\"960\">\n"
            "<rect width=\"64\" height=\"64\" fill=\"#1b2024\"/>\n"
         << "<text x=\"1\" y=\"2\" fill=\"#d4e0e8\" font-size=\"1.1\">"
         << name(local.recipe) << " · local field</text>\n";
  constexpr double step = 0.25;
  for (int z = 0; z < 256; ++z) {
    for (int x = 0; x < 256; ++x) {
      const double local_x = (double(x) + 0.5) * step - 32.0;
      const double local_z = (double(z) + 0.5) * step - 32.0;
      if (local_cave_field(world.seed, local, world.clearance_radius,
          {local.centre.x + local_x, local.centre.y, local.centre.z + local_z}) < 0.0) {
        output << "<rect x=\"" << double(x) * step << "\" y=\"" << double(z) * step
               << "\" width=\"0.25\" height=\"0.25\" fill=\"#b4a891\"/>\n";
      }
    }
  }
  for (const room_link link : local.links) {
    const point a = local.rooms[link.a].centre;
    const point b = local.rooms[link.b].centre;
    output << "<line x1=\"" << a.x - local.centre.x + 32.0 << "\" y1=\""
           << a.z - local.centre.z + 32.0 << "\" x2=\""
           << b.x - local.centre.x + 32.0 << "\" y2=\""
           << b.z - local.centre.z + 32.0
           << "\" stroke=\"#526574\" stroke-width=\"0.08\"/>\n";
  }
  for (const portal& port : local.portals) {
    output << "<circle cx=\"" << port.centre.x - local.centre.x + 32.0
           << "\" cy=\"" << port.centre.z - local.centre.z + 32.0
           << "\" r=\"0.38\" fill=\"#66d5df\"/>\n";
  }
  for (const room& chamber : local.rooms) {
    const double x = chamber.centre.x - local.centre.x + 32.0;
    const double z = chamber.centre.z - local.centre.z + 32.0;
    output << "<circle cx=\"" << x << "\" cy=\"" << z
           << "\" r=\"0.16\" fill=\"#e6a46d\"/>\n"
           << "<text x=\"" << x + 0.24 << "\" y=\"" << z - 0.24
           << "\" fill=\"#ffffff\" font-family=\"sans-serif\" font-size=\"0.56\">"
           << chamber.purpose << "</text>\n";
  }
  output << "</svg>\n";
  return bool(output);
}

} // namespace gn09
