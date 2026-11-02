#include "adventure.h"
#include "local_structure.h"
#include "world_chain.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <numbers>
#include <optional>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"

// Уровень залов. Регион 768 м решает, где в нём лежат большие залы, а граф секторов
// 96 м — где базы и галереи. Зал стоит на высоте слоя, поэтому маршрут проходит
// сквозь него: сектор, чья база попала бы в зал, получает вместо базы узел внутри
// зала, и обычные рёбра соседей входят в зал через стены. Зал — функция только seed
// и региона (графа он не читает), а граф читает залы: зависимость односторонняя.

namespace gn09 {
namespace {

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const point a) { return std::sqrt(dot(a, a)); }
point normalize(const point a) { return scale(a, 1.0 / length(a)); }


double segment_distance(const point p, const point a, const point b) {
  const point ab = subtract(b, a);
  const double span = dot(ab, ab);
  const double t = span > 0.0 ? std::clamp(dot(subtract(p, a), ab) / span, 0.0, 1.0) : 0.0;
  return length(subtract(p, add(a, scale(ab, t))));
}

double box_distance(const point p, const point centre, const point half) {
  const point q{std::abs(p.x - centre.x) - half.x, std::abs(p.y - centre.y) - half.y,
                std::abs(p.z - centre.z) - half.z};
  const point outside{std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0)};
  return length(outside) + std::min(std::max({q.x, q.y, q.z}), 0.0);
}

// Рельеф стен: крупный и мелкий слои. Модуль их суммы не больше relief_bound.
constexpr double relief_large = 5.0, relief_large_period = 31.0;
constexpr double relief_small = 1.5, relief_small_period = 9.0;
constexpr double relief_bound = relief_large + relief_small;
// Коридор и ниши неровные только наружу: радиус в [r, r + 2 * relief].
constexpr double core_relief = 4.0;
constexpr double niche_relief = 2.5;
// Ниша — не высверленная труба: сечение вытянуто по высоте, у входа раструб.
constexpr double niche_stretch = 1.35;
constexpr double niche_flare = 0.6;
constexpr double niche_flare_reach = 0.45; // доля входной трубки с раструбом
// Фоновая скала cave_density: 3 + 0.30 broad + 0.16 detail.
double pillar_flare() { return world_constants().number("pillar_flare"); }

point right_of(const hall& h) { return {-h.forward.z, 0, h.forward.x}; }

point to_local(const hall& h, const point p) {
  const point d = subtract(p, h.centre);
  return {dot(d, h.forward), d.y, dot(d, right_of(h))};
}

point to_world(const hall& h, const double u, const double y, const double v) {
  return add(h.centre, add(add(scale(h.forward, u), scale(right_of(h), v)), {0, y, 0}));
}

point to_world(const hall& h, const point local) { return to_world(h, local.x, local.y, local.z); }

double smallest_radius(const hall& h) {
  return std::min({h.half_length, h.half_width, h.half_height});
}

// Приближение расстояния до эллипсоида k0 (k0 - 1) / k1. Снаружи оно не меньше
// (k0 - 1) * min_radius, внутри не больше -(1 - k0) * min_radius: на этих оценках
// стоят границы влияния и проверка «вся область — вода».
double ellipsoid_distance(const hall& h, const point local) {
  const double a = local.x / h.half_length, b = local.y / h.half_height,
               c = local.z / h.half_width;
  const double k0 = std::sqrt(a * a + b * b + c * c);
  const double a2 = a / h.half_length, b2 = b / h.half_height, c2 = c / h.half_width;
  const double k1 = std::sqrt(a2 * a2 + b2 * b2 + c2 * c2);
  return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -smallest_radius(h);
}


// Твёрдая часть зала без шума: стены, дно, колонны, уступы. Отрицательно — вода.
double hall_solid(const hall& h, const point local) {
  double air = std::max(ellipsoid_distance(h, local), h.floor - local.y);
  const double flare = 1.0 + pillar_flare() * (local.y / h.half_height) * (local.y / h.half_height);
  for (const hall_pillar& pillar : h.pillars) {
    air = std::max(air, pillar.radius * flare - std::hypot(local.x - pillar.u, local.z - pillar.v));
  }
  for (const hall_ledge& ledge : h.ledges) {
    air = std::max(air, -box_distance(local, ledge.centre, ledge.half));
  }
  return air;
}

