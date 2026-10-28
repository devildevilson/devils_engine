#include "adventure.h"
#include "local_structure.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>

#include "devils_engine/utils/prng.h"

// Уровень залов. Регион 768 м решает, где в нём лежат большие залы, а обжитой граф
// секторов 96 м — где базы и галереи. Уровни соприкасаются только в горловине:
// вертикальной шахте от пустого узла графа до зала. Сами залы лежат выше или ниже
// обжитого слоя с зазором, поэтому решения баз в канонической окрестности 3x3
// сектора остаются верными, не зная о залах.

namespace gn09 {
namespace {

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const point a) { return std::sqrt(dot(a, a)); }

double random01(const uint64_t seed, const uint64_t a, const uint64_t b = 0) {
  return devils_engine::utils::prng_normalize(devils_engine::utils::mix(seed, a, b));
}

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
constexpr double throat_relief = 1.2;
// Фоновая скала cave_density: 3 + 0.30 broad + 0.16 detail.
constexpr double rock_background = 3.46;
// Зазор между залом (с рельефом) и обжитым слоем графа.
constexpr double settled_gap = 30.0;
constexpr double pillar_flare = 0.35;

point right_of(const hall& h) { return {-h.forward.z, 0, h.forward.x}; }

point to_local(const hall& h, const point p) {
  const point d = subtract(p, h.centre);
  return {dot(d, h.forward), d.y, dot(d, right_of(h))};
}

point to_world(const hall& h, const double u, const double y, const double v) {
  return add(h.centre, add(add(scale(h.forward, u), scale(right_of(h), v)), {0, y, 0}));
}

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

double ellipse_level(const hall& h, const double u, const double v) {
  return 1.0 - (u / h.half_length) * (u / h.half_length) - (v / h.half_width) * (v / h.half_width);
}

// Поле самого зала с коридором, без горловины.
double hall_volume(const uint64_t seed, const hall& h, const point position,
                   const double small_noise) {
  const point local = to_local(h, position);
  double air = std::max(ellipsoid_distance(h, local), h.floor - local.y);
  const double flare = 1.0 + pillar_flare * (local.y / h.half_height) * (local.y / h.half_height);
  for (const hall_pillar& pillar : h.pillars) {
    const double radial = std::hypot(local.x - pillar.u, local.z - pillar.v);
    air = std::max(air, pillar.radius * flare - radial);
  }
  for (const hall_ledge& ledge : h.ledges) {
    air = std::max(air, -box_distance(local, ledge.centre, ledge.half));
  }
  air += relief_large * value_noise(seed + 3, position, relief_large_period) +
         relief_small * small_noise;
  const double core = segment_distance(local, {-h.core_half, h.core_y, 0},
                                       {h.core_half, h.core_y, 0}) - h.core_radius;
  return std::min(air, core);
}

// Локальные полуразмеры, вне которых поле зала больше фоновой скалы.
point relevance_half(const hall& h) {
  const double grow = 1.0 + (rock_background + relief_bound) / smallest_radius(h);
  return {h.half_length * grow, h.half_height * grow, h.half_width * grow};
}

bool boxes_overlap(const point low_a, const point high_a, const point low_b, const point high_b) {
  return low_a.x <= high_b.x && high_a.x >= low_b.x && low_a.y <= high_b.y &&
         high_a.y >= low_b.y && low_a.z <= high_b.z && high_a.z >= low_b.z;
}

hall shape_hall(const uint64_t seed, const uint64_t key, const int rx, const int rz,
                const hall* previous) {
  hall h;
  h.key = key;
  h.region_x = rx;
  h.region_z = rz;
  h.kind = random01(seed, key, 2) < 0.4 ? hall_kind::lair : hall_kind::pillared;
  if (h.kind == hall_kind::pillared) {
    h.half_length = 160.0 + 160.0 * random01(seed, key, 3);
    h.half_width = 70.0 + 60.0 * random01(seed, key, 4);
    h.half_height = 40.0 + 40.0 * random01(seed, key, 5);
    h.core_radius = 18.0;
    h.floor = -0.72 * h.half_height;
  } else {
    h.half_length = 200.0 + 180.0 * random01(seed, key, 3);
    h.half_width = 110.0 + 70.0 * random01(seed, key, 4);
    h.half_height = 70.0 + 50.0 * random01(seed, key, 5);
    h.core_radius = 22.0;
    h.floor = -0.82 * h.half_height;
  }
  const double yaw = 2.0 * std::numbers::pi * random01(seed, key, 6);
  h.forward = {std::cos(yaw), 0, std::sin(yaw)};
  h.core_y = h.floor + h.core_radius + 0.25 * h.half_height;
  h.core_half = 0.8 * h.half_length;

  const double reach = relevance_half(h).y;
  const double below_low = world_y_min + 5.0 + reach;
  const double below_high = settled_y_min - settled_gap - reach;
  const double above_low = settled_y_max + settled_gap + reach;
  const double above_high = world_y_max - 5.0 - reach;
  const double region_x = (double(rx) + 0.5) * region_size;
  const double region_z = (double(rz) + 0.5) * region_size;
  bool above = random01(seed, key, 7) < 0.25;
  double y = 0.0;
  if (previous != nullptr && random01(seed, key, 8) < 0.6) {
    // Стопка: второй зал над или под первым, иногда с проломом между ними.
    h.centre.x = previous->centre.x + (random01(seed, key, 9) - 0.5) * 120.0;
    h.centre.z = previous->centre.z + (random01(seed, key, 10) - 0.5) * 120.0;
    const double gap = (previous->half_height + h.half_height) * (0.8 + 0.3 * random01(seed, key, 11));
    above = previous->centre.y > 0.0;
    y = previous->centre.y + (above ? gap : -gap);
    if (above ? y > above_high : y < below_low) {
      above = !above;
      y = above ? above_low + (above_high - above_low) * random01(seed, key, 12)
                : below_low + (below_high - below_low) * random01(seed, key, 12);
    }
  } else {
    h.centre.x = region_x + (random01(seed, key, 9) - 0.5) * 240.0;
    h.centre.z = region_z + (random01(seed, key, 10) - 0.5) * 240.0;
    y = above ? above_low + (above_high - above_low) * random01(seed, key, 12)
              : below_low + (below_high - below_low) * random01(seed, key, 12);
  }
  h.centre.y = y;

  if (h.kind == hall_kind::pillared) {
    const uint32_t wanted = 4 + uint32_t(7.0 * random01(seed, key, 20));
    for (uint32_t attempt = 0; attempt < 48 && h.pillars.size() < wanted; ++attempt) {
      const uint64_t a = 100 + 4 * uint64_t(attempt);
      hall_pillar pillar{(random01(seed, key, a) - 0.5) * 1.5 * h.half_length,
                         (random01(seed, key, a + 1) - 0.5) * 1.6 * h.half_width,
                         5.0 + 7.0 * random01(seed, key, a + 2)};
      if (ellipse_level(h, pillar.u, pillar.v) < 0.3) continue;
      // Колонна целиком по высоте, поэтому держится в стороне от оси коридора.
      if (std::abs(pillar.v) < pillar.radius * (1.0 + pillar_flare) + h.core_radius + 6.0) continue;
      const bool crowded = std::any_of(h.pillars.begin(), h.pillars.end(),
        [&](const hall_pillar& other) {
          return std::hypot(pillar.u - other.u, pillar.v - other.v) <
                 pillar.radius + other.radius + 18.0;
        });
      if (!crowded) h.pillars.push_back(pillar);
    }
  }
  const uint32_t ledges = h.kind == hall_kind::pillared
    ? 3 + uint32_t(3.0 * random01(seed, key, 21))
    : 1 + uint32_t(2.0 * random01(seed, key, 21));
  for (uint32_t attempt = 0; attempt < 24 && h.ledges.size() < ledges; ++attempt) {
    const uint64_t a = 400 + 5 * uint64_t(attempt);
    const double side = random01(seed, key, a) < 0.5 ? -1.0 : 1.0;
    const double u = (random01(seed, key, a + 1) - 0.5) * 1.4 * h.half_length;
    const double y_level = h.floor + 12.0 +
                           (0.5 * h.half_height - h.floor - 12.0) * random01(seed, key, a + 2);
    const double inner = 1.0 - (u / h.half_length) * (u / h.half_length) -
                         (y_level / h.half_height) * (y_level / h.half_height);
    if (inner <= 0.0) continue;
    const double wall = h.half_width * std::sqrt(inner);
    if (wall < 25.0) continue;
    const double protrude = 12.0 + 14.0 * random01(seed, key, a + 3);
    const hall_ledge ledge{{u, y_level, side * (wall - protrude / 2.0 + 3.0)},
                           {12.0 + 20.0 * random01(seed, key, a + 4),
                            1.5 + 2.0 * random01(seed, key, a + 4), protrude / 2.0 + 3.0}};
    const bool blocks_core = std::abs(ledge.centre.y - h.core_y) < h.core_radius + ledge.half.y + 3.0 &&
                             wall - protrude < h.core_radius + 3.0;
    if (!blocks_core) h.ledges.push_back(ledge);
  }
  return h;
}

double horizontal_rect_distance(const double u, const double v, const plan_rect& rect) {
  const double du = std::max({double(rect.u0) / 100.0 - u, 0.0, u - double(rect.u1) / 100.0});
  const double dv = std::max({double(rect.v0) / 100.0 - v, 0.0, v - double(rect.v1) / 100.0});
  return std::hypot(du, dv);
}

// Горловина: пустой узел обжитого графа над (или под) залом, вертикальная шахта до
// него не задевает построенные детали соседних баз и упирается в открытую воду зала.
bool place_throat(const uint64_t seed, hall& h, const double clearance_radius,
                  std::map<uint64_t, local_structure>& outposts) {
  h.throat_radius = 8.0 + 2.0 * random01(seed, h.key, 30);
  double min_x = 1e30, max_x = -1e30, min_z = 1e30, max_z = -1e30;
  for (const double u : {-h.half_length, h.half_length}) {
    for (const double v : {-h.half_width, h.half_width}) {
      const point p = to_world(h, u, 0, v);
      min_x = std::min(min_x, p.x); max_x = std::max(max_x, p.x);
      min_z = std::min(min_z, p.z); max_z = std::max(max_z, p.z);
    }
  }
  const graph::window area{int(std::floor(min_x / sector_size)) - 1,
                           int(std::floor(min_z / sector_size)) - 1,
                           int(std::floor(max_x / sector_size)) + 2,
                           int(std::floor(max_z / sector_size)) + 2};
  const graph route = generate_graph(seed, area);
  struct candidate {
    double level;
    uint64_t key;
    uint32_t id;
  };
  std::vector<candidate> candidates;
  for (const node& at : route.nodes) {
    if (at.kind != theme::empty) continue;
    // Узел у края окна графа может зависеть от обрезки окна; берём только внутренние.
    const int sx = int(std::floor(at.centre.x / sector_size));
    const int sz = int(std::floor(at.centre.z / sector_size));
    if (sx <= area.min_x || sx >= area.max_x - 1 || sz <= area.min_z || sz >= area.max_z - 1) continue;
    const point local = to_local(h, at.centre);
    const double level = ellipse_level(h, local.x, local.z);
    if (level < 0.55) continue;
    const bool on_pillar = std::any_of(h.pillars.begin(), h.pillars.end(),
      [&](const hall_pillar& pillar) {
        return std::hypot(local.x - pillar.u, local.z - pillar.v) <
               pillar.radius * (1.0 + pillar_flare) + h.throat_radius + 6.0;
      });
    if (!on_pillar) candidates.push_back({-level, at.key, at.id});
  }
  std::sort(candidates.begin(), candidates.end(), [](const candidate& a, const candidate& b) {
    return a.level != b.level ? a.level < b.level : a.key < b.key;
  });
  const bool below = h.centre.y < 0.0;
  const double turn_probe = starter_sub_turn_radius + std::sqrt(3.0) + 1.0;
  for (size_t i = 0; i < candidates.size() && i < 16; ++i) {
    const node& at = route.nodes[candidates[i].id];
    const point local = to_local(h, at.centre);
    const double level = ellipse_level(h, local.x, local.z);
    const double end_y = below ? h.half_height * std::sqrt(level) - 22.0 : h.floor + 22.0;
    // X/Z берутся у самого узла: перевод в локальные координаты и обратно не точен.
    const point end{at.centre.x, h.centre.y + end_y, at.centre.z};
    bool open = true;
    for (int z = -1; z <= 1 && open; ++z) {
      for (int y = -1; y <= 1 && open; ++y) {
        for (int x = -1; x <= 1 && open; ++x) {
          const int squared = x * x + y * y + z * z;
          const double f = squared == 0 ? 0.0 : turn_probe / std::sqrt(double(squared));
          const point p = add(end, {x * f, y * f, z * f});
          open = hall_volume(seed, h, p,
                             value_noise(seed + 4, p, relief_small_period)) < -0.5;
        }
      }
    }
    if (!open) continue;
    const double keep_out = h.throat_radius + throat_relief + 8.0;
    bool clear = true;
    for (const node& base : route.nodes) {
      if (!is_base(base.kind) || !clear) continue;
      if (std::hypot(base.centre.x - at.centre.x, base.centre.z - at.centre.z) > 110.0) continue;
      auto found = outposts.find(base.key);
      if (found == outposts.end()) {
        found = outposts.emplace(base.key, canonical_outpost(seed, base, clearance_radius)).first;
      }
      const local_structure& structure = found->second;
      const point d = subtract(at.centre, structure.centre);
      const point right{-structure.forward.z, 0, structure.forward.x};
      const double u = dot(d, structure.forward), v = dot(d, right);
      for (const built_solid& solid : materialize_outpost_solids(structure)) {
        if (horizontal_rect_distance(u, v, solid.footprint) < keep_out) {
          clear = false;
          break;
        }
      }
    }
    if (!clear) continue;
    h.throat_node = at.key;
    h.throat_top = at.centre;
    h.throat_end = end;
    return true;
  }
  return false;
}

} // namespace

