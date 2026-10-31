#include "adventure.h"
#include "world_chain.h"
#include "local_structure.h"
#include "station.h"
#include "tunnel.h"
#include "cave.h"

#include <algorithm>
#include <bit>
#include <mutex>
#include <map>
#include <array>
#include <cmath>
#include <deque>
#include <fstream>
#include <numeric>
#include <stdexcept>

#include "devils_engine/originator/primitive_field.h"
#include "devils_engine/utils/core.h"
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

} // namespace

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

double layer_height(const uint64_t seed, const double x, const double z) {
  const point p{x, 0.0, z};
  return 200.0 * value_noise(seed + 11, p, 1500.0) + 90.0 * value_noise(seed + 12, p, 520.0) +
         30.0 * value_noise(seed + 13, p, 190.0);
}



namespace {

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

double bend_distance(const point centre, const point a, const point b) {
  return segment_distance(centre, a, b);
}

} // namespace

// Ребро остаётся одним топологическим соединением. Ширина меняется вдоль него, а
// две смещённые полости дают асимметричные залы без дополнительных узлов графа.
// Всё, что не зависит от точки, считается один раз на область.
edge_shape shape_edge(const adventure& world, const node& from, const node& to) {
  edge_shape out;
  out.a = port_position(from, to);
  out.b = port_position(to, from);
  const point ab = subtract(out.b, out.a);
  out.distance = length(ab);
  out.core_radius = world.clearance_radius + surface_guard;
  const point middle = scale(add(from.centre, to.centre), 0.5);
  const cave_biome biome = biome_at(world.seed, middle);
  out.biome = biome;
  // Rift — высокие ущелья, chimney — узкие высокие колодцы.
  out.vertical_gain = biome == cave_biome::karst ? 0.0 :
                      biome == cave_biome::rift ? 14.0 : 22.0;
  out.biome_width = biome == cave_biome::chimney ? 0.82 : 1.0;

  const uint64_t first = std::min(from.key, to.key), second = std::max(from.key, to.key);
  const uint64_t key = devils_engine::utils::mix(world.seed, first, second, 0x43415645ull);
  out.constriction = devils_engine::utils::mix(key, 1) % 6 == 0;
  out.axis = scale(ab, 1.0 / out.distance);
  const point reference = std::abs(out.axis.y) < 0.85 ? point{0, 1, 0} : point{1, 0, 0};
  out.right = scale(cross(out.axis, reference), 1.0 / length(cross(out.axis, reference)));
  out.up = cross(out.right, out.axis);
  out.first_t = 0.25 + 0.12 * random01(world.seed, key, 2);
  out.second_t = 0.65 + 0.12 * random01(world.seed, key, 3);
  // Сужение всё ещё шире капсулы лодки с запасом на шум, иначе ядро проступало бы
  // в нём гладким цилиндром.
  out.base_width = out.constriction ? 7.4 : 9.0 + 2.0 * random01(world.seed, key, 4);
  out.first_width = 3.0 + 2.0 * random01(world.seed, key, 5);
  out.second_width = 3.0 + 2.0 * random01(world.seed, key, 6);
  out.base_roof = out.constriction ? 7.2 : 7.5 + 1.5 * random01(world.seed, key, 7);
  for (uint64_t i = 0; i < 2; ++i) {
    const double place = i == 0 ? out.first_t : out.second_t;
    const double shift = (i == 0 ? 1.0 : -1.0) *
                         (3.0 + 2.5 * random01(world.seed, key, 10 + i));
    const double rise = (random01(world.seed, key, 12 + i) - 0.5) *
                        (4.0 + out.vertical_gain);
    edge_shape::lobe& lobe = out.lobes[i];
    lobe.centre = add(add(out.a, scale(out.axis, place * out.distance)),
                      add(scale(out.right, shift), scale(out.up, rise)));
    lobe.axial = 11.0 + 4.0 * random01(world.seed, key, 14 + i);
    lobe.lateral = 10.0 + 3.0 * random01(world.seed, key, 16 + i);
    lobe.vertical = 7.0 + 2.0 * random01(world.seed, key, 18 + i) + out.vertical_gain * 0.8;
  }
  // Дальность от ядра, за которой поле ребра заведомо больше фоновой скалы.
  // Эллипсы галереи и полостей недооценивают расстояние: отрицательная и малая
  // часть лежит внутри (k - 1) * min_radius < 3.46 + шум, т.е. k < 1 + 4.7 / min.
  const double width = out.base_width + out.first_width + out.second_width;
  const double roof = out.base_roof + 4.5 + out.vertical_gain;
  const double gallery_min = std::min(out.base_width * out.biome_width, out.base_roof);
  double extent = std::max(width, roof) * (1.0 + 4.7 / gallery_min);
  for (const edge_shape::lobe& lobe : out.lobes) {
    const point offset = subtract(lobe.centre, add(out.a, scale(out.axis,
      dot(subtract(lobe.centre, out.a), out.axis))));
    extent = std::max(extent, length(offset) +
      std::max({lobe.axial, lobe.lateral, lobe.vertical}) * (1.0 + 4.7 / 7.0));
  }
  out.reach = std::max(24.0, extent + 1.0);
  return out;
}