// Расстояние до оси петли патруля (замкнутая ломаная).
double loop_distance(const hall& h, const point local) {
  double best = 1e30;
  for (size_t i = 0; i < h.loop.size(); ++i) {
    best = std::min(best, segment_distance(local, h.loop[i], h.loop[(i + 1) % h.loop.size()]));
  }
  return best;
}

// Петля вырезана под тело с охранным слоем сетки; неровность только наружу.
double loop_clearance(const hall& h) { return h.body_radius + surface_guard(); }

// Расстояние до поверхности трубки ниши: сечение вытянуто по высоте (вертикальная
// составляющая смещения сжата), у входа раструб. Оба приёма только расширяют
// трубку, поэтому круглая трубка радиуса r остаётся внутри.
double niche_tube(const point local, const point a, const point b, const double radius,
                  const bool flared) {
  const point ab = subtract(b, a);
  const double t = std::clamp(dot(subtract(local, a), ab) / dot(ab, ab), 0.0, 1.0);
  const point offset = subtract(local, add(a, scale(ab, t)));
  const double across = length({offset.x, offset.y / niche_stretch, offset.z});
  const double open = flared && t < niche_flare_reach
    ? (1.0 - t / niche_flare_reach) * (1.0 - t / niche_flare_reach) : 0.0;
  return across - radius * (1.0 + niche_flare * open);
}

// Расстояние до поверхности ниши без рельефа: две трубки и сфера поворота в колене.
double niche_distance(const hall_niche& niche, const point local) {
  return std::min({niche_tube(local, niche.mouth, niche.elbow, niche.radius, true),
                   niche_tube(local, niche.elbow, niche.end, niche.radius, false),
                   length(subtract(local, niche.elbow)) - starter_sub_turn_radius()});
}

// Лодка в нише стоит у её дна; именно эту точку не должно быть видно с коридора.
point niche_target(const hall_niche& niche) {
  const point axis = normalize(subtract(niche.end, niche.elbow));
  return subtract(niche.end, scale(axis, 5.0));
}

// Каждый луч от станции петли до укрытия упирается в скалу.
bool niche_hidden(const hall& h, const hall_niche& niche,
                  const std::function<bool(const point& local)>& rock) {
  const point target = niche_target(niche);
  // Ближние к нише станции первыми: видимую нишу они отбрасывают быстрее всего.
  std::vector<point> stations = loop_stations(h);
  std::sort(stations.begin(), stations.end(), [&](const point a, const point b) {
    return length(subtract(a, target)) < length(subtract(b, target));
  });
  for (const point from : stations) {
    const point ray = subtract(target, from);
    const double span = length(ray);
    bool blocked = false;
    for (double t = 0.0; t < span - 2.0 && !blocked; t += 2.0) {
      blocked = rock(add(from, scale(ray, t / span)));
    }
    if (!blocked) return false;
  }
  return true;
}

// Полуразмеры (в локальных осях), вне которых поле зала больше фоновой скалы.
point relevance_half(const hall& h) {
  return h.relevance;
}

bool boxes_overlap(const point low_a, const point high_a, const point low_b, const point high_b) {
  return low_a.x <= high_b.x && high_a.x >= low_b.x && low_a.y <= high_b.y &&
         high_a.y >= low_b.y && low_a.z <= high_b.z && high_a.z >= low_b.z;
}

} // namespace

const char* name(const hall_kind value) {
  return value == hall_kind::lair ? "lair" : "pillared";
}

const char* name(const creature_class value) {
  return value == creature_class::lord ? "lord" : value == creature_class::beast ? "beast" : "none";
}

std::vector<point> loop_stations(const hall& h) {
  std::vector<point> out;
  for (size_t i = 0; i < h.loop.size(); ++i) {
    const point a = h.loop[i], b = h.loop[(i + 1) % h.loop.size()];
    const double span = length(subtract(b, a));
    const uint32_t steps = std::max(1u, uint32_t(std::ceil(span / 30.0)));
    for (uint32_t k = 0; k < steps; ++k) out.push_back(add(a, scale(subtract(b, a), double(k) / steps)));
  }
  return out;
}

std::vector<hall> region_halls(const uint64_t seed, const int rx, const int rz) {
  // Залы — экземпляр генератора halls цепочки (правила — generator/macro/01_halls.lua). Залы не
  // зависят от радиуса прохода, поэтому годится любая цепочка; берётся цепочка радиуса 1 м.
  const auto instance = world_chain(1.0).get("halls", seed, {{uint64_t(int64_t(rx)), uint64_t(int64_t(rz)), 0}});
  return halls_from(*instance);
}