const char* name(const hall_kind value) {
  return value == hall_kind::lair ? "lair" : "pillared";
}

std::vector<hall> region_halls(const uint64_t seed, const int rx, const int rz,
                               const double clearance_radius, uint32_t* sealed) {
  const uint64_t key = devils_engine::utils::mix(seed, uint64_t(int64_t(rx)),
                                                 uint64_t(int64_t(rz)), 0x48414c4cull);
  const uint32_t count = random01(seed, key, 1) < 0.35 ? 2u : 1u;
  std::vector<hall> out;
  std::map<uint64_t, local_structure> outposts;
  std::optional<hall> first;
  for (uint32_t i = 0; i < count; ++i) {
    hall h = shape_hall(seed, devils_engine::utils::mix(key, i), rx, rz,
                        first ? &*first : nullptr);
    if (!first) first = h;
    // Зал без безопасной горловины не строится: он был бы недоступной полостью.
    if (place_throat(seed, h, clearance_radius, outposts)) out.push_back(std::move(h));
    else if (sealed != nullptr) ++*sealed;
  }
  return out;
}

double hall_field(const uint64_t seed, const hall& h, const point position) {
  const double small_noise = value_noise(seed + 4, position, relief_small_period);
  const double volume = hall_volume(seed, h, position, small_noise);
  const double throat = segment_distance(position, h.throat_top, h.throat_end) -
                        h.throat_radius + throat_relief * small_noise;
  return std::min(volume, throat);
}

