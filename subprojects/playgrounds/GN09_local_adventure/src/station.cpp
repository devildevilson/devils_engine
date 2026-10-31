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
constexpr double hull_radius = starter_sub_beam / 2.0;
// Высоты проб слоя станции относительно пола: плита пола, этаж, плита крыши.
constexpr std::array<double, 5> station_band{-0.25, 0.5, 1.5, 2.5, 3.25};
// Порода вокруг внутреннего помещения: клеток за его стеной.
constexpr int32_t deep_ring = 3; // 1.5 м: вырез на 0.75 м за стеной не вскрывает породу

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

// Капсула радиуса radius вокруг [a,b] целиком в воде: ось через метр, вокруг оси
// восемь направлений на полном и половинном радиусе, на концах — полусферы.
template <class Field>
bool capsule_in_water(Field& field, const point a, const point b, const double radius) {
  point axis = subtract(b, a);
  const double len = length(axis);
  axis = len > 1e-9 ? scale(axis, 1.0 / len) : point{1, 0, 0};
  const point helper = std::abs(axis.y) < 0.9 ? point{0, 1, 0} : point{1, 0, 0};
  point e1 = cross(axis, helper);
  e1 = scale(e1, 1.0 / length(e1));
  const point e2 = cross(axis, e1);
  const int steps = std::max(1, int(std::ceil(len)));
  for (int i = 0; i <= steps; ++i) {
    const point c = add(a, scale(axis, len * double(i) / steps));
    if (field(c) >= 0.0) return false;
    for (int j = 0; j < 8; ++j) {
      const double angle = pi * j / 4.0;
      const point d = add(scale(e1, std::cos(angle)), scale(e2, std::sin(angle)));
      if (field(add(c, scale(d, radius))) >= 0.0) return false;
      if (field(add(c, scale(d, radius * 0.5))) >= 0.0) return false;
    }
  }
  return field(add(a, scale(axis, -radius))) < 0.0 && field(add(b, scale(axis, radius))) < 0.0;
}

// Путь лодки от поворотного узла до причала: параллельный перенос капсулы.
template <class Field>
bool berth_sweep_in_water(Field& field, const point from, const point berth,
                          const point berth_axis) {
  for (int i = 0; i <= 5; ++i) {
    const point c = add(from, scale(subtract(berth, from), double(i) / 5.0));
    if (!capsule_in_water(field, add(c, scale(berth_axis, -starter_sub_axis_half)),
                          add(c, scale(berth_axis, starter_sub_axis_half)),
                          starter_sub_path_radius)) return false;
  }
  return true;
}

} // namespace