std::vector<hall> halls_from(const devils_engine::originator::generator_result& instance) {
  namespace originator = devils_engine::originator;
  const auto field = [&](const std::string_view buffer, const std::string_view name) {
    return instance.field(buffer, name);
  };
  const auto get3 = [](const originator::const_field_accessor& f, const size_t i) {
    return point{f.get(i, 0), f.get(i, 1), f.get(i, 2)};
  };
  const auto count = size_t(field("hall_counts", "halls").get(0));
  const auto key = field("halls", "key"), centre = field("halls", "centre"), forward = field("halls", "forward"),
             half = field("halls", "half"), relevance = field("halls", "relevance"),
             floor = field("halls", "floor"), kind = field("halls", "kind"),
             resident = field("halls", "resident"), body = field("halls", "body_radius"),
             first_pillar = field("halls", "first_pillar"), pillar_count = field("halls", "pillar_count"),
             first_ledge = field("halls", "first_ledge"), ledge_count = field("halls", "ledge_count"),
             first_niche = field("halls", "first_niche"), niche_count = field("halls", "niche_count"),
             first_loop = field("halls", "first_loop"), loop_count = field("halls", "loop_count");
  const auto pu = field("hall_pillars", "u"), pv = field("hall_pillars", "v"), pr = field("hall_pillars", "radius");
  const auto lc = field("hall_ledges", "centre"), lh = field("hall_ledges", "half");
  const auto nm = field("hall_niches", "mouth"), ne = field("hall_niches", "elbow"), nd = field("hall_niches", "end"),
             nr = field("hall_niches", "radius"), no = field("hall_niches", "overhang");
  const auto lp = field("hall_loop", "point");
  std::vector<hall> out(count);
  for (size_t i = 0; i < count; ++i) {
    hall& h = out[i];
    h.key = originator::read_key_word(key, i);
    h.region_x = int(int64_t(instance.key().words[0]));
    h.region_z = int(int64_t(instance.key().words[1]));
    h.kind = hall_kind(uint8_t(kind.get(i)));
    h.centre = get3(centre, i);
    h.forward = get3(forward, i);
    h.half_length = half.get(i, 0);
    h.half_width = half.get(i, 1);
    h.half_height = half.get(i, 2);
    h.relevance = get3(relevance, i);
    h.floor = floor.get(i);
    h.resident = creature_class(uint8_t(resident.get(i)));
    h.body_radius = body.get(i);
    for (size_t k = size_t(first_pillar.get(i)), e = k + size_t(pillar_count.get(i)); k < e; ++k) {
      h.pillars.push_back({pu.get(k), pv.get(k), pr.get(k)});
    }
    for (size_t k = size_t(first_ledge.get(i)), e = k + size_t(ledge_count.get(i)); k < e; ++k) {
      h.ledges.push_back({get3(lc, k), get3(lh, k)});
    }
    for (size_t k = size_t(first_niche.get(i)), e = k + size_t(niche_count.get(i)); k < e; ++k) {
      h.niches.push_back({get3(nm, k), get3(ne, k), get3(nd, k), nr.get(k), no.get(k) != 0.0});
    }
    for (size_t k = size_t(first_loop.get(i)), e = k + size_t(loop_count.get(i)); k < e; ++k) {
      h.loop.push_back(get3(lp, k));
    }
  }
  return out;
}

std::vector<hall> halls_near(const uint64_t seed, const graph::window area) {
  // Самый длинный зал с запасом на рельеф и ниши: дальше этого его поле — скала.
  constexpr double reach = 520.0;
  const auto region = [](const double metres) {
    return int(std::floor(metres / double(region_size())));
  };
  std::vector<hall> out;
  for (int rz = region(area.min_z * double(sector_size()) - reach);
       rz <= region(area.max_z * double(sector_size()) + reach); ++rz) {
    for (int rx = region(area.min_x * double(sector_size()) - reach);
         rx <= region(area.max_x * double(sector_size()) + reach); ++rx) {
      for (hall& h : region_halls(seed, rx, rz)) out.push_back(std::move(h));
    }
  }
  return out;
}

bool inside_hall(const hall& h, const point position) {
  const point local = to_local(h, position);
  const double a = local.x / h.half_length, b = local.y / h.half_height,
               c = local.z / h.half_width;
  return a * a + b * b + c * c < 1.0 && local.y > h.floor;
}