double hall_vertical_reach(const hall& h) { return relevance_half(h).y; }

bool hall_reaches(const hall& h, const point low, const point high) {
  const point half = relevance_half(h);
  point hall_low{1e30, h.centre.y - half.y, 1e30};
  point hall_high{-1e30, h.centre.y + half.y, -1e30};
  for (const double u : {-half.x, half.x}) {
    for (const double v : {-half.z, half.z}) {
      const point p = to_world(h, u, 0, v);
      hall_low.x = std::min(hall_low.x, p.x); hall_high.x = std::max(hall_high.x, p.x);
      hall_low.z = std::min(hall_low.z, p.z); hall_high.z = std::max(hall_high.z, p.z);
    }
  }
  if (boxes_overlap(low, high, hall_low, hall_high)) return true;
  const double grow = h.throat_radius + throat_relief + rock_background + 1.0;
  const point throat_low{std::min(h.throat_top.x, h.throat_end.x) - grow,
                         std::min(h.throat_top.y, h.throat_end.y) - grow,
                         std::min(h.throat_top.z, h.throat_end.z) - grow};
  const point throat_high{std::max(h.throat_top.x, h.throat_end.x) + grow,
                          std::max(h.throat_top.y, h.throat_end.y) + grow,
                          std::max(h.throat_top.z, h.throat_end.z) + grow};
  return boxes_overlap(low, high, throat_low, throat_high);
}

