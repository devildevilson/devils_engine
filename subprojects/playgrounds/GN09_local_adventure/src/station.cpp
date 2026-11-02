#include "station.h"

#include <algorithm>
#include <span>
#include <functional>
#include <array>
#include <cmath>
#include <chrono>
#include <bit>
#include <cstdio>
#include <future>
#include <map>
#include <mutex>
#include <cstdlib>
#include <fstream>
#include <tuple>

#include "local_structure.h"
#include "world_chain.h"
#include "devils_engine/originator/primitive_field.h"
#include "devils_engine/originator/geometry.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"

namespace gn09 {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double degrees = 180.0 / pi;
// Пороги опоры. Уклон пола/свода и отклонение стены от вертикали — по замеру
// распределений на 128 seed (`--support-stats`); неровность — сколько остаётся
// на фундамент или вырез по нормали к найденной плоскости.
// Три прохода от строгого к ослабленному: неровное место лучше, чем база без станции.
// Последний проход сокращает и требование к объёму породы за стеной.
constexpr std::array<double, 3> max_floor_tilt{15.0, 25.0, 32.0};
constexpr std::array<double, 3> max_wall_tilt{25.0, 35.0, 45.0};
constexpr std::array<double, 3> max_residual{1.5, 2.5, 3.5};
constexpr std::array<double, 3> inner_depth{14.0, 14.0, 8.0};
constexpr double ray_reach = 70.0;
// Зазор лодки до станции у люка и высота середины двери над полом.
constexpr double berth_gap = 0.4;
constexpr double door_centre = 0.95;
double hull_radius() { return cave_hull_radius(); }
// Высоты проб слоя станции относительно пола: плита пола, этаж, плита крыши.
// Порода вокруг внутреннего помещения: клеток за его стеной.
constexpr int32_t deep_ring = 3; // 1.5 м: вырез на 0.75 м за стеной не вскрывает породу
// Неровность воротника нужна хостовой проверке его влияния.
constexpr double station_rock_relief = 0.5;

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const point a) { return std::sqrt(dot(a, a)); }
point cross(const point a, const point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double point_segment(const point p, const point a, const point b) {
  const point ab = subtract(b, a);
  const double len = dot(ab, ab);
  const double t = len > 0.0 ? std::clamp(dot(subtract(p, a), ab) / len, 0.0, 1.0) : 0.0;
  return length(subtract(p, add(a, scale(ab, t))));
}

point axis_u(const double yaw) { return {std::cos(yaw), 0.0, std::sin(yaw)}; }
point axis_v(const double yaw) { return {-std::sin(yaw), 0.0, std::cos(yaw)}; }

} // namespace

const char* name(const station_pose value) {
  switch (value) {
    case station_pose::wall: return "wall";
    case station_pose::floor: return "floor";
    case station_pose::ceiling: return "ceiling";
    case station_pose::ledge: return "ledge";
  }
  return "unknown";
}

point station_to_world(const station_site& site, const double u, const double y, const double v) {
  const point a = axis_u(site.yaw), b = axis_v(site.yaw);
  return {site.origin.x + a.x * u + b.x * v, site.origin.y + y, site.origin.z + a.z * u + b.z * v};
}

station_anchor anchor_of(const adventure& world, const uint32_t node_id) {
  const node& at = world.route.nodes.at(node_id);
  station_anchor anchor;
  anchor.key = at.key;
  anchor.centre = at.centre;
  anchor.kind = at.kind;
  anchor.recipe = recipe_of(world.seed, at);
  anchor.preferred = preferred_dock_axis(world.seed, at);
  return anchor;
}

std::vector<station_anchor> hall_station_anchors(const uint64_t seed, const hall& h) {
  // Аванпосты — данные генератора halls (generator/macro/01_halls.lua).
  namespace originator = devils_engine::originator;
  const auto instance = world_chain(1.0).get("halls", seed, {{uint64_t(int64_t(h.region_x)), uint64_t(int64_t(h.region_z)), 0}});
  const auto key = instance->field("hall_outposts", "key"), centre = instance->field("hall_outposts", "centre"),
             kind = instance->field("hall_outposts", "kind"), recipe = instance->field("hall_outposts", "recipe"),
             preferred = instance->field("hall_outposts", "preferred"), hall_key = instance->field("hall_outposts", "hall");
  std::vector<station_anchor> out;
  for (size_t i = 0; i < key.count(); ++i) {
    if (originator::read_key_word(hall_key, i) != h.key) continue;
    station_anchor anchor;
    anchor.key = originator::read_key_word(key, i);
    anchor.centre = {centre.get(i, 0), centre.get(i, 1), centre.get(i, 2)};
    anchor.kind = theme(uint8_t(kind.get(i)));
    anchor.recipe = site_recipe(uint8_t(recipe.get(i)));
    anchor.preferred = int8_t(preferred.get(i));
    anchor.hall_key = h.key;
    out.push_back(anchor);
  }
  return out;
}

std::vector<station_anchor> station_anchors(const adventure& world) {
  std::vector<station_anchor> out;
  for (const node& at : world.route.nodes) {
    if (is_base(at.kind)) out.push_back(anchor_of(world, at.id));
  }
  for (const hall& h : world.halls) {
    for (const station_anchor& anchor : hall_station_anchors(world.seed, h)) out.push_back(anchor);
  }
  return out;
}

// ---------------------------------------------------------------------------------------
// Маска плана