point hall_to_world(const hall& h, const point local) { return to_world(h, local); }
point hall_to_local(const hall& h, const point world) { return to_local(h, world); }

double hall_field(const uint64_t seed, const hall& h, const point position) {
  const point local = to_local(h, position);
  const double large = value_noise(seed + 3, position, relief_large_period);
  const double small = value_noise(seed + 4, position, relief_small_period);
  double air = hall_solid(h, local) + relief_large * large + relief_small * small;
  if (!h.niches.empty()) {
    // Неровность ниш двумя слоями, только наружу.
    const double grain = 0.6 * small + 0.4 * value_noise(seed + 5, position, 5.0);
    for (const hall_niche& niche : h.niches) {
      air = std::min(air, niche_distance(niche, local) - niche_relief + niche_relief * grain);
    }
  }
  const double core = loop_distance(h, local) - loop_clearance(h) - core_relief +
                      core_relief * large;
  return std::min(air, core);
}

double hall_vertical_reach(const hall& h) { return relevance_half(h).y; }

void hall_influence(const hall& h, point& hall_low, point& hall_high) {
  const point half = relevance_half(h);
  hall_low = {1e30, h.centre.y - half.y, 1e30};
  hall_high = {-1e30, h.centre.y + half.y, -1e30};
  for (const double u : {-half.x, half.x}) {
    for (const double v : {-half.z, half.z}) {
      const point p = to_world(h, u, 0, v);
      hall_low.x = std::min(hall_low.x, p.x); hall_high.x = std::max(hall_high.x, p.x);
      hall_low.z = std::min(hall_low.z, p.z); hall_high.z = std::max(hall_high.z, p.z);
    }
  }
}

bool hall_reaches(const hall& h, const point low, const point high) {
  point hall_low, hall_high;
  hall_influence(h, hall_low, hall_high);
  return boxes_overlap(low, high, hall_low, hall_high);
}

bool hall_contains_air(const hall& h, const point low, const point high) {
  // Эллипсоид и полупространство над дном выпуклы: если в них все 8 углов, то и
  // вся коробка. Запас 8 м больше рельефа 6.5 м, поэтому поле там отрицательно.
  // Коридор и ниши только добавляют воды и проверке не мешают.
  constexpr double margin = 8.0;
  static_assert(margin > relief_bound);
  const double inner = 1.0 - margin / smallest_radius(h);
  point local_low{1e30, 1e30, 1e30}, local_high{-1e30, -1e30, -1e30};
  for (int corner = 0; corner < 8; ++corner) {
    const point p{corner & 1 ? high.x : low.x, corner & 2 ? high.y : low.y,
                  corner & 4 ? high.z : low.z};
    const point local = to_local(h, p);
    const double a = local.x / h.half_length, b = local.y / h.half_height,
                 c = local.z / h.half_width;
    if (std::sqrt(a * a + b * b + c * c) > inner || local.y < h.floor + margin) return false;
    local_low = {std::min(local_low.x, local.x), std::min(local_low.y, local.y),
                 std::min(local_low.z, local.z)};
    local_high = {std::max(local_high.x, local.x), std::max(local_high.y, local.y),
                  std::max(local_high.z, local.z)};
  }
  for (const hall_pillar& pillar : h.pillars) {
    const double du = std::max({local_low.x - pillar.u, 0.0, pillar.u - local_high.x});
    const double dv = std::max({local_low.z - pillar.v, 0.0, pillar.v - local_high.z});
    if (std::hypot(du, dv) < pillar.radius * (1.0 + pillar_flare()) + margin) return false;
  }
  for (const hall_ledge& ledge : h.ledges) {
    const point ledge_low = subtract(ledge.centre, add(ledge.half, {margin, margin, margin}));
    const point ledge_high = add(ledge.centre, add(ledge.half, {margin, margin, margin}));
    if (boxes_overlap(local_low, local_high, ledge_low, ledge_high)) return false;
  }
  return true;
}