const char* name(const station_pose value) {
  switch (value) {
    case station_pose::wall: return "wall";
    case station_pose::floor: return "floor";
    case station_pose::ceiling: return "ceiling";
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
  // Аванпосты — данные генератора halls (generator/macro/scripts/halls.lua).
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
field_region station_region(const adventure& world, const station_site& site, const int32_t i0,
                            const int32_t k0, const int32_t i1, const int32_t k1) {
  point low{1e30, site.origin.y - 6.0, 1e30}, high{-1e30, site.origin.y + 10.0, -1e30};
  for (const int32_t i : {i0, i1}) for (const int32_t k : {k0, k1}) {
    const point p = station_to_world(site, i * station_cell, 0.0, k * station_cell);
    low.x = std::min(low.x, p.x - 1.0); high.x = std::max(high.x, p.x + 1.0);
    low.z = std::min(low.z, p.z - 1.0); high.z = std::max(high.z, p.z + 1.0);
  }
  return prepare_field(world, low, high, false);
}

// Вертикальный диапазон столба станции относительно пола: фундамент вниз у позы на
// дне, подвес вверх у позы под сводом.
std::pair<double, double> column_range(const station_pose pose) {
  const double bottom = -station_cell - (pose == station_pose::floor ? station_cell * station_plinth_layers : 0.0);
  const double top = station_cell * (station_interior_layers + 1) +
                     (pose == station_pose::ceiling ? station_cell * station_plinth_layers : 0.0);
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
  const point half = scale(site.berth_axis, starter_sub_axis_half);
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
      tube(h, h.loop[i], h.loop[(i + 1) % h.loop.size()], h.body_radius + surface_guard);
    }
    for (const hall_niche& n : h.niches) {
      const double radius = 1.35 * n.radius + surface_guard + 2.5;
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
  if (site.origin.y + low.y < world_y_min + 1.0 || site.origin.y + high.y > world_y_max - 1.0) return true;
  for (const auto& [a, b] : set.routes) {
    if (segment_box_distance(a, b, low, high, starter_sub_path_radius) < starter_sub_path_radius) return true;
  }
  for (const point& p : set.turns) {
    if (box_distance(p, low, high) < starter_sub_turn_radius) return true;
  }
  for (const auto& t : set.tubes) {
    if (segment_box_distance(t.a, t.b, low, high, t.radius) < t.radius) return true;
  }
  // Корпус у причала — точная геометрия, без охранного слоя сетки скалы.
  for (const auto& [a, b] : set.berth) {
    if (segment_box_distance(a, b, low, high, hull_radius + 0.15) < hull_radius + 0.15) return true;
  }
  return false;
}

} // namespace

// Ленивая маска плана над памятью буферов генератора station (mask_band, mask_held, mask_margin,
// mask_cells): класс клетки считается при первом запросе и запоминается там, поэтому переживает вызовы
// блока station_room_cells в пределах одного места.
struct station_mask_state {
  station_site site;
  keep_out_set keep;
  // Поле пещеры до монтажа (с пещерами мест), подготовленное над всей маской.
  std::function<double(point)> natural;
  double bottom = 0.0, top = 0.0;
  int32_t cantilever = 0;
  int32_t i0 = 0, k0 = 0, width = 0, depth = 0;
  enum : uint8_t { water = 0, mixed = 1, rock = 2, unknown = 0xFF };
  uint8_t *band = nullptr, *held = nullptr, *margin = nullptr, *cells = nullptr;

  // Путь лодки к причалу вырезается вместе со станцией, поэтому для плана это вода:
  // иначе маска сочла бы глубокой породу, которую вырез потом вскроет.
  double field(const point p) const { return std::min(natural(p), berth_sweep_field(site, p)); }

  // Слой станции над клеткой: вода, смесь или порода; заодно опора и запас породы.
  uint8_t band_at(const int32_t a, const int32_t b) {
    // За краем маски — вода: подготовленная область туда не дотягивается, и маска не
    // должна придумывать там ни глубокую породу (seed 13: внутреннее помещение у края
    // получило несуществующее кольцо породы), ни опору для консоли.
    if (a < 0 || b < 0 || a >= width || b >= depth) return water;
    const size_t index = size_t(b) * width + a;
    if (band[index] != unknown) return band[index];
    const double u = (i0 + a + 0.5) * station_cell, v = (k0 + b + 0.5) * station_cell;
    uint32_t solid = 0;
    for (const double y : station_band) solid += field(station_to_world(site, u, y, v)) > 0.0;
    band[index] = solid == 0 ? water : solid == station_band.size() ? rock : mixed;
    if (band[index] == water && site.pose != station_pose::wall) {
      // Опора снизу (дно под фундаментом) или сверху (свод над подвесом).
      const double sign = site.pose == station_pose::floor ? -1.0 : 1.0;
      const double start = site.pose == station_pose::floor ? -station_cell
                                                            : station_cell * (station_interior_layers + 1);
      for (int32_t n = 1; n <= station_plinth_layers && !held[index]; ++n) {
        held[index] = field(station_to_world(site, u, start + sign * station_cell * (n - 0.5), v)) > 0.0;
      }
    } else if (band[index] == rock) {
      margin[index] = field(station_to_world(site, u, -1.0, v)) > 0.0 &&
                      field(station_to_world(site, u, 4.0, v)) > 0.0;
    }
    return band[index];
  }

  // Есть ли в квадрате радиуса r клетка другого слоя. Обход кольцами от ближнего:
  // ответ «да» обычно находится рядом, не вычисляя весь квадрат.
  bool ring(const int32_t a, const int32_t b, const int32_t r, const uint8_t differs_from) {
    for (int32_t d = 1; d <= r; ++d) {
      for (int32_t t = -d; t <= d; ++t) {
        if (band_at(a + t, b - d) != differs_from || band_at(a + t, b + d) != differs_from ||
            band_at(a - d, b + t) != differs_from || band_at(a + d, b + t) != differs_from) {
          return true;
        }
      }
    }
    return false;
  }

  mask_cell cell(const int32_t a, const int32_t b) {
    const size_t index = size_t(b) * width + a;
    if (cells[index] != unknown) return mask_cell(cells[index]);
    const double u0 = (i0 + a) * station_cell, v0 = (k0 + b) * station_cell;
    mask_cell result;
    if (box_forbidden(site, keep, {u0, bottom, v0}, {u0 + station_cell, top, v0 + station_cell})) {
      result = mask_cell::forbidden;
    } else {
      const uint8_t here = band_at(a, b);
      if (here == rock) {
        // Глубокая: порода над и под этажом и на метр вокруг на всю его высоту.
        result = margin[index] && !ring(a, b, deep_ring, rock) ? mask_cell::deep : mask_cell::rock;
      } else if (here == mixed) {
        result = mask_cell::mixed;
      } else {
        result = held[index] || ring(a, b, cantilever, water) ? mask_cell::water : mask_cell::open_water;
      }
    }
    cells[index] = uint8_t(result);
    return result;
  }

  mask_cell at_plan(const int32_t i, const int32_t k) {
    const int32_t a = i - i0, b = k - k0;
    if (a < 0 || b < 0 || a >= width || b >= depth) return mask_cell::forbidden;
    return cell(a, b);
  }
};

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

// Клетка лежит в диагональной полосе стены у скошенного угла.
bool chamfer_wall(const station_room& r, const int32_t i, const int32_t k) {
  const int32_t a[4] = {i - r.u0, r.u1 - 1 - i, r.u1 - 1 - i, i - r.u0};
  const int32_t b[4] = {k - r.v0, k - r.v0, r.v1 - 1 - k, r.v1 - 1 - k};
  for (uint32_t c = 0; c < 4; ++c) {
    const int32_t cut = r.chamfer[c];
    if (cut == 0 || a[c] < -1 || b[c] < -1) continue;
    if (a[c] + b[c] == cut - 2 || a[c] + b[c] == cut - 1) return true;
  }
  return false;
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
                                const station_plan& plan) {
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
  // Поле: причал в воде, внутренние помещения в породе с запасом, наружные стоят
  // на опоре, ни один столб не задевает маршрут, поворот и путь лодки.
  field_cache sweep_field(world, false, 48.0);
  const point centre = anchor.centre;
  if (!berth_sweep_in_water(sweep_field, centre, site.berth, site.berth_axis)) {
    return "station berth is not naturally open";
  }
  const keep_out_set keep = keep_out_for(world, anchor, site);
  const auto [bottom, top] = column_range(site.pose);
  int32_t i0 = INT32_MAX, k0 = INT32_MAX, i1 = INT32_MIN, k1 = INT32_MIN;
  for (const station_room& r : plan.rooms) {
    i0 = std::min(i0, r.u0 - 4); k0 = std::min(k0, r.v0 - 4);
    i1 = std::max(i1, r.u1 + 4); k1 = std::max(k1, r.v1 + 4);
  }
  const field_region region = station_region(world, site, i0, k0, i1, k1);
  const auto field = [&](const point p) { return cave_density(region, p); };
  for (uint32_t index = 0; index < plan.rooms.size(); ++index) {
    const station_room& r = plan.rooms[index];
    for (int32_t i = r.u0 - 1; i <= r.u1; ++i) for (int32_t k = r.v0 - 1; k <= r.v1; ++k) {
      if (!room_outline(r, i, k)) continue;
      const double u = i * station_cell, v = k * station_cell;
      if (box_forbidden(site, keep, {u, bottom, v}, {u + station_cell, top, v + station_cell})) {
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
          const point p = station_to_world(site, (i + 0.5) * station_cell, y, (k + 0.5) * station_cell);
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

station_voxels build_station_voxels(const std::function<double(point)>& field, const station_plan& plan) {
  station_voxels voxels;
  int32_t u0 = INT32_MAX, v0 = INT32_MAX, u1 = INT32_MIN, v1 = INT32_MIN;
  for (const station_room& r : plan.rooms) {
    u0 = std::min(u0, r.u0 - 1); v0 = std::min(v0, r.v0 - 1);
    u1 = std::max(u1, r.u1 + 1); v1 = std::max(v1, r.v1 + 1);
  }
  voxels.i0 = u0; voxels.k0 = v0;
  voxels.width = u1 - u0; voxels.depth = v1 - v0;
  voxels.y0 = -1 - station_plinth_layers;
  voxels.height = station_interior_layers + 2 + 2 * station_plinth_layers;
  const size_t plane = size_t(voxels.width) * voxels.depth;
  voxels.solid.assign(plane * voxels.height, 0);
  voxels.shade.assign(plane * voxels.height, 0);
  std::vector<uint8_t> air(plane, 0), outline(plane, 0), owners(plane, 0), bevel(plane, 0);
  for (const station_room& r : plan.rooms) {
    for (int32_t i = r.u0 - 1; i <= r.u1; ++i) for (int32_t k = r.v0 - 1; k <= r.v1; ++k) {
      const size_t index = size_t(k - v0) * voxels.width + (i - u0);
      const uint8_t contour = room_outline(r, i, k);
      air[index] |= room_interior(r, i, k);
      outline[index] |= contour;
      if (contour) owners[index] = uint8_t(std::min(owners[index] + 1, 255));
      // Скос наружного помещения — кандидат в окно; внутренние скосы в породе стальные.
      if (contour && !r.inner && chamfer_wall(r, i, k)) bevel[index] = 1;
    }
  }
  for (const station_door& d : plan.doors) {
    for (int32_t i = d.u0; i < d.u1; ++i) for (int32_t k = d.v0; k < d.v1; ++k) {
      air[size_t(k - v0) * voxels.width + (i - u0)] = 0xF;
    }
  }
  const bool derelict = plan.kind == theme::derelict;
  const uint8_t wall_shade = derelict ? 115 : 123, slab_shade = derelict ? 112 : 119;
  const bool wall_hatch = plan.site.pose == station_pose::wall;
  const auto in_hatch = [&](const int32_t i, const int32_t k) {
    return i >= plan.hatch_u0 && i < plan.hatch_u1 && k >= plan.hatch_v0 && k < plan.hatch_v1;
  };
  for (int32_t k = v0; k < v1; ++k) for (int32_t i = u0; i < u1; ++i) {
    const size_t cell = size_t(k - v0) * voxels.width + (i - u0);
    const uint8_t footprint = outline[cell];
    if (!footprint) continue;
    const uint8_t wall = uint8_t(footprint & ~air[cell]);
    const bool hatch = in_hatch(i, k);
    for (int32_t y = voxels.y0; y < voxels.y0 + voxels.height; ++y) {
      uint8_t q = 0, shade = wall_shade;
      if (y == -1) {
        q = wall_hatch || !hatch || plan.site.pose != station_pose::ceiling ? footprint : 0;
        shade = slab_shade;
      } else if (y == station_interior_layers) {
        q = wall_hatch || !hatch || plan.site.pose != station_pose::floor ? footprint : 0;
        shade = slab_shade;
      } else if (y >= 0 && y < station_interior_layers) {
        // Люк в стене — проём 1x2 м в нижних четырёх слоях, рамка золотая.
        q = hatch && wall_hatch && y < 4 ? 0 : wall;
        if (wall_hatch && (hatch || in_hatch(i, k - 1) || in_hatch(i, k + 1))) shade = 126;
        // Окно в скосе: стекло от 0.5 до 2.5 м, если за стеной вода, а не порода и
        // не соседнее помещение. Подоконник и перемычка остаются стальными.
        if (q && bevel[cell] && owners[cell] == 1 && y >= 1 && y <= 4 &&
            field(station_to_world(plan.site, (i + 0.5) * station_cell, (y + 0.5) * station_cell,
                                   (k + 0.5) * station_cell)) < 0.0) shade = station_glass_shade;
      }
      const size_t index = size_t(y - voxels.y0) * plane + cell;
      voxels.solid[index] = q;
      voxels.shade[index] = shade;
    }
    if (!wall_hatch && hatch) {
      for (const int32_t y : {-1, station_interior_layers}) {
        voxels.shade[size_t(y - voxels.y0) * plane + cell] = 126;
      }
    }
    // Фундамент вниз до дна и подвес вверх до свода, ступенями по слою.
    const double u = (i + 0.5) * station_cell, v = (k + 0.5) * station_cell;
    // Ещё один слой заходит в породу: вырез станции отодвигает скалу от плиты.
    const auto extend = [&](const int32_t start, const int32_t step) {
      for (int32_t n = 0; n < station_plinth_layers; ++n) {
        const int32_t y = start + step * n;
        const bool rock = field(station_to_world(plan.site, u, (y + 0.5) * station_cell, v)) > 0.0;
        const size_t index = size_t(y - voxels.y0) * plane + cell;
        voxels.solid[index] = footprint;
        voxels.shade[index] = slab_shade;
        if (rock) return;
      }
    };
    if (plan.site.pose == station_pose::floor) extend(-2, -1);
    if (plan.site.pose == station_pose::ceiling) extend(station_interior_layers + 1, 1);
  }
  return voxels;
}

std::vector<station_triangle> station_mesh(const station_plan& plan, const station_voxels& voxels) {
  std::vector<station_triangle> mesh;
  const station_site& site = plan.site;
  const point du = axis_u(site.yaw), dv = axis_v(site.yaw);
  const auto world = [&](const double u, const double y, const double v) {
    return station_to_world(site, u, y, v);
  };
  const auto emit = [&](point a, point b, point c, const point normal, const uint8_t shade) {
    if (dot(cross(subtract(b, a), subtract(c, a)), normal) < 0.0) std::swap(b, c);
    mesh.push_back({a, b, c, normal, shade});
  };
  const auto local_normal = [&](const double nu, const double ny, const double nv) {
    return point{du.x * nu + dv.x * nv, ny, du.z * nu + dv.z * nv};
  };
  // Направление стороны s в осях (u, v) и концы её ребра.
  static constexpr int32_t side_u[4] = {-1, 0, 1, 0}, side_v[4] = {0, 1, 0, -1};
  for (int32_t y = voxels.y0; y < voxels.y0 + voxels.height; ++y) {
    for (int32_t k = voxels.k0; k < voxels.k0 + voxels.depth; ++k) {
      for (int32_t i = voxels.i0; i < voxels.i0 + voxels.width; ++i) {
        const uint8_t q = voxels.at(i, y, k);
        if (!q) continue;
        const uint8_t shade = voxels.shade[(size_t(y - voxels.y0) * voxels.depth + (k - voxels.k0)) *
                                           voxels.width + (i - voxels.i0)];
        const double u0 = i * station_cell, u1 = u0 + station_cell;
        const double v0 = k * station_cell, v1 = v0 + station_cell;
        const double y0 = y * station_cell, y1 = y0 + station_cell;
        const double uc = (u0 + u1) / 2.0, vc = (v0 + v1) / 2.0;
        // Углы клетки между сторонами s и s+1.
        const double corner_u[4] = {u0, u1, u1, u0}, corner_v[4] = {v1, v1, v0, v0};
        const auto side_ends = [&](const uint32_t s) {
          return std::pair{std::pair{corner_u[(s + 3) % 4], corner_v[(s + 3) % 4]},
                           std::pair{corner_u[s], corner_v[s]}};
        };
        for (uint32_t s = 0; s < 4; ++s) {
          if (!(q & (1u << s))) continue;
          const auto [p, r] = side_ends(s);
          // Сторона клетки, если у соседа противоположная четверть пуста.
          const uint8_t n = voxels.at(i + side_u[s], y, k + side_v[s]);
          if (!(n & (1u << ((s + 2) % 4)))) {
            const point normal = local_normal(side_u[s], 0, side_v[s]);
            const point a = world(p.first, y0, p.second), b = world(r.first, y0, r.second);
            const point c = world(r.first, y1, r.second), d = world(p.first, y1, p.second);
            emit(a, b, c, normal, shade);
            emit(a, c, d, normal, shade);
          }
          // Половина диагонали к следующей четверти.
          const uint32_t t = (s + 1) % 4;
          if (!(q & (1u << t))) {
            const double nu = side_u[t] - side_u[s], nv = side_v[t] - side_v[s];
            const point normal = local_normal(nu / std::sqrt(2.0), 0, nv / std::sqrt(2.0));
            const point a = world(uc, y0, vc), b = world(corner_u[s], y0, corner_v[s]);
            const point c = world(corner_u[s], y1, corner_v[s]), d = world(uc, y1, vc);
            emit(a, b, c, normal, shade);
            emit(a, c, d, normal, shade);
          }
          const uint32_t back = (s + 3) % 4;
          if (!(q & (1u << back))) {
            const double nu = side_u[back] - side_u[s], nv = side_v[back] - side_v[s];
            const point normal = local_normal(nu / std::sqrt(2.0), 0, nv / std::sqrt(2.0));
            const point a = world(uc, y0, vc), b = world(corner_u[back], y0, corner_v[back]);
            const point c = world(corner_u[back], y1, corner_v[back]), d = world(uc, y1, vc);
            emit(a, b, c, normal, shade);
            emit(a, c, d, normal, shade);
          }
          // Верх и низ четверти.
          if (!(voxels.at(i, y + 1, k) & (1u << s))) {
            emit(world(uc, y1, vc), world(p.first, y1, p.second), world(r.first, y1, r.second),
                 {0, 1, 0}, shade);
          }
          if (!(voxels.at(i, y - 1, k) & (1u << s))) {
            emit(world(uc, y0, vc), world(p.first, y0, p.second), world(r.first, y0, r.second),
                 {0, -1, 0}, shade);
          }
        }
      }
    }
  }
  return mesh;
}

// ---------------------------------------------------------------------------------------
// Станция в мире

namespace {

// Коробка станции: воксели со всем запасом выреза и люка и путь лодки к причалу. За коробкой вырез
// больше фоновой скалы (3 + 0.46): отбросить его — точно. false — станция не помещается в куб
// station_reach вокруг узла.
bool station_box(const station_plan& plan, const station_voxels& v, point& low_out, point& high_out) {
  point low{1e30, 1e30, 1e30}, high{-1e30, -1e30, -1e30};
  const double margin = 5.0;
  for (const int32_t i : {v.i0, v.i0 + v.width}) for (const int32_t k : {v.k0, v.k0 + v.depth}) {
    for (const int32_t y : {v.y0, v.y0 + v.height}) {
      const point p = station_to_world(plan.site, i * station_cell, y * station_cell, k * station_cell);
      low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
      high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
  }
  const station_site& site = plan.site;
  const point half = scale(site.berth_axis, starter_sub_axis_half);
  for (const point p : {add(site.node_centre, half), subtract(site.node_centre, half),
                        add(site.berth, half), subtract(site.berth, half)}) {
    low = {std::min(low.x, p.x - starter_sub_path_radius), std::min(low.y, p.y - starter_sub_path_radius),
           std::min(low.z, p.z - starter_sub_path_radius)};
    high = {std::max(high.x, p.x + starter_sub_path_radius), std::max(high.y, p.y + starter_sub_path_radius),
            std::max(high.z, p.z + starter_sub_path_radius)};
  }
  low_out = {low.x - margin, low.y - margin, low.z - margin};
  high_out = {high.x + margin, high.y + margin, high.z + margin};
  const point& c = site.node_centre;
  return !(low_out.x < c.x - station_reach || high_out.x > c.x + station_reach ||
           low_out.y < c.y - station_reach || high_out.y > c.y + station_reach ||
           low_out.z < c.z - station_reach || high_out.z > c.z + station_reach);
}

namespace originator = devils_engine::originator;

point read3(const originator::const_field_accessor& f, const size_t i) { return {f.get(i, 0), f.get(i, 1), f.get(i, 2)}; }
point read3(const originator::field_accessor& f, const size_t i) { return {f.get(i, 0), f.get(i, 1), f.get(i, 2)}; }
void write3(const originator::field_accessor& f, const size_t i, const point p) {
  f.set(i, p.x, 0); f.set(i, p.y, 1); f.set(i, p.z, 2);
}

// Поле пещеры до монтажа над коробкой плана [i0,i1)x[k0,k1) со всем вертикальным запасом станции: та же
// коробка, что у station_region, но над полем цепочки.
std::function<double(point)> chain_station_field(const std::string_view field_name, const station_site& site,
                                                 const int32_t i0, const int32_t k0, const int32_t i1, const int32_t k1) {
  point low{1e30, site.origin.y - 6.0, 1e30}, high{-1e30, site.origin.y + 10.0, -1e30};
  for (const int32_t i : {i0, i1}) for (const int32_t k : {k0, k1}) {
    const point p = station_to_world(site, i * station_cell, 0.0, k * station_cell);
    low.x = std::min(low.x, p.x - 1.0); high.x = std::max(high.x, p.x + 1.0);
    low.z = std::min(low.z, p.z - 1.0); high.z = std::max(high.z, p.z + 1.0);
  }
  const auto& field = originator::current_field(field_name);
  auto region = std::make_shared<originator::primitive_field::region>(
    field.prepare({low.x, low.y, low.z}, {high.x, high.y, high.z}));
  return [region](const point p) { return region->value({p.x, p.y, p.z}).value; };
}

// Место станции из её буферов (входы 0..5: поза, начало, поворот, узел, причал, ось причала).
station_site site_from(const originator::tool_call& call, const bool from_outputs) {
  station_site site;
  if (from_outputs) {
    site.pose = station_pose(uint8_t(call.output(1).write().get(0)));
    site.origin = read3(call.output(2).write(), 0);
    site.yaw = call.output(3).write().get(0);
    site.node_centre = read3(call.output(4).write(), 0);
    site.berth = read3(call.output(5).write(), 0);
    site.berth_axis = read3(call.output(6).write(), 0);
  } else {
    site.pose = station_pose(uint8_t(call.input(0).read().get(0)));
    site.origin = read3(call.input(1).read(), 0);
    site.yaw = call.input(2).read().get(0);
    site.node_centre = read3(call.input(3).read(), 0);
    site.berth = read3(call.input(4).read(), 0);
    site.berth_axis = read3(call.input(5).read(), 0);
  }
  return site;
}

// Блок генератора station: классы клеток маски под прямоугольником помещения [u0-1, u1] x [v0-1, v1].
// Отказ — если хоть одна клетка запретна или вода без опоры рядом; иначе счётчики внутренности: всего,
// в породе, глубоко в породе, в воде с опорой. Маска ленивая, её состояние — в выходах 0..3.
void tool_station_room_cells(const originator::tool_call& call, const size_t, const size_t) {
  station_mask_state state;
  state.site = site_from(call, false);
  const auto param = [&](const char* name) { return int32_t(call.params->integer(name)); };
  const auto routes_a = call.input(6).read(), routes_b = call.input(7).read(), turns = call.input(8).read(),
             berth_a = call.input(9).read(), berth_b = call.input(10).read(), tubes_a = call.input(11).read(),
             tubes_b = call.input(12).read(), tubes_r = call.input(13).read();
  for (int32_t i = 0; i < param("routes"); ++i) state.keep.routes.push_back({read3(routes_a, size_t(i)), read3(routes_b, size_t(i))});
  for (int32_t i = 0; i < param("turns"); ++i) state.keep.turns.push_back(read3(turns, size_t(i)));
  for (int32_t i = 0; i < param("berths"); ++i) state.keep.berth.push_back({read3(berth_a, size_t(i)), read3(berth_b, size_t(i))});
  for (int32_t i = 0; i < param("tubes"); ++i) {
    state.keep.tubes.push_back({read3(tubes_a, size_t(i)), read3(tubes_b, size_t(i)), tubes_r.get(size_t(i))});
  }
  state.i0 = param("i0"); state.k0 = param("k0"); state.width = param("width"); state.depth = param("depth");
  std::tie(state.bottom, state.top) = column_range(state.site.pose);
  // Консоль наружной части от породы: у стены до 6 м, у дна и свода — 4 м.
  state.cantilever = state.site.pose == station_pose::wall ? 12 : 8;
  const size_t cells = size_t(state.width) * size_t(state.depth);
  const auto span = [&](const size_t output) {
    auto s = call.output(output).write().as_span<uint8_t>();
    if (s.size() < cells) devils_engine::utils::error{}("GN09 station_room_cells: mask buffer holds {} of {} cells", s.size(), cells);
    return s.data();
  };
  state.band = span(0); state.held = span(1); state.margin = span(2); state.cells = span(3);
  state.natural = chain_station_field(call.params->string("field"), state.site, state.i0, state.k0,
                                      state.i0 + state.width, state.k0 + state.depth);
  const int32_t u0 = param("u0"), v0 = param("v0"), u1 = param("u1"), v1 = param("v1");
  uint32_t total = 0, solid = 0, deep = 0, water = 0;
  bool refused = false;
  for (int32_t i = u0 - 1; i <= u1 && !refused; ++i) for (int32_t k = v0 - 1; k <= v1; ++k) {
    const mask_cell c = state.at_plan(i, k);
    if (c == mask_cell::forbidden || c == mask_cell::open_water) { refused = true; break; }
    if (!(i >= u0 && i < u1 && k >= v0 && k < v1)) continue;
    ++total;
    solid += c == mask_cell::rock || c == mask_cell::deep;
    deep += c == mask_cell::deep;
    water += c == mask_cell::water;
  }
  call.output(4).write().set(0, refused ? 1.0 : 0.0);
  call.output(5).write().set(0, double(total));
  call.output(6).write().set(0, double(solid));
  call.output(7).write().set(0, double(deep));
  call.output(8).write().set(0, double(water));
}

// План станции из буферов помещений и дверей (входы) и её мета (выходы 0..9).
station_plan plan_from(const originator::tool_call& call) {
  station_plan plan;
  plan.site = site_from(call, true);
  const auto rect = call.input(0).read(), chamfer = call.input(1).read(), inner = call.input(2).read();
  const auto door_a = call.input(3).read(), door_b = call.input(4).read(), door_rect = call.input(5).read();
  for (int64_t i = 0; i < call.params->integer("rooms"); ++i) {
    station_room r;
    const size_t at = size_t(i);
    r.u0 = int32_t(rect.get(at, 0)); r.v0 = int32_t(rect.get(at, 1)); r.u1 = int32_t(rect.get(at, 2)); r.v1 = int32_t(rect.get(at, 3));
    for (uint32_t c = 0; c < 4; ++c) r.chamfer[c] = uint8_t(chamfer.get(at, c));
    r.inner = inner.get(at) != 0.0;
    plan.rooms.push_back(r);
  }
  for (int64_t i = 0; i < call.params->integer("doors"); ++i) {
    const size_t at = size_t(i);
    plan.doors.push_back({uint32_t(door_a.get(at)), uint32_t(door_b.get(at)), int32_t(door_rect.get(at, 0)),
                          int32_t(door_rect.get(at, 1)), int32_t(door_rect.get(at, 2)), int32_t(door_rect.get(at, 3))});
  }
  const auto hatch = call.output(7).write();
  plan.hatch_u0 = int32_t(hatch.get(0, 0)); plan.hatch_v0 = int32_t(hatch.get(0, 1));
  plan.hatch_u1 = int32_t(hatch.get(0, 2)); plan.hatch_v1 = int32_t(hatch.get(0, 3));
  plan.kind = theme(uint8_t(call.output(8).write().get(0)));
  return plan;
}

// Блок генератора station: воксели, сетка и коробка готового плана. built = 0, если станция не
// помещается в куб досягаемости вокруг узла.
void tool_station_build(const originator::tool_call& call, const size_t, const size_t) {
  const station_plan plan = plan_from(call);
  int32_t u0 = INT32_MAX, v0 = INT32_MAX, u1 = INT32_MIN, v1 = INT32_MIN;
  for (const station_room& r : plan.rooms) {
    u0 = std::min(u0, r.u0 - 1); v0 = std::min(v0, r.v0 - 1);
    u1 = std::max(u1, r.u1 + 1); v1 = std::max(v1, r.v1 + 1);
  }
  const station_voxels voxels =
    build_station_voxels(chain_station_field(call.params->string("field"), plan.site, u0, v0, u1, v1), plan);
  const std::vector<station_triangle> mesh = station_mesh(plan, voxels);
  point low, high;
  const bool fits = station_box(plan, voxels, low, high);
  const auto mesh_a = call.output(14).write(), mesh_b = call.output(15).write(), mesh_c = call.output(16).write(),
             mesh_n = call.output(17).write(), mesh_s = call.output(18).write();
  const auto solid = call.output(19).write(), shade = call.output(20).write();
  if (mesh.size() > mesh_a.count() || voxels.solid.size() > solid.count()) {
    devils_engine::utils::error{}("GN09 station_build: {} triangles / {} voxels exceed capacities {} / {}", mesh.size(),
                                  voxels.solid.size(), mesh_a.count(), solid.count());
  }
  for (size_t i = 0; i < mesh.size(); ++i) {
    write3(mesh_a, i, mesh[i].a); write3(mesh_b, i, mesh[i].b); write3(mesh_c, i, mesh[i].c);
    write3(mesh_n, i, mesh[i].normal); mesh_s.set(i, double(mesh[i].shade));
  }
  for (size_t i = 0; i < voxels.solid.size(); ++i) { solid.set(i, double(voxels.solid[i])); shade.set(i, double(voxels.shade[i])); }
  call.output(9).write().set(0, double(mesh.size()));
  const auto origin = call.output(10).write(), size = call.output(11).write();
  origin.set(0, voxels.i0, 0); origin.set(0, voxels.k0, 1); origin.set(0, voxels.y0, 2);
  size.set(0, voxels.width, 0); size.set(0, voxels.depth, 1); size.set(0, voxels.height, 2);
  write3(call.output(12).write(), 0, low);
  write3(call.output(13).write(), 0, high);
  call.output(0).write().set(0, fits ? 1.0 : 0.0);
}

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
  return station;
}

} // namespace

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
  // Станция — экземпляр генератора station (generator/station/scripts/station.lua) в канонической
  // окрестности своего якоря; разобранный C++-вид живёт с экземпляром, кэш — кэш цепочки.
  return station_view(*station_instance(world, at));
}

void add_station_tools(originator::tool_registry& tools) {
  tools.add(originator::tool_description{.name = "station_room_cells", .shape = originator::aperture::sequential,
                                         .input_count = 14, .output_count = 9, .body = tool_station_room_cells,
                                         .footprint = originator::no_temporary_memory});
  tools.add(originator::tool_description{.name = "station_build", .shape = originator::aperture::sequential,
                                         .input_count = 6, .output_count = 21, .body = tool_station_build,
                                         .footprint = originator::no_temporary_memory});
}

bool station_reaches(const built_station& station, const point low, const point high) {
  return station.low.x <= high.x && station.high.x >= low.x && station.low.y <= high.y &&
         station.high.y >= low.y && station.low.z <= high.z && station.high.z >= low.z;
}

namespace {

// Знаковое расстояние до многоугольника помещения в плане (метры): прямоугольник
// и срезы углов как пересечение полуплоскостей. Внутри отрицательно.
double room_plan_field(const station_room& r, const double u, const double v) {
  const double u0 = r.u0 * station_cell, u1 = r.u1 * station_cell;
  const double v0 = r.v0 * station_cell, v1 = r.v1 * station_cell;
  double d = std::max({u0 - u, u - u1, v0 - v, v - v1});
  const double cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
  const double su[4] = {1, -1, -1, 1}, sv[4] = {1, 1, -1, -1};
  for (uint32_t c = 0; c < 4; ++c) {
    if (!r.chamfer[c]) continue;
    const double cut = r.chamfer[c] * station_cell;
    d = std::max(d, (cut - (su[c] * (u - cu[c]) + sv[c] * (v - cv[c]))) / std::sqrt(2.0));
  }
  return d;
}

double cell_box_field(const int32_t i0, const int32_t k0, const int32_t i1, const int32_t k1,
                      const double y0, const double y1, const double u, const double y, const double v) {
  return std::max({i0 * station_cell - u, u - i1 * station_cell, k0 * station_cell - v,
                   v - k1 * station_cell, y0 - y, y - y1});
}

} // namespace

double berth_sweep_field(const station_site& site, const point p) {
  // Параллелограмм: ось лодки (вдоль berth_axis) переносится от узла к причалу.
  const point half = scale(site.berth_axis, starter_sub_axis_half);
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
  return distance - starter_sub_path_radius;
}

double station_carve_field(const built_station& station, const point position) {
  const station_plan& plan = station.plan;
  const point d = subtract(position, plan.site.origin);
  const double u = dot(d, axis_u(plan.site.yaw)), y = d.y, v = dot(d, axis_v(plan.site.yaw));
  const double ceiling = station_cell * station_interior_layers;
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
  } else if (plan.site.pose == station_pose::floor) {
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
  if (y <= 0.0 || y >= station_cell * station_interior_layers) return false;
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
  std::array<point, 12> a{}, b{};
  for (const station_triangle& t : mesh) {
    if (std::max({t.a.x, t.b.x, t.c.x}) < low.x || std::min({t.a.x, t.b.x, t.c.x}) >= high.x ||
        std::max({t.a.y, t.b.y, t.c.y}) < low.y || std::min({t.a.y, t.b.y, t.c.y}) >= high.y ||
        std::max({t.a.z, t.b.z, t.c.z}) < low.z || std::min({t.a.z, t.b.z, t.c.z}) >= high.z) continue;
    if (skip && skip(t)) continue;
    size_t count = 3;
    a[0] = t.a; a[1] = t.b; a[2] = t.c;
    // Сазерленд — Ходжман по шести плоскостям: [low, high) по каждой оси.
    for (uint32_t axis = 0; axis < 3 && count; ++axis) {
      for (const bool upper : {false, true}) {
        const auto coord = [&](const point& p) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
        const double plane = axis == 0 ? (upper ? high.x : low.x) : axis == 1 ? (upper ? high.y : low.y)
                                                                              : (upper ? high.z : low.z);
        // Полуоткрыто: грань ровно на верхней плоскости достаётся соседнему чанку.
        const auto inside = [&](const point& p) { return upper ? coord(p) < plane : coord(p) >= plane; };
        size_t out = 0;
        for (size_t i = 0; i < count; ++i) {
          const point& p = a[i];
          const point& q = a[(i + 1) % count];
          const bool pin = inside(p), qin = inside(q);
          if (pin) b[out++] = p;
          if (pin != qin) {
            const double t = (plane - coord(p)) / (coord(q) - coord(p));
            b[out++] = add(p, scale(subtract(q, p), t));
          }
        }
        count = out;
        std::copy(b.begin(), b.begin() + out, a.begin());
        if (!count) break;
      }
    }
    if (count >= 3) polygon(a.data(), count, t.normal, t.shade);
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
  const double au = dot(chosen->berth_axis, axis_u(chosen->yaw)) * starter_sub_axis_half;
  const double av = dot(chosen->berth_axis, axis_v(chosen->yaw)) * starter_sub_axis_half;
  out << "<line x1=\"" << bu - au << "\" y1=\"" << bv - av << "\" x2=\"" << bu + au << "\" y2=\""
      << bv + av << "\" stroke=\"#ff8c5a\" stroke-opacity=\"0.6\" stroke-width=\"6\" stroke-linecap=\"round\"/>\n";
  out << "<text x=\"" << mask.i0 * 0.5 + 1 << "\" y=\"" << mask.k0 * 0.5 + 2 << "\" font-size=\"1.2\" fill=\"white\">"
      << name(chosen->pose) << " tilt " << chosen->tilt_deg << " residual " << chosen->residual
      << (failed ? " — programme did not fit" : "") << "</text>\n";
  out << "</svg>\n";
  return bool(out);
}

} // namespace gn09