namespace {

// Подготовленное поле пещеры над прямоугольником плана [i0,i1)x[k0,k1) со всем
// вертикальным запасом станции (фундамент, подвес, пробы над и под этажом).
// with_sites = false — природная порода без пещер мест: станция их не ждёт, и её обещания (порода вокруг
// внутренних комнат) — относительно этого поля. С пещерами — для мягкого счёта вскрытий.
field_region station_region(const adventure& world, const station_site& site, const int32_t i0,
                            const int32_t k0, const int32_t i1, const int32_t k1, const bool with_sites = false) {
  point low{1e30, site.origin.y - 6.0, 1e30}, high{-1e30, site.origin.y + 10.0, -1e30};
  for (const int32_t i : {i0, i1}) for (const int32_t k : {k0, k1}) {
    const point p = station_to_world(site, i * station_cell(), 0.0, k * station_cell());
    low.x = std::min(low.x, p.x - 1.0); high.x = std::max(high.x, p.x + 1.0);
    low.z = std::min(low.z, p.z - 1.0); high.z = std::max(high.z, p.z + 1.0);
  }
  return prepare_field(world, low, high, false, with_sites);
}

// Вертикальный диапазон столба станции относительно пола: фундамент вниз у позы на
// дне, подвес вверх у позы под сводом.
std::pair<double, double> column_range(const station_pose pose) {
  const double bottom = -station_cell() - (roof_hatch(pose) ? station_cell() * station_plinth_layers : 0.0);
  const double top = station_cell() * (station_interior_layers + 1) +
                     (pose == station_pose::ceiling ? station_cell() * station_plinth_layers : 0.0);
  return {bottom, top};
}

// Запретная геометрия в локальных осях станции (u, y, v): маршруты, шары поворота,
// путь лодки к причалу. Клетка проверяется точным расстоянием до своей коробки.
struct keep_out_set {
  std::vector<std::pair<point, point>> routes; // x=u, y=y, z=v
  std::vector<point> turns;
  std::vector<std::pair<point, point>> berth;  // отрезки, заметающие путь лодки
  // Коридоры залов: петля крупного существа и оси ниш-укрытий, со своим радиусом.
  struct tube { point a, b; double radius; };
  std::vector<tube> tubes;
};

point to_local(const station_site& site, const point p) {
  const point d = subtract(p, site.origin);
  return {dot(d, axis_u(site.yaw)), d.y, dot(d, axis_v(site.yaw))};
}

keep_out_set keep_out_for(const adventure& world, const station_anchor& anchor,
                          const station_site& site) {
  keep_out_set out;
  const point centre = anchor.centre;
  for (const edge link : world.route.edges) {
    const point a = world.route.nodes[link.a].centre, b = world.route.nodes[link.b].centre;
    if (point_segment(centre, a, b) > 140.0) continue;
    out.routes.push_back({to_local(site, a), to_local(site, b)});
  }
  for (const node& n : world.route.nodes) {
    if (length(subtract(n.centre, centre)) < 140.0) out.turns.push_back(to_local(site, n.centre));
  }
  const point half = scale(site.berth_axis, starter_sub_axis_half());
  for (int i = 0; i <= 4; ++i) {
    const point c = add(centre, scale(subtract(site.berth, centre), double(i) / 4.0));
    out.berth.push_back({to_local(site, subtract(c, half)), to_local(site, add(c, half))});
  }
  out.berth.push_back({to_local(site, subtract(centre, half)), to_local(site, subtract(site.berth, half))});
  out.berth.push_back({to_local(site, add(centre, half)), to_local(site, add(site.berth, half))});
  // Станция в зале не встаёт на пути существа и не закрывает ниши.
  const auto tube = [&](const hall& h, const point a, const point b, const double radius) {
    const point wa = hall_to_world(h, a), wb = hall_to_world(h, b);
    if (point_segment(centre, wa, wb) > 140.0 + radius) return;
    out.tubes.push_back({to_local(site, wa), to_local(site, wb), radius});
  };
  for (const hall& h : world.halls) {
    if (std::abs(h.centre.x - centre.x) > h.half_length + 200.0 ||
        std::abs(h.centre.z - centre.z) > h.half_length + 200.0) continue;
    for (size_t i = 0; i < h.loop.size(); ++i) {
      tube(h, h.loop[i], h.loop[(i + 1) % h.loop.size()], h.body_radius + surface_guard());
    }
    for (const hall_niche& n : h.niches) {
      const double radius = 1.35 * n.radius + surface_guard() + 2.5;
      tube(h, n.mouth, n.elbow, radius);
      tube(h, n.elbow, n.end, radius);
    }
  }
  return out;
}

double box_distance(const point p, const point low, const point high) {
  const double x = std::max({low.x - p.x, 0.0, p.x - high.x});
  const double y = std::max({low.y - p.y, 0.0, p.y - high.y});
  const double z = std::max({low.z - p.z, 0.0, p.z - high.z});
  return std::sqrt(x * x + y * y + z * z);
}

// Отрезок до коробки. Расстояние до выпуклого множества вдоль отрезка выпукло по
// параметру, поэтому минимум находится троичным поиском. Грубая оценка через
// описанную сферу отсекает далёкие отрезки, не дойдя до поиска.
double segment_box_distance(const point a, const point b, const point low, const point high,
                            const double threshold) {
  const point centre = scale(add(low, high), 0.5);
  const double radius = length(subtract(high, low)) / 2.0;
  const double coarse = point_segment(centre, a, b) - radius;
  if (coarse >= threshold) return coarse;
  const point ab = subtract(b, a);
  const auto at = [&](const double t) { return box_distance(add(a, scale(ab, t)), low, high); };
  double left = 0.0, right = 1.0;
  for (int i = 0; i < 40; ++i) {
    const double m1 = left + (right - left) / 3.0, m2 = right - (right - left) / 3.0;
    (at(m1) <= at(m2) ? right : left) = (at(m1) <= at(m2) ? m2 : m1);
  }
  return std::min({at(0.0), at(1.0), at((left + right) / 2.0)});
}

// Коробка клетки в локальных осях: u in [u0,u1], y in [y0,y1], v in [v0,v1].
bool box_forbidden(const station_site& site, const keep_out_set& set, const point low,
                   const point high) {
  if (site.origin.y + low.y < world_y_min() + 1.0 || site.origin.y + high.y > world_y_max() - 1.0) return true;
  for (const auto& [a, b] : set.routes) {
    if (segment_box_distance(a, b, low, high, starter_sub_path_radius()) < starter_sub_path_radius()) return true;
  }
  for (const point& p : set.turns) {
    if (box_distance(p, low, high) < starter_sub_turn_radius()) return true;
  }
  for (const auto& t : set.tubes) {
    if (segment_box_distance(t.a, t.b, low, high, t.radius) < t.radius) return true;
  }
  // Корпус у причала — точная геометрия, без охранного слоя сетки скалы.
  for (const auto& [a, b] : set.berth) {
    if (segment_box_distance(a, b, low, high, hull_radius() + 0.15) < hull_radius() + 0.15) return true;
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Проверка плана — по прямоугольникам и по полю, без маски и без порядка роста.

namespace {

// Четверти клетки (i,k) внутри помещения со срезанными углами.
uint8_t room_quarters(const int32_t u0, const int32_t v0, const int32_t u1, const int32_t v1,
                      const uint8_t chamfer[4], const int32_t i, const int32_t k) {
  if (i < u0 || i >= u1 || k < v0 || k >= v1) return 0;
  uint8_t mask = 0xF;
  // Для угла: расстояния (a,b) от угла в клетках и половина, что остаётся.
  const int32_t a[4] = {i - u0, u1 - 1 - i, u1 - 1 - i, i - u0};
  const int32_t b[4] = {k - v0, k - v0, v1 - 1 - k, v1 - 1 - k};
  static constexpr uint8_t kept[4] = {0b0110, 0b0011, 0b1001, 0b1100};
  for (uint32_t c = 0; c < 4; ++c) {
    const int32_t cut = chamfer[c];
    if (cut == 0) continue;
    if (a[c] + b[c] + 2 <= cut) return 0;
    if (a[c] + b[c] + 1 == cut) mask &= kept[c];
  }
  return mask;
}

uint8_t room_interior(const station_room& r, const int32_t i, const int32_t k) {
  return room_quarters(r.u0, r.v0, r.u1, r.v1, r.chamfer, i, k);
}

// Наружный контур стены: прямоугольник шире на клетку, срез угла глубже на одну.
// Расширение прямоугольника уже сдвигает диагональ к углу на две ступени (a+b), так
// что срез +1 оставляет между диагоналями полосу в ступень — стену 0.35 м. Срез +2
// совпадал с внутренней диагональю, и у скошенного угла стены не было вовсе.
uint8_t room_outline(const station_room& r, const int32_t i, const int32_t k) {
  uint8_t chamfer[4];
  for (uint32_t c = 0; c < 4; ++c) chamfer[c] = r.chamfer[c] ? uint8_t(r.chamfer[c] + 1) : 0;
  return room_quarters(r.u0 - 1, r.v0 - 1, r.u1 + 1, r.v1 + 1, chamfer, i, k);
}

} // namespace

uint8_t station_room_quarters(const station_room& room, const int32_t i, const int32_t k) {
  return room_interior(room, i, k);
}

std::string verify_station_plan(const adventure& world, const uint32_t node_id,
                                const station_plan& plan) {
  return verify_station_plan(world, anchor_of(world, node_id), plan);
}

std::string verify_station_plan(const adventure& world, const station_anchor& anchor,
                                const station_plan& plan, const bool with_sites) {
  const station_site& site = plan.site;
  if (plan.rooms.empty() || plan.rooms[0].purpose != "airlock") return "station has no airlock";
  for (uint32_t i = 0; i < plan.rooms.size(); ++i) {
    const station_room& r = plan.rooms[i];
    if (r.u0 >= r.u1 || r.v0 >= r.v1) return "station room has an invalid rectangle";
    for (uint32_t c = 0; c < 4; ++c) {
      if (2 * r.chamfer[c] > std::min(r.u1 - r.u0, r.v1 - r.v0)) return "station chamfer is too deep";
    }
    for (uint32_t j = 0; j < i; ++j) {
      const station_room& o = plan.rooms[j];
      if (r.u0 - 1 < o.u1 && o.u0 < r.u1 + 1 && r.v0 - 1 < o.v1 && o.v0 < r.v1 + 1) {
        return "station rooms share space without a wall";
      }
    }
  }
  std::vector<uint8_t> reached(plan.rooms.size(), 0);
  reached[0] = 1;
  for (const station_door& d : plan.doors) {
    if (d.a >= plan.rooms.size() || d.b >= plan.rooms.size() || d.a == d.b) {
      return "station door names an invalid room";
    }
    // Дверь — линия клеток стены, по обе стороны которой целые клетки двух помещений.
    const bool across_u = d.u1 - d.u0 == 1 && d.v1 > d.v0;
    const bool across_v = d.v1 - d.v0 == 1 && d.u1 > d.u0;
    if (!across_u && !across_v) return "station door is not a wall line";
    const station_room& ra = plan.rooms[d.a];
    const station_room& rb = plan.rooms[d.b];
    const int32_t di = across_u ? 1 : 0, dk = across_v ? 1 : 0;
    for (int32_t i = d.u0; i < d.u1; ++i) for (int32_t k = d.v0; k < d.v1; ++k) {
      const bool forward = room_interior(ra, i - di, k - dk) == 0xF && room_interior(rb, i + di, k + dk) == 0xF;
      const bool backward = room_interior(rb, i - di, k - dk) == 0xF && room_interior(ra, i + di, k + dk) == 0xF;
      if (!forward && !backward) return "station door does not join two rooms";
    }
  }
  for (size_t pass = 0; pass < plan.rooms.size(); ++pass) {
    for (const station_door& d : plan.doors) {
      if (reached[d.a] || reached[d.b]) reached[d.a] = reached[d.b] = 1;
    }
  }
  if (std::find(reached.begin(), reached.end(), 0) != reached.end()) {
    return "station room is unreachable from the airlock";
  }
  const station_room& airlock = plan.rooms[0];
  if (site.pose == station_pose::wall) {
    if (plan.hatch_u1 != airlock.u0 || plan.hatch_u0 != airlock.u0 - 1 ||
        plan.hatch_v0 < airlock.v0 || plan.hatch_v1 > airlock.v1) return "station hatch is off the airlock";
  } else if (plan.hatch_u0 < airlock.u0 || plan.hatch_u1 > airlock.u1 ||
             plan.hatch_v0 < airlock.v0 || plan.hatch_v1 > airlock.v1) {
    return "station hatch is off the airlock";
  }
  // Мокрый док базы: корпус у причала и на подходе от узла стоит в природной воде (без пещер мест) —
  // станция ничего не вырезает под лодку, путь к причалу лишь обещан вырезом на случай неровности. У
  // уступа зала подход по-прежнему вырезается: зал не обещает воды над уступом.
  if (site.pose != station_pose::ledge) {
    const point half = scale(site.berth_axis, starter_sub_axis_half());
    point low{1e30, 1e30, 1e30}, high{-1e30, -1e30, -1e30};
    for (const point c : {site.node_centre, site.berth}) for (const double t : {-1.0, 1.0}) {
      const point p = add(c, scale(half, t));
      low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
      high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const double pad = hull_radius() + 1.0;
    const field_region dock = prepare_field(world, {low.x - pad, low.y - pad, low.z - pad},
                                            {high.x + pad, high.y + pad, high.z + pad}, false, false);
    const point side = std::abs(site.berth_axis.y) < 0.9 ? point{0.0, 1.0, 0.0} : point{1.0, 0.0, 0.0};
    const point e1 = scale(cross(site.berth_axis, side), 1.0 / length(cross(site.berth_axis, side)));
    const point e2 = cross(site.berth_axis, e1);
    for (int step = 0; step <= 4; ++step) {
      const point c = add(site.node_centre, scale(subtract(site.berth, site.node_centre), step / 4.0));
      for (int a = -4; a <= 4; ++a) for (int r = 0; r < 8; ++r) {
        const double angle = r * pi / 4.0;
        const point p = add(add(c, scale(half, a / 4.0)),
                            add(scale(e1, hull_radius() * std::cos(angle)), scale(e2, hull_radius() * std::sin(angle))));
        if (cave_density(dock, p) >= 0.0) return "station dock or its approach is in rock";
      }
    }
  }
  // Воротник своей породы (если есть) не задевает маршрут, повороты, путь лодки и коридоры залов (с
  // запасом на неровность и шов); заглублённые помещения — в породе с запасом (природной или воротника);
  // ни один столб не задевает запретного. Путь лодки к причалу вырезается самой станцией.
  const keep_out_set keep = keep_out_for(world, anchor, site);
  if (plan.rock_low.x < plan.rock_high.x && plan.rock_low.y < plan.rock_high.y && plan.rock_low.z < plan.rock_high.z) {
    const double reach = station_rock_relief + 1.0;
    keep_out_set collar_keep = keep;
    collar_keep.berth.clear(); // путь к причалу вырезает сама станция
    if (box_forbidden(site, collar_keep, {plan.rock_low.x - reach, plan.rock_low.y - reach, plan.rock_low.z - reach},
                      {plan.rock_high.x + reach, plan.rock_high.y + reach, plan.rock_high.z + reach})) {
      return "station rock meets the route, a turn or the berth";
    }
  }
  const auto [bottom, top] = column_range(site.pose);
  int32_t i0 = INT32_MAX, k0 = INT32_MAX, i1 = INT32_MIN, k1 = INT32_MIN;
  for (const station_room& r : plan.rooms) {
    i0 = std::min(i0, r.u0 - 4); k0 = std::min(k0, r.v0 - 4);
    i1 = std::max(i1, r.u1 + 4); k1 = std::max(k1, r.v1 + 4);
  }
  const field_region region = station_region(world, site, i0, k0, i1, k1, with_sites);
  const auto field = [&](const point p) {
    return std::max(cave_density(region, p), station_rock_core(site, plan.rock_low, plan.rock_high, p));
  };
  for (uint32_t index = 0; index < plan.rooms.size(); ++index) {
    const station_room& r = plan.rooms[index];
    for (int32_t i = r.u0 - 1; i <= r.u1; ++i) for (int32_t k = r.v0 - 1; k <= r.v1; ++k) {
      if (!room_outline(r, i, k)) continue;
      const double u = i * station_cell(), v = k * station_cell();
      if (box_forbidden(site, keep, {u, bottom, v}, {u + station_cell(), top, v + station_cell()})) {
        return "station column meets the route, a turn or the berth";
      }
    }
    if (r.inner) {
      // Стены внутренних помещений: за стеной ещё полметра породы во все стороны.
      for (int32_t i = r.u0 - deep_ring; i < r.u1 + deep_ring; ++i) {
        for (int32_t k = r.v0 - deep_ring; k < r.v1 + deep_ring; ++k) {
        const bool inside_room = i >= r.u0 && i < r.u1 && k >= r.v0 && k < r.v1;
        for (const double y : {-1.0, 1.5, 4.0}) {
          if (!inside_room && y != 1.5) continue;
          // Путь лодки к причалу тоже вырезается: он не должен вскрыть внутреннюю часть.
          const point p = station_to_world(site, (i + 0.5) * station_cell(), y, (k + 0.5) * station_cell());
          if (field(p) <= 0.0 || berth_sweep_field(site, p) <= 0.0) {
            if (std::getenv("GN09_STATION_DEBUG")) {
              const field_region at = prepare_field(world, {p.x - 1, p.y - 1, p.z - 1}, {p.x + 1, p.y + 1, p.z + 1}, false);
              std::fprintf(stderr, "inner room %s not enclosed at cell %d,%d y=%.1f: field %.3f (%s), sweep %.3f\n",
                           r.purpose.c_str(), i, k, y, field(p), name(rock_at(at, p)), berth_sweep_field(site, p));
            }
            return "inner station room is not enclosed in rock";
          }
        }
        }
      }
    }
  }
  return {};
}

// ---------------------------------------------------------------------------------------
// Воксели и сетка

uint8_t station_voxels::at(const int32_t i, const int32_t y, const int32_t k) const {
  const int32_t a = i - i0, b = k - k0, c = y - y0;
  if (a < 0 || b < 0 || c < 0 || a >= width || b >= depth || c >= height) return 0;
  return solid[(size_t(c) * depth + b) * width + a];
}

// ---------------------------------------------------------------------------------------
// Станция в мире

namespace {

// Коробка станции: воксели со всем запасом выреза и люка и путь лодки к причалу. За коробкой вырез
// больше фоновой скалы (3 + 0.46): отбросить его — точно. false — станция не помещается в куб
// station_reach вокруг узла.
namespace originator = devils_engine::originator;

point read3(const originator::const_field_accessor& f, const size_t i) { return {f.get(i, 0), f.get(i, 1), f.get(i, 2)}; }


// Станция из буферов экземпляра генератора station: представление для нативных потребителей.
std::shared_ptr<const built_station> station_from(const originator::generator_result& instance) {
  const auto meta = [&](const std::string_view name) { return instance.field("station_meta", name); };
  if (meta("built").get(0) == 0.0) return nullptr;
  auto station = std::make_shared<built_station>();
  station_anchor& anchor = station->anchor;
  anchor.key = originator::read_key_word(meta("anchor_key"), 0);
  anchor.centre = read3(meta("anchor_centre"), 0);
  anchor.kind = theme(uint8_t(meta("kind").get(0)));
  anchor.recipe = site_recipe(uint8_t(meta("recipe").get(0)));
  anchor.preferred = int8_t(meta("preferred").get(0));
  anchor.hall_key = originator::read_key_word(meta("hall_key"), 0);
  station->node_key_low = uint32_t(anchor.key);
  station_plan& plan = station->plan;
  plan.kind = anchor.kind;
  station_site& site = plan.site;
  site.pose = station_pose(uint8_t(meta("pose").get(0)));
  site.outer_cells = int32_t(meta("outer_cells").get(0));
  site.origin = read3(meta("origin"), 0);
  site.yaw = meta("yaw").get(0);
  site.node_centre = read3(meta("node_centre"), 0);
  site.berth = read3(meta("berth"), 0);
  site.berth_axis = read3(meta("berth_axis"), 0);
  site.tilt_deg = meta("tilt").get(0);
  site.residual = meta("residual").get(0);
  site.reach = meta("reach").get(0);
  site.rock_distance = meta("rock_distance").get(0);
  const auto hatch = meta("hatch");
  plan.hatch_u0 = int32_t(hatch.get(0, 0)); plan.hatch_v0 = int32_t(hatch.get(0, 1));
  plan.hatch_u1 = int32_t(hatch.get(0, 2)); plan.hatch_v1 = int32_t(hatch.get(0, 3));
  const auto& purposes = station_purposes();
  const auto rect = instance.field("station_rooms", "rect"), chamfer = instance.field("station_rooms", "chamfer"),
             purpose = instance.field("station_rooms", "purpose"), inner = instance.field("station_rooms", "inner"),
             parent = instance.field("station_rooms", "parent");
  for (size_t i = 0; i < rect.count(); ++i) {
    station_room r;
    r.u0 = int32_t(rect.get(i, 0)); r.v0 = int32_t(rect.get(i, 1)); r.u1 = int32_t(rect.get(i, 2)); r.v1 = int32_t(rect.get(i, 3));
    for (uint32_t c = 0; c < 4; ++c) r.chamfer[c] = uint8_t(chamfer.get(i, c));
    const auto code = size_t(purpose.get(i));
    if (code >= purposes.size()) devils_engine::utils::error{}("GN09: station room purpose {} is outside the vocabulary", code);
    r.purpose = purposes[code];
    r.inner = inner.get(i) != 0.0;
    r.parent = uint32_t(parent.get(i));
    plan.rooms.push_back(r);
  }
  const auto da = instance.field("station_doors", "a"), db = instance.field("station_doors", "b"),
             dr = instance.field("station_doors", "rect");
  for (size_t i = 0; i < da.count(); ++i) {
    plan.doors.push_back({uint32_t(da.get(i)), uint32_t(db.get(i)), int32_t(dr.get(i, 0)), int32_t(dr.get(i, 1)),
                          int32_t(dr.get(i, 2)), int32_t(dr.get(i, 3))});
  }
  station_voxels& v = station->voxels;
  const auto vo = meta("voxel_origin"), vs = meta("voxel_size");
  v.i0 = int32_t(vo.get(0, 0)); v.k0 = int32_t(vo.get(0, 1)); v.y0 = int32_t(vo.get(0, 2));
  v.width = int32_t(vs.get(0, 0)); v.depth = int32_t(vs.get(0, 1)); v.height = int32_t(vs.get(0, 2));
  const auto solid = instance.field("station_voxels", "solid"), shade = instance.field("station_voxels", "shade");
  v.solid.resize(solid.count());
  v.shade.resize(solid.count());
  for (size_t i = 0; i < solid.count(); ++i) { v.solid[i] = uint8_t(solid.get(i)); v.shade[i] = uint8_t(shade.get(i)); }
  const auto ma = instance.field("station_mesh", "a"), mb = instance.field("station_mesh", "b"),
             mc = instance.field("station_mesh", "c"), mn = instance.field("station_mesh", "normal"),
             ms = instance.field("station_mesh", "shade");
  station->mesh.reserve(ma.count());
  for (size_t i = 0; i < ma.count(); ++i) {
    station->mesh.push_back({read3(ma, i), read3(mb, i), read3(mc, i), read3(mn, i), uint8_t(ms.get(i))});
  }
  station->low = read3(meta("low"), 0);
  station->high = read3(meta("high"), 0);
  plan.rock_low = read3(instance.field("station_rock", "local_low"), 0);
  plan.rock_high = read3(instance.field("station_rock", "local_high"), 0);
  // Коробка станции охватывает и свою породу с неровностью и швом: по ней чанк отбирает станции.
  const double rock_reach = station_rock_relief + 2.0;
  if (plan.rock_low.x < plan.rock_high.x)
  for (const double u : {plan.rock_low.x, plan.rock_high.x}) for (const double y : {plan.rock_low.y, plan.rock_high.y}) {
    for (const double v : {plan.rock_low.z, plan.rock_high.z}) {
      const point corner = station_to_world(site, u, y, v);
      station->low = {std::min(station->low.x, corner.x - rock_reach), std::min(station->low.y, corner.y - rock_reach),
                      std::min(station->low.z, corner.z - rock_reach)};
      station->high = {std::max(station->high.x, corner.x + rock_reach), std::max(station->high.y, corner.y + rock_reach),
                       std::max(station->high.z, corner.z + rock_reach)};
    }
  }
  return station;
}

} // namespace

double station_rock_core(const station_site& site, const point rock_low, const point rock_high, const point position) {
  const point q = to_local(site, position);
  return q.x >= rock_low.x && q.x <= rock_high.x && q.y >= rock_low.y && q.y <= rock_high.y && q.z >= rock_low.z &&
             q.z <= rock_high.z ? 1.0 : -1.0;
}

std::shared_ptr<const built_station> station_view(const devils_engine::originator::generator_result& instance) {
  return instance.view<built_station>(station_from);
}

originator::instance_key station_key(const station_anchor& anchor) {
  const auto home = sector_of(anchor.centre);
  return {{uint64_t(int64_t(home[0])), uint64_t(int64_t(home[1])), anchor.key}};
}

std::shared_ptr<const devils_engine::originator::generator_result> station_instance(const adventure& world,
                                                                                    const station_anchor& at) {
  return world_chain(world.clearance_radius).get("station", world.seed, station_key(at));
}

std::shared_ptr<const built_station> station_for(const adventure& world, const uint32_t node_id) {
  if (!is_base(world.route.nodes.at(node_id).kind)) return nullptr;
  return station_for(world, anchor_of(world, node_id));
}

std::shared_ptr<const built_station> station_for(const adventure& world, const station_anchor& at) {
  // Станция — экземпляр генератора station (generator/station/01_station.lua) в канонической
  // окрестности своего якоря; разобранный C++-вид живёт с экземпляром, кэш — кэш цепочки.
  return station_view(*station_instance(world, at));
}

bool station_reaches(const built_station& station, const point low, const point high) {
  return station.low.x <= high.x && station.high.x >= low.x && station.low.y <= high.y &&
         station.high.y >= low.y && station.low.z <= high.z && station.high.z >= low.z;
}

namespace {

// Знаковое расстояние до многоугольника помещения в плане (метры): прямоугольник
// и срезы углов как пересечение полуплоскостей. Внутри отрицательно.
double room_plan_field(const station_room& r, const double u, const double v) {
  const double u0 = r.u0 * station_cell(), u1 = r.u1 * station_cell();
  const double v0 = r.v0 * station_cell(), v1 = r.v1 * station_cell();
  double d = std::max({u0 - u, u - u1, v0 - v, v - v1});
  const double cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
  const double su[4] = {1, -1, -1, 1}, sv[4] = {1, 1, -1, -1};
  for (uint32_t c = 0; c < 4; ++c) {
    if (!r.chamfer[c]) continue;
    const double cut = r.chamfer[c] * station_cell();
    d = std::max(d, (cut - (su[c] * (u - cu[c]) + sv[c] * (v - cv[c]))) / std::sqrt(2.0));
  }
  return d;
}

double cell_box_field(const int32_t i0, const int32_t k0, const int32_t i1, const int32_t k1,
                      const double y0, const double y1, const double u, const double y, const double v) {
  return std::max({i0 * station_cell() - u, u - i1 * station_cell(), k0 * station_cell() - v,
                   v - k1 * station_cell(), y0 - y, y - y1});
}

} // namespace

double berth_sweep_field(const station_site& site, const point p) {
  // Параллелограмм: ось лодки (вдоль berth_axis) переносится от узла к причалу.
  const point half = scale(site.berth_axis, starter_sub_axis_half());
  const point origin = subtract(site.node_centre, half);
  const point e1 = subtract(site.berth, site.node_centre), e2 = scale(half, 2.0);
  const point n = cross(e1, e2);
  const double nn = dot(n, n);
  double distance = 1e30;
  if (nn > 1e-9) {
    // Проекция на плоскость и барицентрические координаты в базисе (e1, e2).
    const point d = subtract(p, origin);
    const double a11 = dot(e1, e1), a12 = dot(e1, e2), a22 = dot(e2, e2);
    const double b1 = dot(d, e1), b2 = dot(d, e2);
    const double det = a11 * a22 - a12 * a12;
    const double s = (b1 * a22 - b2 * a12) / det, t = (b2 * a11 - b1 * a12) / det;
    if (s >= 0.0 && s <= 1.0 && t >= 0.0 && t <= 1.0) distance = std::abs(dot(d, n)) / std::sqrt(nn);
  }
  const point corner[4] = {origin, add(origin, e1), add(add(origin, e1), e2), add(origin, e2)};
  for (int i = 0; i < 4; ++i) distance = std::min(distance, point_segment(p, corner[i], corner[(i + 1) % 4]));
  return distance - starter_sub_path_radius();
}

double station_carve_field(const built_station& station, const point position) {
  const station_plan& plan = station.plan;
  const point d = subtract(position, plan.site.origin);
  const double u = dot(d, axis_u(plan.site.yaw)), y = d.y, v = dot(d, axis_v(plan.site.yaw));
  const double ceiling = station_cell() * station_interior_layers;
  double field = 3.0;
  for (const station_room& r : plan.rooms) {
    field = std::min(field, std::max(room_plan_field(r, u, v), std::max(-y, y - ceiling)));
  }
  for (const station_door& door : plan.doors) {
    field = std::min(field, cell_box_field(door.u0, door.v0, door.u1, door.v1, 0.0, ceiling, u, y, v));
  }
  // Люк открывается к причалу: боковой — наружу на метр, вертикальный — сквозь плиту.
  if (plan.site.pose == station_pose::wall) {
    field = std::min(field, cell_box_field(plan.hatch_u0 - 2, plan.hatch_v0, plan.hatch_u1, plan.hatch_v1,
                                           0.0, 2.0, u, y, v));
  } else if (roof_hatch(plan.site.pose)) {
    field = std::min(field, cell_box_field(plan.hatch_u0, plan.hatch_v0, plan.hatch_u1, plan.hatch_v1,
                                           0.0, ceiling + 1.5, u, y, v));
  } else {
    field = std::min(field, cell_box_field(plan.hatch_u0, plan.hatch_v0, plan.hatch_u1, plan.hatch_v1,
                                           -1.5, ceiling, u, y, v));
  }
  return std::min(field - station_carve_margin, berth_sweep_field(plan.site, position));
}

bool station_interior_contains(const built_station& station, const point position) {
  const station_plan& plan = station.plan;
  const point d = subtract(position, plan.site.origin);
  const double u = dot(d, axis_u(plan.site.yaw)), y = d.y, v = dot(d, axis_v(plan.site.yaw));
  if (y <= 0.0 || y >= station_cell() * station_interior_layers) return false;
  for (const station_room& r : plan.rooms) {
    if (room_plan_field(r, u, v) < 0.0) return true;
  }
  return false;
}

void clip_station_mesh(const built_station& station, const point low, const point high,
                       const std::function<void(const point*, size_t, point, uint8_t)>& polygon) {
  if (!station_reaches(station, low, high)) return;
  clip_triangles(station.mesh, low, high, polygon);
}

void clip_triangles(const std::vector<station_triangle>& mesh, const point low, const point high,
                    const std::function<void(const point*, size_t, point, uint8_t)>& polygon,
                    const std::function<bool(const station_triangle&)>& skip) {
  for (const station_triangle& t : mesh) {
    if (!devils_engine::originator::triangle_box_bounds_overlap(t.a, t.b, t.c, low, high)) continue;
    if (skip && skip(t)) continue;
    const auto clipped = devils_engine::originator::clip_triangle_to_box(t.a, t.b, t.c, low, high);
    if (clipped.count >= 3) polygon(clipped.vertices.data(), clipped.count, t.normal, t.shade);
  }
}

// ---------------------------------------------------------------------------------------

bool write_station_svg(const adventure& window, const uint32_t window_id, const std::string& path) {
  if (window_id >= window.route.nodes.size() || !is_base(window.route.nodes[window_id].kind)) return false;
  // Станция — экземпляр генератора station; маска — те её клетки, что спросил рост плана (ленивая),
  // остальные серые. Если места нет, рисовать нечего.
  const station_anchor anchor = anchor_of(window, window_id);
  const auto instance = station_instance(window, anchor);
  const auto built = station_for(window, anchor);
  if (!built) return false;
  const std::optional<station_plan> plan = built->plan;
  const std::optional<station_site> chosen = built->plan.site;
  const bool failed = false;
  const auto mask_rect = instance->field("station_meta", "mask");
  struct { int32_t i0, k0, width, depth; } mask{int32_t(mask_rect.get(0, 0)), int32_t(mask_rect.get(0, 1)),
                                                 int32_t(mask_rect.get(0, 2)), int32_t(mask_rect.get(0, 3))};
  const auto mask_cells = instance->field("mask_cells", "value");
  std::ofstream out(path);
  if (!out) return false;
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"" << mask.i0 * 0.5 << ' ' << mask.k0 * 0.5 << ' '
      << mask.width * 0.5 << ' ' << mask.depth * 0.5 << "\" width=\"" << mask.width * 9 << "\" height=\""
      << mask.depth * 9 << "\">\n";
  static constexpr const char* colours[] = {"#5a2230", "#0e1a2a", "#1d3b58", "#4d4a3f", "#6b6150", "#8a7c63"};
  for (int32_t b = 0; b < mask.depth; ++b) for (int32_t a = 0; a < mask.width; ++a) {
    const auto c = uint32_t(mask_cells.get(size_t(b) * size_t(mask.width) + size_t(a)));
    out << "<rect x=\"" << (mask.i0 + a) * 0.5 << "\" y=\"" << (mask.k0 + b) * 0.5 << "\" width=\"0.5\" height=\"0.5\" fill=\""
        << (c < 6 ? colours[c] : "#30343a") << "\"/>\n";
  }
  if (plan) {
    const station_voxels& voxels = built->voxels;
    // Стены — по четвертям на уровне середины этажа.
    for (int32_t k = voxels.k0; k < voxels.k0 + voxels.depth; ++k) {
      for (int32_t i = voxels.i0; i < voxels.i0 + voxels.width; ++i) {
        const uint8_t q = voxels.at(i, 2, k), floor = voxels.at(i, -1, k);
        const double u0 = i * 0.5, v0 = k * 0.5, uc = u0 + 0.25, vc = v0 + 0.25;
        const double cu[4] = {u0, u0 + 0.5, u0 + 0.5, u0}, cv[4] = {v0 + 0.5, v0 + 0.5, v0, v0};
        for (uint32_t s = 0; s < 4; ++s) {
          const uint32_t prev = (s + 3) % 4;
          const char* fill = (q >> s) & 1u ? "#cfd6da" : (floor >> s) & 1u ? "#3c6f86" : nullptr;
          if (!fill) continue;
          out << "<polygon points=\"" << uc << ',' << vc << ' ' << cu[prev] << ',' << cv[prev] << ' '
              << cu[s] << ',' << cv[s] << "\" fill=\"" << fill << "\"/>\n";
        }
      }
    }
    for (const station_door& d : plan->doors) {
      out << "<rect x=\"" << d.u0 * 0.5 << "\" y=\"" << d.v0 * 0.5 << "\" width=\"" << (d.u1 - d.u0) * 0.5
          << "\" height=\"" << (d.v1 - d.v0) * 0.5 << "\" fill=\"#e5c36b\"/>\n";
    }
    out << "<rect x=\"" << plan->hatch_u0 * 0.5 << "\" y=\"" << plan->hatch_v0 * 0.5 << "\" width=\""
        << (plan->hatch_u1 - plan->hatch_u0) * 0.5 << "\" height=\"" << (plan->hatch_v1 - plan->hatch_v0) * 0.5
        << "\" fill=\"none\" stroke=\"#ffb000\" stroke-width=\"0.2\"/>\n";
    for (const station_room& r : plan->rooms) {
      out << "<text x=\"" << (r.u0 + r.u1) * 0.25 << "\" y=\"" << (r.v0 + r.v1) * 0.25
          << "\" text-anchor=\"middle\" font-size=\"0.8\" fill=\"" << (r.inner ? "#ffd9a0" : "#d8f0ff")
          << "\">" << r.purpose << "</text>\n";
    }
  }
  // Лодка у причала в плане станции.
  const point delta = subtract(chosen->berth, chosen->origin);
  const double bu = dot(delta, axis_u(chosen->yaw)), bv = dot(delta, axis_v(chosen->yaw));
  const double au = dot(chosen->berth_axis, axis_u(chosen->yaw)) * starter_sub_axis_half();
  const double av = dot(chosen->berth_axis, axis_v(chosen->yaw)) * starter_sub_axis_half();
  out << "<line x1=\"" << bu - au << "\" y1=\"" << bv - av << "\" x2=\"" << bu + au << "\" y2=\""
      << bv + av << "\" stroke=\"#ff8c5a\" stroke-opacity=\"0.6\" stroke-width=\"6\" stroke-linecap=\"round\"/>\n";
  out << "<text x=\"" << mask.i0 * 0.5 + 1 << "\" y=\"" << mask.k0 * 0.5 + 2 << "\" font-size=\"1.2\" fill=\"white\">"
      << name(chosen->pose) << " tilt " << chosen->tilt_deg << " residual " << chosen->residual
      << (failed ? " — programme did not fit" : "") << "</text>\n";
  out << "</svg>\n";
  return bool(out);
}

} // namespace gn09