std::vector<std::pair<point, point>> hall_entrances(const uint64_t seed, const hall& h) {
  const point half = relevance_half(h);
  double min_x = 1e30, max_x = -1e30, min_z = 1e30, max_z = -1e30;
  for (const double u : {-half.x, half.x}) {
    for (const double v : {-half.z, half.z}) {
      const point p = to_world(h, u, 0, v);
      min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
      min_z = std::min(min_z, p.z); max_z = std::max(max_z, p.z);
    }
  }
  const graph route = generate_graph(seed, {int(std::floor(min_x / sector_size())) - 1,
                                            int(std::floor(min_z / sector_size())) - 1,
                                            int(std::floor(max_x / sector_size())) + 2,
                                            int(std::floor(max_z / sector_size())) + 2});
  std::vector<std::pair<point, point>> out;
  for (const edge link : route.edges) {
    const point a = route.nodes[link.a].centre, b = route.nodes[link.b].centre;
    const bool in_a = inside_hall(h, a), in_b = inside_hall(h, b);
    if (in_a != in_b) out.push_back(in_a ? std::pair{b, a} : std::pair{a, b});
  }
  return out;
}

std::string verify_hall(const uint64_t seed, const hall& h, const double clearance_radius) {
  const point half = relevance_half(h);
  if (h.centre.y - half.y < world_y_min() || h.centre.y + half.y > world_y_max()) {
    return "hall exceeds the finite world height";
  }
  // Зал вписан в маршрут: в него входят хотя бы два ребра графа снаружи. Ярус
  // стопки может не касаться слоя — тогда он входит через пересечённый с ним зал.
  if (hall_entrances(seed, h).size() < 2) {
    bool through_partner = false;
    for (const hall& other : region_halls(seed, h.region_x, h.region_z)) {
      if (other.key == h.key || through_partner) continue;
      bool shared = false;
      for (int i = 0; i <= 64 && !shared; ++i) {
        const point p = add(h.centre, scale(subtract(other.centre, h.centre), double(i) / 64.0));
        shared = inside_hall(h, p) && inside_hall(other, p);
      }
      through_partner = shared && hall_entrances(seed, other).size() >= 2;
    }
    if (!through_partner) return "hall has fewer than two route entrances";
  }

  // Только этот зал, без графа: независимо от генератора пересчитываем то, что
  // зал обещает сам по себе. Галереи и монтажные объёмы могли бы лишь добавить воды.
  const adventure context{seed, {}, clearance_radius, {h}};
  const auto all_open = [&](const std::vector<point>& points) {
    point low{1e30, 1e30, 1e30}, high{-1e30, -1e30, -1e30};
    for (const point p : points) {
      low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
      high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    const field_region region = prepare_field(context, low, high);
    return std::all_of(points.begin(), points.end(),
                       [&](const point p) { return density(region, p) < 0.0; });
  };
  std::vector<point> batch;
  // Вся область влияния зала одной подготовленной областью — для лучей видимости.
  const field_region whole = [&] {
    const point half = relevance_half(h);
    point low{1e30, h.centre.y - half.y, 1e30}, high{-1e30, h.centre.y + half.y, -1e30};
    for (const double u : {-half.x, half.x}) {
      for (const double v : {-half.z, half.z}) {
        const point p = to_world(h, u, 0, v);
        low.x = std::min(low.x, p.x); high.x = std::max(high.x, p.x);
        low.z = std::min(low.z, p.z); high.z = std::max(high.z, p.z);
      }
    }
    return prepare_field(context, low, high);
  }();
  // Петля патруля: ось и оболочка на радиусе тела плюс диагональ клетки.
  const double probe = h.body_radius + std::sqrt(3.0);
  for (size_t i = 0; i < h.loop.size(); ++i) {
    const point a = h.loop[i], b = h.loop[(i + 1) % h.loop.size()];
    const point axis = normalize(subtract(b, a));
    point side{axis.z, 0, -axis.x};
    side = length(side) < 1e-3 ? point{1, 0, 0} : normalize(side);
    const point up{axis.y * side.z - axis.z * side.y, axis.z * side.x - axis.x * side.z,
                   axis.x * side.y - axis.y * side.x};
    const double span = length(subtract(b, a));
    for (double t = 0.0; t < span; t += 3.0) {
      const point centre = add(a, scale(axis, t));
      batch.push_back(to_world(h, centre));
      for (int k = 0; k < 8; ++k) {
        const double angle = double(k) * std::numbers::pi / 4.0;
        batch.push_back(to_world(h, add(centre, add(scale(side, probe * std::cos(angle)),
                                                    scale(up, probe * std::sin(angle))))));
      }
      if (batch.size() > 400) {
        if (!all_open(batch)) return "hall creature loop is blocked";
        batch.clear();
      }
    }
  }
  if (h.loop.size() < 3) return "hall creature loop is missing";
  if (!all_open(batch)) return "hall creature loop is blocked";
  batch.clear();
  // Ниши: корпус лодки проходит по оси до дна, а дно не видно с оси коридора.
  const double hull_probe = starter_sub_beam() / 2.0 + std::sqrt(3.0);
  for (const hall_niche& niche : h.niches) {
    if (niche.radius < hull_probe + surface_guard()) return "hall niche is too narrow for the submarine";
    // Корпус идёт по входу, поворачивается в колене и уходит в отвод.
    for (const auto& [from, to] : {std::pair{niche.mouth, niche.elbow},
                                   std::pair{niche.elbow, niche.end}}) {
      const point axis = normalize(subtract(to, from));
      point side = {axis.z, 0, -axis.x};
      side = length(side) < 1e-3 ? point{1, 0, 0} : normalize(side);
      const point up{axis.y * side.z - axis.z * side.y, axis.z * side.x - axis.x * side.z,
                     axis.x * side.y - axis.y * side.x};
      const double span = length(subtract(to, from));
      for (double t = 0.0; t <= span; t += 2.0) {
        const point centre = add(from, scale(axis, t));
        batch.push_back(to_world(h, centre));
        for (const point normal : {side, scale(side, -1.0), up, scale(up, -1.0)}) {
          batch.push_back(to_world(h, add(centre, scale(normal, hull_probe))));
        }
      }
    }
    const double turn_probe = starter_sub_length() / 2.0 + std::sqrt(3.0);
    for (int z = -1; z <= 1; ++z) {
      for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
          const int squared = x * x + y * y + z * z;
          const double f = squared == 0 ? 0.0 : turn_probe / std::sqrt(double(squared));
          batch.push_back(to_world(h, add(niche.elbow, {x * f, y * f, z * f})));
        }
      }
    }
    if (!all_open(batch)) return "hall niche is too narrow for the submarine";
    batch.clear();
    const auto rock = [&](const point& local) { return density(whole, to_world(h, local)) >= 0.0; };
    if (!niche_hidden(h, niche, rock)) return "hall niche is visible from the creature loop";
  }
  // Оценки однородного чанка обязаны совпадать с полными сэмплами.
  for (const point local : {h.loop[0], h.loop[h.loop.size() / 2]}) {
    const point at = to_world(h, local.x, local.y, local.z);
    const int cx = int(std::floor(at.x / chunk::cells));
    const int cy = int(std::floor(at.y / chunk::cells));
    const int cz = int(std::floor(at.z / chunk::cells));
    const field_region region = prepare_chunk_field(context, cx, cy, cz);
    const int sign = uniform_sign(region);
    if (sign == 0) continue;
    const chunk full = generate_chunk(region, cx, cy, cz);
    for (const double sample : full.samples) {
      if ((sign < 0) != (sample < 0.0)) return "uniform chunk estimate disagrees with samples";
    }
  }
  return {};
}