bool hall_contains_air(const hall& h, const point low, const point high) {
  // Эллипсоид и полупространство над дном выпуклы: если в них все 8 углов, то и
  // вся коробка. Запас 8 м больше рельефа 6.5 м, поэтому поле там отрицательно.
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
    if (std::hypot(du, dv) < pillar.radius * (1.0 + pillar_flare) + margin) return false;
  }
  for (const hall_ledge& ledge : h.ledges) {
    const point ledge_low = subtract(ledge.centre, add(ledge.half, {margin, margin, margin}));
    const point ledge_high = add(ledge.centre, add(ledge.half, {margin, margin, margin}));
    if (boxes_overlap(local_low, local_high, ledge_low, ledge_high)) return false;
  }
  return true;
}

std::string verify_hall(const uint64_t seed, const hall& h, const double clearance_radius) {
  // Объём с рельефом может быть водой лишь там, где ellipsoid < relief_bound;
  // снаружи оценка расстояния не меньше (k0 - 1) * min_radius.
  const double air_reach = h.half_height * (1.0 + relief_bound / smallest_radius(h));
  const bool below = h.centre.y < 0.0;
  if (below ? h.centre.y + air_reach > settled_y_min - 1.0
            : h.centre.y - air_reach < settled_y_max + 1.0) {
    return "hall cuts into the settled layer";
  }
  if (h.centre.y - air_reach < world_y_min || h.centre.y + air_reach > world_y_max) {
    return "hall exceeds the finite world height";
  }
  adventure context = canonical_context(seed, h.throat_top, clearance_radius);
  const auto top = std::find_if(context.route.nodes.begin(), context.route.nodes.end(),
    [&](const node& at) { return at.key == h.throat_node; });
  if (top == context.route.nodes.end() || top->kind != theme::empty ||
      top->centre.x != h.throat_top.x || top->centre.y != h.throat_top.y ||
      top->centre.z != h.throat_top.z) {
    return "hall throat does not start at a settled graph node";
  }
  if (h.throat_top.x != h.throat_end.x || h.throat_top.z != h.throat_end.z) {
    return "hall throat is not a vertical shaft";
  }
  // Шахта не проходит сквозь построенные детали баз (их планы — канонические).
  for (const node& base : context.route.nodes) {
    if (!is_base(base.kind)) continue;
    if (std::hypot(base.centre.x - h.throat_top.x, base.centre.z - h.throat_top.z) > 110.0) continue;
    const local_structure structure = canonical_outpost(seed, base, clearance_radius);
    const point d = subtract(h.throat_top, structure.centre);
    const point right{-structure.forward.z, 0, structure.forward.x};
    for (const built_solid& solid : materialize_outpost_solids(structure)) {
      if (horizontal_rect_distance(dot(d, structure.forward), dot(d, right), solid.footprint) <
          h.throat_radius + throat_relief) return "hall throat cuts through an outpost";
    }
  }
  // Без планов баз окрестности: монтажный объём только добавил бы воды, так что
  // проверка без него строже настоящего мира.
  context.halls = {h};
  // Точки проверяются пачками в одной подготовленной области, накрывающей пачку:
  // внутри области поле то же, что поточечное.
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
  const double hull_probe = starter_sub_beam / 2.0 + std::sqrt(3.0);
  const double turn_probe = starter_sub_length / 2.0 + std::sqrt(3.0);
  const point side_a{1, 0, 0}, side_b{0, 0, 1};
  const double shaft = length(subtract(h.throat_end, h.throat_top));
  const uint32_t steps = std::max(1u, uint32_t(std::ceil(shaft / 2.0)));
  std::vector<point> batch;
  for (uint32_t step = 0; step <= steps; ++step) {
    const point centre = add(h.throat_top, scale(subtract(h.throat_end, h.throat_top),
                                                 double(step) / double(steps)));
    for (const point normal : {side_a, scale(side_a, -1.0), side_b, scale(side_b, -1.0)}) {
      for (const double station : {-starter_sub_axis_half, 0.0, starter_sub_axis_half}) {
        batch.push_back(add(add(centre, {0, station, 0}), scale(normal, hull_probe)));
      }
    }
    if (step % 16 == 15 || step == steps) {
      if (!all_open(batch)) return "hall throat is too narrow for the submarine";
      batch.clear();
    }
  }
  for (int z = -1; z <= 1; ++z) {
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        const int squared = x * x + y * y + z * z;
        const double f = squared == 0 ? 0.0 : turn_probe / std::sqrt(double(squared));
        batch.push_back(add(h.throat_end, {x * f, y * f, z * f}));
      }
    }
  }
  if (!all_open(batch)) return "hall throat does not open into the hall";
  batch.clear();
  // Коридор существ: ось и оболочка на радиусе с охранным слоем сетки.
  const double probe = h.core_radius - surface_guard;
  const uint32_t core_steps = uint32_t(std::ceil(2.0 * h.core_half / 4.0));
  for (uint32_t step = 0; step <= core_steps; ++step) {
    const double u = -h.core_half + 2.0 * h.core_half * double(step) / double(core_steps);
    for (int k = 0; k <= 8; ++k) {
      const double angle = double(k) * std::numbers::pi / 4.0;
      const double r = k == 8 ? 0.0 : probe;
      batch.push_back(to_world(h, u, h.core_y + r * std::sin(angle), r * std::cos(angle)));
    }
    if (step % 8 == 7 || step == core_steps) {
      if (!all_open(batch)) return "hall creature corridor is blocked";
      batch.clear();
    }
  }
  // Оценки однородного чанка обязаны совпадать с полными сэмплами.
  for (const point local : {point{0, h.core_y, 0},
                            point{0.5 * h.core_half, h.core_y + 0.2 * h.half_height, 0}}) {
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

} // namespace gn09