namespace {

double cave_edge_field(const edge_shape& shape, const point position, const double wall_noise) {
  const double core = segment_distance(position, shape.a, shape.b) - shape.core_radius;
  if (core > shape.reach) return core;
  const double along = dot(subtract(position, shape.a), shape.axis);
  const double t = std::clamp(along / shape.distance, 0.0, 1.0);
  const point nearest = add(shape.a, scale(shape.axis, std::clamp(along, 0.0, shape.distance)));
  const point offset = subtract(position, nearest);
  const double side = dot(offset, shape.right), height = dot(offset, shape.up);
  const double beyond = std::max({-along, along - shape.distance, 0.0});
  const auto pulse = [](const double t, const double centre, const double span) {
    const double q = std::max(0.0, 1.0 - std::abs(t - centre) / span);
    return q * q * (3.0 - 2.0 * q);
  };
  const double first_pulse = pulse(t, shape.first_t, 0.27);
  const double second_pulse = pulse(t, shape.second_t, 0.27);
  const double width = shape.base_width + shape.first_width * first_pulse +
                       shape.second_width * second_pulse;
  const double roof = shape.base_roof + 2.0 * first_pulse + 2.5 * second_pulse +
                      shape.vertical_gain;
  const double biome_width = width * shape.biome_width;
  const double gallery = (std::sqrt((side / biome_width) * (side / biome_width) +
                                    (height / roof) * (height / roof) +
                                    (beyond / std::min(biome_width, roof)) *
                                    (beyond / std::min(biome_width, roof))) - 1.0) *
                         std::min(biome_width, roof) + (shape.constriction ? 0.25 : 1.0) * wall_noise;
  double field = std::min(core, gallery);
  for (const edge_shape::lobe& lobe : shape.lobes) {
    const point q = subtract(position, lobe.centre);
    const double axial = dot(q, shape.axis) / lobe.axial;
    const double lateral = dot(q, shape.right) / lobe.lateral;
    const double vertical = dot(q, shape.up) / lobe.vertical;
    const double value = (std::sqrt(axial * axial + lateral * lateral + vertical * vertical) - 1.0) *
                         7.0 + wall_noise;
    field = std::min(field, value);
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

const char* name(const rock_kind value) {
  switch (value) {
    case rock_kind::background: return "background";
    case rock_kind::karst: return "karst";
    case rock_kind::rift: return "rift";
    case rock_kind::chimney: return "chimney";
    case rock_kind::base_cavern: return "base_cavern";
    case rock_kind::hall: return "hall";
    case rock_kind::ore_cave: return "ore_cave";
    case rock_kind::nest_cave: return "nest_cave";
    case rock_kind::thermal_cave: return "thermal_cave";
    case rock_kind::pocket: return "pocket";
    case rock_kind::station_cut: return "station_cut";
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
  // Граф от радиуса прохода не зависит: берётся цепочка радиуса по умолчанию.
  return graph_from(*world_chain(1.0).get("graph", seed, window_key(area)));
}

site generate_site(const uint64_t seed, const node& at, std::vector<portal> ports,
                   const double clearance_radius) {
  if (at.kind == theme::empty) throw std::invalid_argument("empty route node has no local site");
  if (!is_base(at.kind)) throw std::invalid_argument("natural sites are caves, see cave_for");
  // Программа места базы — экземпляр генератора base_site (generator/base/scripts/base_site.lua).
  const auto home = sector_of(at.centre);
  const auto instance = world_chain(clearance_radius).get(
    "base_site", seed, {{uint64_t(int64_t(home[0])), uint64_t(int64_t(home[1])), at.key}});
  site out;
  out.node_id = at.id;
  out.kind = at.kind;
  out.recipe = recipe_for(seed, at);
  out.centre = at.centre;
  out.portals = std::move(ports);
  const auto centre = instance->field("base_rooms", "centre"), radius = instance->field("base_rooms", "radius"),
             purpose = instance->field("base_rooms", "purpose");
  const auto& purposes = base_purposes();
  for (size_t i = 0; i < centre.count(); ++i) {
    const auto code = size_t(purpose.get(i));
    if (code >= purposes.size()) devils_engine::utils::error{}("GN09: base room purpose {} is outside the vocabulary", code);
    out.rooms.push_back({{centre.get(i, 0), centre.get(i, 1), centre.get(i, 2)}, radius.get(i), purposes[code]});
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
  if (is_base(at.kind)) return generate_site(world.seed, at, std::move(ports), world.clearance_radius);
  // Пещера выбрана в канонической окрестности; порты — рёбра этого окна (в поле
  // пещеры они не входят: соединители лежат внутри капсулы маршрута).
  site local = *cave_for(world, node_id);
  local.node_id = node_id;
  local.portals = std::move(ports);
  return local;
}

adventure canonical_context(const uint64_t seed, const point centre,
                            const double clearance_radius) {
  return sector_context(seed, sector_of(centre)[0], sector_of(centre)[1], clearance_radius);
}

std::array<int, 2> sector_of(const point centre) {
  return {int(std::floor(centre.x / sector_size)), int(std::floor(centre.z / sector_size))};
}

adventure sector_context(const uint64_t seed, const int sx, const int sz, const double clearance_radius) {
  return generate(seed, {sx - 1, sz - 1, sx + 2, sz + 2}, clearance_radius);
}

adventure generate(const uint64_t seed, const graph::window area, const double clearance_radius) {
  if (!std::isfinite(clearance_radius) || clearance_radius < 0.5 || clearance_radius > 3.0) {
    throw std::invalid_argument("sphere clearance radius must be in [0.5,3] metres");
  }
  if (area.max_x - area.min_x < 2 || area.max_z - area.min_z < 2) {
    throw std::invalid_argument("graph window must contain at least 2x2 sectors");
  }
  // Мир окна — объект хоста генератора `window`: граф из скриптов макрографа и залы регионов окна.
  const auto instance = world_chain(clearance_radius).get("window", seed, window_key(area));
  return *world_view(*instance);
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

namespace {

// Ребро влияет на поле не дальше 24 м от ядра (ранний выход cave_edge_field) и
// 24 м от оси маршрута (ворота кармана); дальше его слагаемые больше фоновой скалы.
constexpr double edge_reach = 24.0;
constexpr double max_edge_reach = 80.0;
// Неровность ядер наружу: капсула лодки до +2 * core_relief, сфера узла до +2 * node_relief.
constexpr double core_relief = 1.5;
constexpr double node_relief = 2.5;
// Самая большая сфера узла (полость базы) с неровностью плюс фон скалы 3.46 с запасом.
constexpr double node_reach = base_cavern_radius + 2.0 * node_relief + 4.5;
// Совпадают с проверками в точке ниже: вне куба место и база не считаются вовсе.
constexpr double site_reach = cave_reach; // пещера места целиком в этом кубе (cave.h)
constexpr double base_reach = 65.0;

bool overlaps(const point low, const point high, const point a, const point b,
              const double margin) {
  return std::min(a.x, b.x) - margin <= high.x && std::max(a.x, b.x) + margin >= low.x &&
         std::min(a.y, b.y) - margin <= high.y && std::max(a.y, b.y) + margin >= low.y &&
         std::min(a.z, b.z) - margin <= high.z && std::max(a.z, b.z) + margin >= low.z;
}

double node_radius(const adventure& world, const node& at) {
  return is_base(at.kind) ? base_cavern_radius :
    at.kind == theme::empty && at.rank != 1
      ? starter_sub_turn_radius + 2.5 * random01(world.seed, at.key, 601)
      : starter_sub_turn_radius;
}

} // namespace

namespace {

namespace originator = devils_engine::originator;

rock_kind biome_rock(const cave_biome biome) {
  return biome == cave_biome::karst ? rock_kind::karst :
         biome == cave_biome::rift ? rock_kind::rift : rock_kind::chimney;
}

rock_kind site_rock(const site_recipe recipe) {
  return recipe == site_recipe::nest_cave ? rock_kind::nest_cave :
         recipe == site_recipe::thermal_cave ? rock_kind::thermal_cave : rock_kind::ore_cave;
}


originator::field_vector to_field(const point p) { return {p.x, p.y, p.z}; }
point from_field(const originator::field_vector& p) { return {p[0], p[1], p[2]}; }

originator::field_accessor environment_field(originator::buffer& target, const std::string_view name) {
  const size_t index = target.find_field(name);
  if (index == originator::buffer_layout::npos) {
    devils_engine::utils::error{}("GN09: buffer '{}' has no field '{}'", target.name(), name);
  }
  return target.field(index);
}

void set3(const originator::field_accessor& field, const size_t i, const point p) {
  field.set(i, p.x, 0);
  field.set(i, p.y, 1);
  field.set(i, p.z, 2);
}

point get3(const originator::const_field_accessor& field, const size_t i) {
  return {field.get(i, 0), field.get(i, 1), field.get(i, 2)};
}

// Строка галереи: форма ребра, центры его узлов (капсула хода) и карман.
struct gallery_row {
  edge_shape shape;
  point a, b;
  std::optional<edge_pocket> pocket;
};

struct turn_row {
  point centre;
  double radius = 0.0;
  rock_kind rock = rock_kind::background;
};

// Пещера природного места целиком: одна строка на место, коробка — коробка места.
using site_row = std::shared_ptr<const site>;

using station_row = std::shared_ptr<const built_station>;
using tunnel_row = std::shared_ptr<const built_tunnel>;

// Коробка влияния галереи: вне обеих капсул (формы и хода с карманом) поле ребра больше фоновой
// скалы — то же отсечение, что у prepare_field.
originator::field_box gallery_box(const gallery_row& row) {
  const auto form = originator::field_box::around(to_field(row.shape.a), to_field(row.shape.b),
                                                  row.shape.reach + row.shape.core_radius + 1.0);
  const auto route = originator::field_box::around(to_field(row.a), to_field(row.b), edge_reach + 1.0);
  originator::field_box box;
  for (size_t k = 0; k < 3; ++k) {
    box.low[k] = std::min(form.low[k], route.low[k]);
    box.high[k] = std::max(form.high[k], route.high[k]);
  }
  return box;
}

// Строки окна объектом: ОДНИ И ТЕ ЖЕ для буферов окна (write_environment) и для полей, собранных
// хостом (prepare_field). Формулы — в словаре ниже.
gallery_row make_gallery_row(const adventure& world, const node& a, const node& b, edge_shape shape) {
  return gallery_row{std::move(shape), a.centre, b.centre, materialize_edge_pocket(world.seed, a, b)};
}

turn_row make_turn_row(const adventure& world, const node& at) {
  return turn_row{at.centre, node_radius(world, at),
                  is_base(at.kind) ? rock_kind::base_cavern : biome_rock(biome_at(world.seed, at.centre))};
}

originator::field_box turn_box(const turn_row& row) {
  return originator::field_box::around(to_field(row.centre), to_field(row.centre), node_reach);
}

template <typename thing_t>
originator::field_box thing_box(const thing_t& thing) {
  return originator::field_box{to_field(thing.low), to_field(thing.high)};
}

constexpr size_t slot_broad = 0, slot_detail = 1;

originator::shape_vocabulary make_world_shapes(const double clearance_radius) {
  originator::shape_vocabulary words;
  words.add_slot("broad", [](originator::field_point& p) {
    return value_noise(p.seed() + 1, from_field(p.position()), 11.0);
  });
  words.add_slot("detail", [](originator::field_point& p) {
    return value_noise(p.seed() + 2, from_field(p.position()), 4.0);
  });
  // Фоновая скала 3 + неровность (<= 3.46); вне высоты мира — скала, какие бы формы ни касались.
  words.set_background(
    [](originator::field_point& p, bool& stop) {
      const double y = p.position()[1];
      if (y < world_y_min || y > world_y_max) {
        stop = true;
        return originator::shape_value{3.0, uint32_t(rock_kind::background)};
      }
      const double irregularity = 0.30 * p.slot(slot_broad) + 0.16 * p.slot(slot_detail);
      return originator::shape_value{3.0 + irregularity, uint32_t(rock_kind::background)};
    },
    3.46);

  words.add_kind<gallery_row>(
    "gallery",
    [](const originator::generator_result& instance) {
      std::vector<std::pair<gallery_row, originator::field_box>> rows;
      const auto count = size_t(instance.field("environment_counts", "galleries").get(0));
      const auto f = [&](const std::string_view name) { return instance.field("galleries", name); };
      const auto ends_a = f("ends_a"), ends_b = f("ends_b"), port_a = f("port_a"), port_b = f("port_b"),
                 axis = f("axis"), right = f("right"), up = f("up"), distance = f("distance"),
                 core_radius = f("core_radius"), reach = f("reach"), first_t = f("first_t"), second_t = f("second_t"),
                 base_width = f("base_width"), first_width = f("first_width"), second_width = f("second_width"),
                 base_roof = f("base_roof"), vertical_gain = f("vertical_gain"), biome_width = f("biome_width"),
                 constriction = f("constriction"), biome = f("biome"), lobe0 = f("lobe0"), lobe0_size = f("lobe0_size"),
                 lobe1 = f("lobe1"), lobe1_size = f("lobe1_size"), pocket = f("pocket"),
                 pocket_mouth = f("pocket_mouth"), pocket_centre = f("pocket_centre"),
                 pocket_radius = f("pocket_radius");
      rows.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        gallery_row row;
        edge_shape& s = row.shape;
        row.a = get3(ends_a, i);
        row.b = get3(ends_b, i);
        s.a = get3(port_a, i);
        s.b = get3(port_b, i);
        s.axis = get3(axis, i);
        s.right = get3(right, i);
        s.up = get3(up, i);
        s.distance = distance.get(i);
        s.core_radius = core_radius.get(i);
        s.reach = reach.get(i);
        s.first_t = first_t.get(i);
        s.second_t = second_t.get(i);
        s.base_width = base_width.get(i);
        s.first_width = first_width.get(i);
        s.second_width = second_width.get(i);
        s.base_roof = base_roof.get(i);
        s.vertical_gain = vertical_gain.get(i);
        s.biome_width = biome_width.get(i);
        s.constriction = constriction.get(i) != 0.0;
        s.biome = cave_biome(uint8_t(biome.get(i)));
        const point size0 = get3(lobe0_size, i), size1 = get3(lobe1_size, i);
        s.lobes[0] = {get3(lobe0, i), size0.x, size0.y, size0.z};
        s.lobes[1] = {get3(lobe1, i), size1.x, size1.y, size1.z};
        if (pocket.get(i) != 0.0) row.pocket = edge_pocket{get3(pocket_mouth, i), get3(pocket_centre, i), pocket_radius.get(i)};
        const auto box = gallery_box(row);
        rows.emplace_back(std::move(row), box);
      }
      return rows;
    },
    [](const gallery_row& row, originator::field_point& p) {
      const point position = from_field(p.position());
      const double broad = p.slot(slot_broad), detail = p.slot(slot_detail);
      const double wall_noise = 0.85 * broad + 0.35 * detail;
      const auto gallery = uint32_t(biome_rock(row.shape.biome));
      originator::shape_value best{cave_edge_field(row.shape, position, wall_noise), gallery};
      // Сметённый объём корпуса по ребру; неровность только наружу.
      const double route_distance = segment_distance(position, row.a, row.b);
      const double core = route_distance - starter_sub_path_radius - core_relief * (1.0 - broad);
      if (core < best.value) best = {core, gallery};
      if (route_distance <= edge_reach && row.pocket) {
        const uint64_t seed = p.seed();
        const double relief = cave_wall_relief(seed, position);
        const point warped = cave_warp(seed, position);
        const double connector = segment_distance(warped, row.pocket->mouth, row.pocket->centre) -
                                 row.shape.core_radius - cave_warp_amount - relief;
        const double chamber = length(subtract(warped, row.pocket->centre)) - row.pocket->radius - cave_warp_amount - relief;
        const double value = std::min(connector, chamber);
        if (value < best.value) best = {value, uint32_t(rock_kind::pocket)};
      }
      return best;
    });

  words.add_kind<turn_row>(
    "turn",
    [](const originator::generator_result& instance) {
      std::vector<std::pair<turn_row, originator::field_box>> rows;
      const auto count = size_t(instance.field("environment_counts", "turns").get(0));
      const auto centre = instance.field("turns", "centre"), radius = instance.field("turns", "radius"),
                 rock = instance.field("turns", "rock");
      for (size_t i = 0; i < count; ++i) {
        turn_row row{get3(centre, i), radius.get(i), rock_kind(uint8_t(rock.get(i)))};
        rows.emplace_back(row, turn_box(row));
      }
      return rows;
    },
    [](const turn_row& row, originator::field_point& p) {
      // В сфере помещается корпус при ЛЮБОЙ ориентации вокруг центра узла.
      const double broad = p.slot(slot_broad);
      return originator::shape_value{length(subtract(from_field(p.position()), row.centre)) - row.radius -
                                       node_relief * (1.0 - broad),
                                     uint32_t(row.rock)};
    });

  words.add_kind<hall>(
    "hall",
    [](const originator::generator_result& instance) {
      // Зал пока целиком C++ (ниши, уступы, петля), поэтому строки — из объекта хоста окна.
      std::vector<std::pair<hall, originator::field_box>> rows;
      const auto world = world_view(instance);
      for (const hall& h : world->halls) {
        point low, high;
        hall_influence(h, low, high);
        rows.emplace_back(h, originator::field_box{to_field(low), to_field(high)});
      }
      return rows;
    },
    [](const hall& h, originator::field_point& p) {
      return originator::shape_value{hall_field(p.seed(), h, from_field(p.position())), uint32_t(rock_kind::hall)};
    });
  // Пещера места — объект экземпляра генератора cave (весь site, одна строка с коробкой места).
  words.add_kind<site_row>(
    "site",
    [clearance_radius](const originator::generator_result& instance) {
      auto local = site_view(instance, clearance_radius);
      std::vector<std::pair<site_row, originator::field_box>> rows;
      const auto box = thing_box(*local);
      rows.emplace_back(std::move(local), box);
      return rows;
    },
    [clearance_radius](const site_row& row, originator::field_point& p) {
      return originator::shape_value{local_cave_field(p.seed(), *row, clearance_radius, from_field(p.position())),
                                     uint32_t(site_rock(row->recipe))};
    });
  // Станция — экземпляр генератора station, туннель — экземпляр генератора tunnel (по строке на
  // построенный экземпляр; непостроенный не даёт строк).
  words.add_kind<station_row>(
    "station",
    [](const originator::generator_result& instance) {
      std::vector<std::pair<station_row, originator::field_box>> rows;
      if (auto station = station_view(instance)) rows.emplace_back(station, thing_box(*station));
      return rows;
    },
    [](const station_row& row, originator::field_point& p) {
      return originator::shape_value{station_carve_field(*row, from_field(p.position())), uint32_t(rock_kind::station_cut)};
    });
  words.add_kind<tunnel_row>(
    "tunnel",
    [](const originator::generator_result& instance) {
      std::vector<std::pair<tunnel_row, originator::field_box>> rows;
      if (auto tunnel = tunnel_view(instance)) rows.emplace_back(tunnel, thing_box(*tunnel));
      return rows;
    },
    [](const tunnel_row& row, originator::field_point& p) {
      return originator::shape_value{tunnel_carve_field(*row, from_field(p.position())), uint32_t(rock_kind::station_cut)};
    });
  return words;
}

} // namespace

structures structures_near(const adventure& world, const point low, const point high) {
  structures found;
  // Вырез станции — точная коробка: вне неё поле станции не опускается ниже фоновой скалы, и
  // отброшенное слагаемое минимума ничего не меняет.
  for (const node& at : world.route.nodes) {
    if (!is_base(at.kind) || !overlaps(low, high, at.centre, at.centre, station_reach)) continue;
    if (auto station = station_for(world, at.id); station && station_reaches(*station, low, high)) {
      found.stations.push_back(std::move(station));
    }
  }
  for (const hall& h : world.halls) {
    for (const station_anchor& anchor : hall_station_anchors(world.seed, h)) {
      if (!overlaps(low, high, anchor.centre, anchor.centre, station_reach)) continue;
      if (auto station = station_for(world, anchor); station && station_reaches(*station, low, high)) {
        found.stations.push_back(std::move(station));
      }
    }
  }
  found.tunnels = tunnels_near(world, low, high);
  return found;
}

field_region prepare_field(const adventure& world, const point low, const point high,
                           const bool with_structures, const bool with_sites) {
  field_region region;
  region.world = &world;
  region.low = low;
  region.high = high;
  region.structures = with_structures;
  // Строки членов поля в порядке слагаемых минимума: галереи по рёбрам, повороты узлов, пещеры мест,
  // залы, вырезы станций, туннели. Порядок решает только метку при равных значениях.
  std::vector<std::pair<gallery_row, originator::field_box>> galleries;
  std::vector<std::pair<turn_row, originator::field_box>> turns;
  std::vector<std::pair<site_row, originator::field_box>> sites;
  std::vector<std::pair<hall, originator::field_box>> halls;
  std::vector<std::pair<station_row, originator::field_box>> stations;
  std::vector<std::pair<tunnel_row, originator::field_box>> tunnels;
  for (uint32_t i = 0; i < world.route.edges.size(); ++i) {
    const node& a = world.route.nodes[world.route.edges[i].a];
    const node& b = world.route.nodes[world.route.edges[i].b];
    // Грубый отбор до расчёта формы: дальность любого ребра не больше 70 м от его
    // портов, а порты не дальше 8 м от центров узлов (оценка по максимальным размерам
    // галереи и полостей в shape_edge).
    if (!overlaps(low, high, a.centre, b.centre, max_edge_reach)) continue;
    edge_shape shape = shape_edge(world, a, b);
    const double core_reach = shape.reach + shape.core_radius + 1.0;
    if (!overlaps(low, high, shape.a, shape.b, core_reach) &&
        !overlaps(low, high, a.centre, b.centre, edge_reach + 1.0)) continue;
    region.edges.push_back(i);
    gallery_row row = make_gallery_row(world, a, b, std::move(shape));
    const auto box = gallery_box(row);
    galleries.emplace_back(std::move(row), box);
  }
  for (uint32_t i = 0; i < world.halls.size(); ++i) {
    if (!hall_reaches(world.halls[i], low, high)) continue;
    region.halls.push_back(i);
    point hall_low, hall_high;
    hall_influence(world.halls[i], hall_low, hall_high);
    halls.emplace_back(world.halls[i], originator::field_box{to_field(hall_low), to_field(hall_high)});
  }
  for (const node& at : world.route.nodes) {
    if (overlaps(low, high, at.centre, at.centre, node_reach)) {
      region.nodes.push_back(at.id);
      const turn_row row = make_turn_row(world, at);
      turns.emplace_back(row, turn_box(row));
    }
    if (with_sites && at.kind != theme::empty && !is_base(at.kind) &&
        overlaps(low, high, at.centre, at.centre, site_reach)) {
      site_row local = cave_for(world, at.id);
      if (overlaps(low, high, local->low, local->high, 0.0)) {
        const auto box = thing_box(*local);
        sites.emplace_back(std::move(local), box);
        region.sites += 1;
      }
    }
  }
  if (with_structures) {
    const structures found = structures_near(world, low, high);
    for (const auto& station : found.stations) stations.emplace_back(station, thing_box(*station));
    for (const auto& tunnel : found.tunnels) tunnels.emplace_back(tunnel, thing_box(*tunnel));
  }
  region.stations = stations.size();
  region.tunnels = tunnels.size();
  const originator::field_term_rows terms[] = {
    {"gallery", originator::shape_vocabulary::pack(std::move(galleries))},
    {"turn", originator::shape_vocabulary::pack(std::move(turns))},
    {"site", originator::shape_vocabulary::pack(std::move(sites))},
    {"hall", originator::shape_vocabulary::pack(std::move(halls))},
    {"station", originator::shape_vocabulary::pack(std::move(stations))},
    {"tunnel", originator::shape_vocabulary::pack(std::move(tunnels))},
  };
  region.field = std::make_shared<const originator::primitive_field>(world_shapes(world.clearance_radius), terms,
                                                                     world.seed);
  region.view = std::make_shared<const originator::primitive_field::region>(
    region.field->prepare(to_field(low), to_field(high)));
  return region;
}

field_region prepare_chunk_field(const adventure& world, const int x, const int y, const int z) {
  const double side = chunk::cells;
  return prepare_field(world, {x * side - 1.0, y * side - 1.0, z * side - 1.0},
                       {(x + 1) * side + 1.0, (y + 1) * side + 1.0, (z + 1) * side + 1.0});
}

// Поле — одно, из словаря форм (world_shapes): минимум, отсечение по коробкам и метку считает
// библиотека. Поле «до построек» — то же поле области, собранной без станций и туннелей.
double cave_density(const field_region& region, const point position) {
  if (region.structures) {
    devils_engine::utils::error{}("GN09: cave_density asked of a region prepared with structures");
  }
  return region.view->value(to_field(position)).value;
}

double density(const field_region& region, const point position) {
  return region.view->value(to_field(position)).value;
}

rock_kind rock_at(const field_region& region, const point position) {
  return rock_kind(uint8_t(region.view->value(to_field(position)).tag));
}

double field_cache::operator()(const point p) {
  if (!region_ || p.x < region_->low.x || p.x > region_->high.x || p.y < region_->low.y ||
      p.y > region_->high.y || p.z < region_->low.z || p.z > region_->high.z) {
    region_ = prepare_field(*world_, {p.x - half_, p.y - half_, p.z - half_},
                            {p.x + half_, p.y + half_, p.z + half_}, structures_, sites_);
  }
  return density(*region_, p);
}

int uniform_sign(const field_region& region) {
  if (region.field->shape_count() == 0) return 1;
  for (const uint32_t id : region.halls) {
    if (hall_contains_air(region.world->halls[id], region.low, region.high)) return -1;
  }
  return 0;
}

double cave_density(const adventure& world, const point position) {
  return cave_density(prepare_field(world, position, position, false), position);
}

double density(const adventure& world, const point position) {
  return density(prepare_field(world, position, position), position);
}

double chunk::at(const int local_x, const int local_y, const int local_z) const {
  constexpr int side = cells + 1;
  return samples[size_t((local_z * side + local_y) * side + local_x)];
}

chunk generate_chunk(const field_region& region, const int x, const int y, const int z) {
  chunk out{x, y, z, {}};
  constexpr int side = chunk::cells + 1;
  out.samples.reserve(size_t(side * side * side));
  for (int iz = 0; iz < side; ++iz) {
    for (int iy = 0; iy < side; ++iy) {
      for (int ix = 0; ix < side; ++ix) {
        out.samples.push_back(density(region, {double(x * chunk::cells + ix),
                                               double(y * chunk::cells + iy),
                                               double(z * chunk::cells + iz)}));
      }
    }
  }
  return out;
}

chunk_instance chunk_from_chain(const uint64_t seed, const double clearance_radius, const int x, const int y,
                                const int z) {
  auto& chain = world_chain(clearance_radius);
  const originator::instance_key key{{uint64_t(int64_t(x)), uint64_t(int64_t(y)), uint64_t(int64_t(z))}};
  chunk_instance out;
  out.instance = chain.get("chunk_field", seed, key);
  const auto home = chain.get("chunk", seed, key);
  const auto area = home->field("home", "area");
  out.window = chain.get("window", seed, {{originator::read_key_word(area, 0, 0), originator::read_key_word(area, 0, 1),
                                           originator::read_key_word(area, 0, 2)}});
  const double side = chunk::cells;
  out.region.world = world_view(*out.window).get();
  out.region.low = {x * side - 1.0, y * side - 1.0, z * side - 1.0};
  out.region.high = {(x + 1) * side + 1.0, (y + 1) * side + 1.0, (z + 1) * side + 1.0};
  out.region.structures = true;
  out.region.field = out.instance->shape_field_handle("solid");
  out.region.view = std::make_shared<const originator::primitive_field::region>(
    out.region.field->prepare(to_field(out.region.low), to_field(out.region.high)));
  // Решётка генератора с полосой в один узел с каждой стороны; объём чанка — её внутренние узлы.
  const auto samples = out.instance->field("samples", "value");
  constexpr size_t side_nodes = chunk::cells + 1, padded = chunk::cells + 3;
  if (samples.count() != padded * padded * padded) {
    devils_engine::utils::error{}("GN09: chunk_field gave {} samples, the lattice has {}", samples.count(),
                                  padded * padded * padded);
  }
  out.volume = {x, y, z, {}};
  out.volume.samples.reserve(side_nodes * side_nodes * side_nodes);
  for (size_t iz = 1; iz <= side_nodes; ++iz) for (size_t iy = 1; iy <= side_nodes; ++iy)
    for (size_t ix = 1; ix <= side_nodes; ++ix) out.volume.samples.push_back(samples.get(ix + padded * (iy + padded * iz)));
  return out;
}

chunk generate_chunk(const adventure& world, const int x, const int y, const int z) {
  return generate_chunk(prepare_chunk_field(world, x, y, z), x, y, z);
}

// Станция проверяется в канонической окрестности якоря, где она и выбиралась: окно
// обрезает рёбра у края и могло бы показать больше породы, чем есть в мире.
std::string verify_built_station(const adventure& world, const built_station& station) {
  const point anchor_centre = station.anchor.centre;
  adventure context = canonical_context(world.seed, anchor_centre, world.clearance_radius);
  if (const std::string refusal = verify_station_plan(context, station.anchor, station.plan);
      !refusal.empty()) return refusal;
  // Итоговое поле той же окрестности со станцией: внутренности помещений вырезаны,
  // путь лодки открыт. Окно для этого не годится — причал бывает в десятках метров
  // от узла, а у края окна рёбра соседних секторов обрезаны и оставляют лишнюю скалу.
  field_cache final_field(context, true);
  const auto field = [&](const point p) { return final_field(p); };
  for (const station_room& r : station.plan.rooms) {
    for (int32_t u = r.u0; u < r.u1; u += 2) for (int32_t v = r.v0; v < r.v1; v += 2) {
      for (const double y : {0.25, 1.5, 2.75}) {
        const point p = station_to_world(station.plan.site, (u + 0.5) * station_cell, y,
                                         (v + 0.5) * station_cell);
        if (station_room_quarters(r, u, v) == 0xF && field(p) >= 0.0) {
          return "station room " + r.purpose + " is not carved out of rock";
        }
      }
    }
  }
  // Путь лодки от поворотной точки к причалу открыт в итоговом поле.
  constexpr double hull_guard = 3.0 + 1.7320508075688772;
  constexpr std::array<point, 6> probes{{{1, 0, 0}, {-1, 0, 0},
                                          {0, 1, 0}, {0, -1, 0},
                                          {0, 0, 1}, {0, 0, -1}}};
  const station_site& site = station.plan.site;
  for (uint32_t step = 0; step <= 4; ++step) {
    const point centre = add(anchor_centre, scale(subtract(site.berth, anchor_centre), step / 4.0));
    for (const double along : {-starter_sub_axis_half, 0.0, starter_sub_axis_half}) {
      for (const point axis : probes) {
        if (field(add(add(centre, scale(site.berth_axis, along)), scale(axis, hull_guard))) >= 0.0) {
          return "station docking approach is blocked by cave";
        }
      }
    }
  }
  return {};
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
  uint32_t result_bases_without_station = 0;
  uint32_t result_caves = 0, result_sites_without_cave = 0;
  std::array<uint32_t, 4> result_cave_soft{};
  // Проверочные точки идут подряд вдоль рёбер и сфер: одна подготовленная область
  // (куб 32 м) обслуживает соседние точки. Внутри области поле то же, что поточечное.
  std::optional<field_region> cached;
  const auto field = [&](const point p) {
    if (!cached || p.x < cached->low.x || p.x > cached->high.x || p.y < cached->low.y ||
        p.y > cached->high.y || p.z < cached->low.z || p.z > cached->high.z) {
      cached = prepare_field(world, subtract(p, {16, 16, 16}), add(p, {16, 16, 16}));
    }
    return density(*cached, p);
  };
  const double probe_radius = world.clearance_radius + std::sqrt(3.0);
  const auto free_profile = [&](const point centre) {
    if (centre.y < world_y_min + probe_radius ||
        centre.y > world_y_max - probe_radius) return false;
    ++checked_samples;
    if (field(centre) >= 0.0) return false;
    for (const point axis : {point{1, 0, 0}, point{-1, 0, 0}, point{0, 1, 0},
                             point{0, -1, 0}, point{0, 0, 1}, point{0, 0, -1}}) {
      ++checked_samples;
      if (field(add(centre, scale(axis, probe_radius))) >= 0.0) return false;
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
          if (field(add(centre, {x * inv_length, y * inv_length, z * inv_length})) >= 0.0) {
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
          if (field(add(at, scale(normal, hull_probe_radius))) >= 0.0) return false;
        }
      }
      for (const double sign : {-1.0, 1.0}) {
        ++checked_samples;
        if (field(add(centre, scale(axis, sign *
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
          if (field(add(centre, {x * factor, y * factor, z * factor})) >= 0.0) {
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
      if (field(add(centre, scale(side, 8.0))) < 0.0 &&
          field(add(centre, scale(side, -8.0))) < 0.0 &&
          field(add(centre, scale(up, 6.0))) < 0.0 &&
          field(add(centre, scale(up, -6.0))) < 0.0) {
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
  // Окно может почти целиком лежать в зале; тогда баз меньше двух, но если они
  // есть, вход и цель обязаны быть базами.
  if (!base_ids.empty() && (!is_base(route.nodes[route.entry].kind) ||
      !is_base(route.nodes[route.destination].kind))) return {false, "missing base endpoints"};

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
    if (!is_base(local.kind)) {
      const local_structure structure = materialize_local_structure(world, i);
      if (const std::string refusal = verify_local_structure(world, local, structure);
          !refusal.empty()) return {false, refusal};
    } else if (const auto built = station_for(world, i)) {
      if (const std::string refusal = verify_built_station(world, *built); !refusal.empty()) {
        return {false, "base " + std::to_string(i) + ": " + refusal};
      }
    } else {
      ++result_bases_without_station;
    }
    // База: программа из 5..7 помещений. Природное место: пещера (устье, ствол, зал,
    // ветка гнезда — от 9 комнат) или, если ей не нашлось места, одна полость узла.
    const size_t minimum = is_base(local.kind) ? 5 : local.has_cave ? 9 : 1;
    const size_t maximum = is_base(local.kind) ? 7 : local.has_cave ? 200 : 1;
    if (local.node_id != i || local.rooms.size() < minimum || local.rooms.size() > maximum ||
        (!is_base(local.kind) && local.links.size() < local.rooms.size() - 1)) {
      return {false, "site does not satisfy local structure contract"};
    }
    if (!is_base(local.kind)) {
      if (local.has_cave) ++result_caves; else ++result_sites_without_cave;
      const cave_check check = verify_cave(world, local);
      if (!check.refusal.empty()) return {false, "site " + std::to_string(i) + ": " + check.refusal};
      // Кэш цепочки отдаёт то же, что посчитала бы чистая функция (экземпляр пересобирается мимо кэша),
      // и природное поле из буферов окна совпадает с полем, собранным хостом.
      if (const std::string differs = verify_cave_instance(world, i); !differs.empty()) {
        return {false, "site " + std::to_string(i) + ": cave instance check: " + differs};
      }
      // Мягкие обещания пещер считаются; доля нарушений проверяется по всему прогону.
      for (uint32_t bit = 0; bit < 4; ++bit) result_cave_soft[bit] += (check.soft >> bit) & 1u;
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

  // Аванпосты залов: та же проверка; место находится не у каждого уступа. Проверяются
  // якоря внутри секторов окна: залы, лишь дотянувшиеся до окна, принадлежат соседям.
  const auto in_window = [&](const station_anchor& anchor) {
    const graph::window& area = route.area;
    return anchor.centre.x >= area.min_x * double(sector_size) && anchor.centre.x < area.max_x * double(sector_size) &&
           anchor.centre.z >= area.min_z * double(sector_size) && anchor.centre.z < area.max_z * double(sector_size);
  };
  uint32_t hall_stations = 0;
  for (const hall& h : world.halls) {
    for (const station_anchor& anchor : hall_station_anchors(world.seed, h)) {
      if (!in_window(anchor)) continue;
      const auto built = station_for(world, anchor);
      if (!built) continue;
      ++hall_stations;
      if (const std::string refusal = verify_built_station(world, *built); !refusal.empty()) {
        return {false, "hall outpost " + std::to_string(anchor.key) + ": " + refusal};
      }
    }
  }

  // Туннели между станциями окна: каждый один раз.
  uint32_t tunnels = 0;
  std::vector<uint64_t> seen_tunnels;
  for (const station_anchor& anchor : station_anchors(world)) {
    if (!in_window(anchor)) continue;
    const auto tunnel = tunnel_for(world, anchor);
    if (!tunnel || std::find(seen_tunnels.begin(), seen_tunnels.end(), tunnel->key) != seen_tunnels.end()) continue;
    seen_tunnels.push_back(tunnel->key);
    ++tunnels;
    if (const std::string refusal = verify_tunnel(world, *tunnel); !refusal.empty()) {
      return {false, "tunnel " + std::to_string(tunnel->key) + ": " + refusal};
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
  // Залы — чистая функция региона: каждый обязан совпасть со своим пересчётом
  // и пройти независимую проверку формы, горловины и коридора.
  const point window_low{double(route.area.min_x * sector_size), double(world_y_min),
                         double(route.area.min_z * sector_size)};
  const point window_high{double(route.area.max_x * sector_size), double(world_y_max),
                          double(route.area.max_z * sector_size)};
  for (const hall& h : world.halls) {
    if (!hall_reaches(h, window_low, window_high)) continue;
    const std::vector<hall> again = region_halls(world.seed, h.region_x, h.region_z);
    const bool same = std::any_of(again.begin(), again.end(), [&](const hall& other) {
      return other.key == h.key && other.centre.x == h.centre.x &&
             other.centre.y == h.centre.y && other.centre.z == h.centre.z &&
             other.pillars.size() == h.pillars.size() && other.niches.size() == h.niches.size();
    });
    if (!same) return {false, "hall differs from its region"};
    const std::string cause = verify_hall(world.seed, h, world.clearance_radius);
    if (!cause.empty()) return {false, cause};
    const std::string creature = verify_creature(world.seed, h, creature_of(world.seed, h));
    if (!creature.empty()) return {false, creature};
  }
  verification out{true, {}, rooms, rooms, checked_samples, longest_deadend,
                   uint32_t(std::lround(100.0 * wide_route / measured_route))};
  out.halls = uint32_t(world.halls.size());
  out.bases_without_station = result_bases_without_station;
  out.caves = result_caves;
  out.sites_without_cave = result_sites_without_cave;
  out.cave_soft = result_cave_soft;
  out.hall_stations = hall_stations;
  out.tunnels = tunnels;
  return out;
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
  // Вид сверху на скелет пещеры: кишки шириной ядра, камеры своими радиусами. Цвет —
  // роль: гнездо красное, тупики с залежами жёлтые, горловины-пробки голубые.
  const double core = cave_core(world.clearance_radius);
  const double half = cave_reach;
  const auto x_of = [&](const point p) { return p.x - local.centre.x + half; };
  const auto z_of = [&](const point p) { return p.z - local.centre.z + half; };
  output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << 2 * half << ' ' << 2 * half
         << "\" width=\"960\" height=\"960\">\n<rect width=\"" << 2 * half << "\" height=\"" << 2 * half
         << "\" fill=\"#1b2024\"/>\n<text x=\"2\" y=\"5\" fill=\"#d4e0e8\" font-size=\"3\">"
         << name(local.recipe) << (local.has_cave ? " · cave, nest depth " + std::to_string(int(cave_depth(local))) + " m"
                                                  : " · no cave") << "</text>\n";
  for (const room_link link : local.links) {
    const point a = local.rooms[link.a].centre, b = local.rooms[link.b].centre;
    output << "<line x1=\"" << x_of(a) << "\" y1=\"" << z_of(a) << "\" x2=\"" << x_of(b) << "\" y2=\"" << z_of(b)
           << "\" stroke=\"#5d6f7c\" stroke-width=\"" << (link.a == 0 || link.b == 0 ? 0.4 : 2.0 * core)
           << "\" stroke-linecap=\"round\"/>\n";
  }
  for (uint32_t i = 0; i < local.rooms.size(); ++i) {
    const room& chamber = local.rooms[i];
    const bool plug = std::find(local.plugs.begin(), local.plugs.end(), i) != local.plugs.end();
    const char* fill = i == 0 ? "none" : i == local.nest ? "#b8433a" : chamber.purpose.starts_with("pocket:") ? "#c9a93f" :
                       plug ? "#6fd0e8" : chamber.purpose == "hub" || chamber.purpose == "antechamber" ? "#7d8f9c" : "#5d6f7c";
    output << "<circle cx=\"" << x_of(chamber.centre) << "\" cy=\"" << z_of(chamber.centre) << "\" r=\""
           << chamber.radius << "\" fill=\"" << fill << "\" stroke=\"#8ab5c7\" stroke-width=\"0.2\"/>\n";
    if (chamber.purpose != "bend") {
      output << "<text x=\"" << x_of(chamber.centre) + 1 << "\" y=\"" << z_of(chamber.centre) - 1
             << "\" fill=\"#ffffff\" font-family=\"sans-serif\" font-size=\"1.8\">" << chamber.purpose << "</text>\n";
    }
  }
  for (const portal& port : local.portals) {
    output << "<circle cx=\"" << x_of(port.centre) << "\" cy=\"" << z_of(port.centre) << "\" r=\"0.8\" fill=\"#66d5df\"/>\n";
  }
  output << "</svg>\n";
  return bool(output);
}

const devils_engine::originator::shape_vocabulary& world_shapes(const double clearance_radius) {
  static std::mutex lock;
  static std::map<uint64_t, std::unique_ptr<const devils_engine::originator::shape_vocabulary>> vocabularies;
  std::lock_guard guard(lock);
  auto& slot = vocabularies[std::bit_cast<uint64_t>(clearance_radius)];
  if (slot == nullptr) {
    slot = std::make_unique<const devils_engine::originator::shape_vocabulary>(make_world_shapes(clearance_radius));
  }
  return *slot;
}

void write_environment(const adventure& world, devils_engine::originator::generator_run& run) {
  auto& galleries = run.output("galleries");
  const auto f = [&](const std::string_view name) { return environment_field(galleries, name); };
  const auto ends_a = f("ends_a"), ends_b = f("ends_b"), port_a = f("port_a"), port_b = f("port_b"), axis = f("axis"),
             right = f("right"), up = f("up"), distance = f("distance"), core_radius = f("core_radius"),
             reach = f("reach"), first_t = f("first_t"), second_t = f("second_t"), base_width = f("base_width"),
             first_width = f("first_width"), second_width = f("second_width"), base_roof = f("base_roof"),
             vertical_gain = f("vertical_gain"), biome_width = f("biome_width"), constriction = f("constriction"),
             biome = f("biome"), lobe0 = f("lobe0"), lobe0_size = f("lobe0_size"), lobe1 = f("lobe1"),
             lobe1_size = f("lobe1_size"), pocket = f("pocket"), pocket_mouth = f("pocket_mouth"),
             pocket_centre = f("pocket_centre"), pocket_radius = f("pocket_radius");
  if (world.route.edges.size() > galleries.count()) {
    devils_engine::utils::error{}("GN09: window has {} edges, gallery capacity {}", world.route.edges.size(),
                                  galleries.count());
  }
  for (size_t i = 0; i < world.route.edges.size(); ++i) {
    const node& a = world.route.nodes[world.route.edges[i].a];
    const node& b = world.route.nodes[world.route.edges[i].b];
    const gallery_row row = make_gallery_row(world, a, b, shape_edge(world, a, b));
    const edge_shape& s = row.shape;
    set3(ends_a, i, row.a);
    set3(ends_b, i, row.b);
    set3(port_a, i, s.a);
    set3(port_b, i, s.b);
    set3(axis, i, s.axis);
    set3(right, i, s.right);
    set3(up, i, s.up);
    distance.set(i, s.distance);
    core_radius.set(i, s.core_radius);
    reach.set(i, s.reach);
    first_t.set(i, s.first_t);
    second_t.set(i, s.second_t);
    base_width.set(i, s.base_width);
    first_width.set(i, s.first_width);
    second_width.set(i, s.second_width);
    base_roof.set(i, s.base_roof);
    vertical_gain.set(i, s.vertical_gain);
    biome_width.set(i, s.biome_width);
    constriction.set(i, s.constriction ? 1.0 : 0.0);
    biome.set(i, double(uint8_t(s.biome)));
    set3(lobe0, i, s.lobes[0].centre);
    set3(lobe0_size, i, {s.lobes[0].axial, s.lobes[0].lateral, s.lobes[0].vertical});
    set3(lobe1, i, s.lobes[1].centre);
    set3(lobe1_size, i, {s.lobes[1].axial, s.lobes[1].lateral, s.lobes[1].vertical});
    if (row.pocket) {
      pocket.set(i, 1.0);
      set3(pocket_mouth, i, row.pocket->mouth);
      set3(pocket_centre, i, row.pocket->centre);
      pocket_radius.set(i, row.pocket->radius);
    }
  }
  auto& turns = run.output("turns");
  const auto centre = environment_field(turns, "centre"), radius = environment_field(turns, "radius"),
             rock = environment_field(turns, "rock");
  if (world.route.nodes.size() > turns.count()) {
    devils_engine::utils::error{}("GN09: window has {} nodes, turn capacity {}", world.route.nodes.size(), turns.count());
  }
  for (size_t i = 0; i < world.route.nodes.size(); ++i) {
    const turn_row row = make_turn_row(world, world.route.nodes[i]);
    set3(centre, i, row.centre);
    radius.set(i, row.radius);
    rock.set(i, double(uint8_t(row.rock)));
  }
  auto& counts = run.output("environment_counts");
  environment_field(counts, "galleries").set(0, double(world.route.edges.size()));
  environment_field(counts, "turns").set(0, double(world.route.nodes.size()));
}

} // namespace gn09