namespace {

// Окно графа вокруг зала: поле зала и ещё два сектора — домен уходит от зала не
// больше чем на четыре ребра.
graph::window window_around(const hall& h, const int margin) {
  const point half = relevance_half(h);
  double min_x = 1e30, max_x = -1e30, min_z = 1e30, max_z = -1e30;
  for (const double u : {-half.x, half.x}) {
    for (const double v : {-half.z, half.z}) {
      const point p = to_world(h, u, 0, v);
      min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
      min_z = std::min(min_z, p.z); max_z = std::max(max_z, p.z);
    }
  }
  return {int(std::floor(min_x / sector_size())) - margin, int(std::floor(min_z / sector_size())) - margin,
          int(std::floor(max_x / sector_size())) + 1 + margin, int(std::floor(max_z / sector_size())) + 1 + margin};
}


// Тело-шар радиуса body проходит отрезок: в центре и на 14 направлениях на радиусе
// тела плюс диагональ клетки поле — вода. Считается по полю пещеры без монтажных
// объёмов станций: они только добавили бы воды, так что ответ «проходит» честен.
bool body_fits(const adventure& context, const point a, const point b, const double body) {
  const double probe = body + std::sqrt(3.0);
  const point low{std::min(a.x, b.x) - probe - 1.0, std::min(a.y, b.y) - probe - 1.0,
                  std::min(a.z, b.z) - probe - 1.0};
  const point high{std::max(a.x, b.x) + probe + 1.0, std::max(a.y, b.y) + probe + 1.0,
                   std::max(a.z, b.z) + probe + 1.0};
  const field_region region = prepare_field(context, low, high, false);
  const double span = length(subtract(b, a));
  const uint32_t steps = std::max(1u, uint32_t(std::ceil(span / 2.0)));
  const double diagonal = 1.0 / std::sqrt(3.0);
  for (uint32_t step = 0; step <= steps; ++step) {
    const point centre = add(a, scale(subtract(b, a), double(step) / double(steps)));
    // Шар целиком в гарантированной воде зала — та же точная оценка, что пропускает
    // однородные чанки; поле здесь можно не считать.
    const point box_low = subtract(centre, {probe, probe, probe});
    const point box_high = add(centre, {probe, probe, probe});
    if (std::any_of(context.halls.begin(), context.halls.end(),
                    [&](const hall& x) { return hall_contains_air(x, box_low, box_high); })) continue;
    if (cave_density(region, centre) >= 0.0) return false;
    for (const point direction : {point{1, 0, 0}, point{-1, 0, 0}, point{0, 1, 0}, point{0, -1, 0},
                                  point{0, 0, 1}, point{0, 0, -1},
                                  point{diagonal, diagonal, diagonal}, point{-diagonal, diagonal, diagonal},
                                  point{diagonal, -diagonal, diagonal}, point{diagonal, diagonal, -diagonal},
                                  point{-diagonal, -diagonal, diagonal}, point{-diagonal, diagonal, -diagonal},
                                  point{diagonal, -diagonal, -diagonal}, point{-diagonal, -diagonal, -diagonal}}) {
      if (cave_density(region, add(centre, scale(direction, probe))) >= 0.0) return false;
    }
  }
  return true;
}

struct creature_context {
  graph route;
  adventure field;
  std::vector<const hall*> domain_halls;
};

creature_context context_for(const uint64_t seed, const hall& h, const std::vector<hall>& near) {
  creature_context out;
  const graph::window area = window_around(h, 2);
  // Мир окна цепочки (тот же, из которого поле берёт строки галерей), залы — переданные.
  out.field = generate(seed, area, 1.5);
  out.field.halls = near;
  out.route = out.field.route;
  return out;
}

// Узлы графа, разделяющие «внутри залов домена» и «снаружи».
bool in_halls(const std::vector<const hall*>& halls, const point p) {
  return std::any_of(halls.begin(), halls.end(), [&](const hall* x) { return inside_hall(*x, p); });
}

} // namespace

creature_presence creature_of(const uint64_t seed, const hall& h) {
  // Существо — экземпляр генератора creature цепочки (creature/scripts/creature.lua). Поле прохода у
  // него — мир при радиусе 1.5 м, как у хоста раньше, поэтому и цепочка этого радиуса.
  namespace originator = devils_engine::originator;
  const auto instance = world_chain(1.5).get(
    "creature", seed, {{uint64_t(int64_t(h.region_x)), uint64_t(int64_t(h.region_z)), h.key}});
  creature_presence out;
  out.hall_key = originator::read_key_word(instance->field("creature_meta", "hall"), 0);
  out.resident = creature_class(uint8_t(instance->field("creature_meta", "resident").get(0)));
  out.body_radius = instance->field("creature_meta", "body_radius").get(0);
  const auto keys = [&](const std::string_view buffer, std::vector<uint64_t>& to) {
    const auto field = instance->field(buffer, "key");
    for (size_t i = 0; i < field.count(); ++i) to.push_back(originator::read_key_word(field, i));
  };
  keys("creature_halls", out.halls);
  keys("creature_nodes", out.nodes);
  keys("creature_refuges", out.refuges);
  const auto a = instance->field("creature_passages", "a"), b = instance->field("creature_passages", "b");
  for (size_t i = 0; i < a.count(); ++i) out.passages.push_back({originator::read_key_word(a, i), originator::read_key_word(b, i)});
  return out;
}

std::string verify_creature(const uint64_t seed, const hall& h, const creature_presence& presence) {
  if (presence.hall_key != h.key || presence.resident != h.resident ||
      presence.body_radius != h.body_radius) return "creature presence disagrees with its hall";
  if (presence.halls.empty() || presence.halls.front() != h.key || presence.halls.size() > 3) {
    return "creature domain halls are invalid";
  }
  const std::vector<hall> near = halls_near(seed, window_around(h, 2));
  const creature_context context = context_for(seed, h, near);
  const graph& route = context.route;
  const auto index_of = [&](const uint64_t key) -> int64_t {
    for (const node& at : route.nodes) if (at.key == key) return at.id;
    return -1;
  };
  std::vector<const hall*> halls;
  for (const uint64_t key : presence.halls) {
    const auto found = std::find_if(near.begin(), near.end(), [&](const hall& x) { return x.key == key; });
    if (found == near.end()) return "creature domain halls are invalid";
    halls.push_back(&*found);
  }
  std::vector<uint8_t> in_domain(route.nodes.size(), 0);
  bool invalid_node = false;
  for (const uint64_t key : presence.nodes) {
    const int64_t id = index_of(key);
    if (id < 0 || is_base(route.nodes[size_t(id)].kind)) invalid_node = true;
    else in_domain[size_t(id)] = 1;
    if (id >= 0) in_domain[size_t(id)] = 1;
  }
  // Обход: внешние концы входов вне домена, их не меньше двух, и между ними есть путь
  // по графу, не заходящий в домен. Встретив существо у входа, можно развернуться и
  // объехать. Для зверя, которого можно одолеть, обход не обязателен.
  std::vector<uint32_t> refuges;
  for (const uint64_t key : presence.refuges) {
    const int64_t id = index_of(key);
    if (id < 0 || in_domain[size_t(id)]) return "creature domain leaves no detour around the hall";
    refuges.push_back(uint32_t(id));
  }
  if (h.resident == creature_class::lord) {
    if (refuges.size() < 2) return "creature domain leaves no detour around the hall";
    std::vector<uint8_t> seen(route.nodes.size(), 0);
    std::vector<uint32_t> stack{refuges.front()};
    seen[refuges.front()] = 1;
    while (!stack.empty()) {
      const uint32_t current = stack.back();
      stack.pop_back();
      for (const edge e : route.edges) {
        if (e.a != current && e.b != current) continue;
        const uint32_t next = e.a == current ? e.b : e.a;
        if (seen[next] || in_domain[next]) continue;
        seen[next] = 1;
        stack.push_back(next);
      }
    }
    for (const uint32_t refuge : refuges) {
      if (!seen[refuge]) return "creature domain leaves no detour around the hall";
    }
  }
  if (invalid_node) return "creature domain names a missing or base node";
  // Каждый проход домена — ребро графа, и тело в нём помещается.
  std::vector<std::vector<uint32_t>> passages(route.nodes.size());
  for (const auto& [ka, kb] : presence.passages) {
    const int64_t a = index_of(ka), b = index_of(kb);
    const bool is_edge = a >= 0 && b >= 0 && std::any_of(route.edges.begin(), route.edges.end(),
      [&](const edge e) { return (e.a == a && e.b == b) || (e.a == b && e.b == a); });
    if (!is_edge || !in_domain[size_t(a)] || !in_domain[size_t(b)]) {
      return "creature domain passage is not a route edge inside the domain";
    }
    if (!body_fits(context.field, route.nodes[size_t(a)].centre, route.nodes[size_t(b)].centre,
                   h.body_radius)) return "creature domain passage is too narrow for its body";
    passages[size_t(a)].push_back(uint32_t(b));
    passages[size_t(b)].push_back(uint32_t(a));
  }
  // Домен связан проходами и ограничен: узел вне залов не дальше четырёх проходов.
  std::vector<int> depth(route.nodes.size(), -1);
  std::deque<uint32_t> queue;
  for (const node& at : route.nodes) {
    if (in_domain[at.id] && in_halls(halls, at.centre)) {
      depth[at.id] = 0;
      queue.push_back(at.id);
    }
  }
  while (!queue.empty()) {
    const uint32_t current = queue.front();
    queue.pop_front();
    for (const uint32_t next : passages[current]) {
      if (depth[next] >= 0) continue;
      depth[next] = in_halls(halls, route.nodes[next].centre) ? 0 : depth[current] + 1;
      queue.push_back(next);
    }
  }
  for (uint32_t i = 0; i < route.nodes.size(); ++i) {
    if (in_domain[i] && !in_halls(halls, route.nodes[i].centre) && (depth[i] < 0 || depth[i] > 4)) {
      return "creature domain reaches too far from its halls";
    }
  }
  return {};
}

} // namespace gn09
