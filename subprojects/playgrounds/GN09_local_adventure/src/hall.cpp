#include "adventure.h"
#include "local_structure.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>

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
// Коридор и ниши неровные только наружу: радиус в [r, r + 2 * relief].
constexpr double core_relief = 4.0;
constexpr double niche_relief = 2.5;
// Ниша — не высверленная труба: сечение вытянуто по высоте, у входа раструб.
constexpr double niche_stretch = 1.35;
constexpr double niche_flare = 0.6;
constexpr double niche_flare_reach = 0.45; // доля входной трубки с раструбом
// Фоновая скала cave_density: 3 + 0.30 broad + 0.16 detail.
constexpr double rock_background = 3.46;
constexpr double pillar_flare = 0.35;

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

double ellipse_level(const hall& h, const double u, const double v) {
  return 1.0 - (u / h.half_length) * (u / h.half_length) - (v / h.half_width) * (v / h.half_width);
}

// Твёрдая часть зала без шума: стены, дно, колонны, уступы. Отрицательно — вода.
double hall_solid(const hall& h, const point local) {
  double air = std::max(ellipsoid_distance(h, local), h.floor - local.y);
  const double flare = 1.0 + pillar_flare * (local.y / h.half_height) * (local.y / h.half_height);
  for (const hall_pillar& pillar : h.pillars) {
    air = std::max(air, pillar.radius * flare - std::hypot(local.x - pillar.u, local.z - pillar.v));
  }
  for (const hall_ledge& ledge : h.ledges) {
    air = std::max(air, -box_distance(local, ledge.centre, ledge.half));
  }
  return air;
}

double core_distance(const hall& h, const point local) {
  return segment_distance(local, {-h.core_half, h.core_y, 0}, {h.core_half, h.core_y, 0});
}

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
                   length(subtract(local, niche.elbow)) - starter_sub_turn_radius});
}

// Скала здесь при ЛЮБОМ рельефе: твёрдая часть выше рельефа с запасом, а коридор и
// ниши (их радиус может вырасти на 2 * relief) сюда не дотягиваются.
bool certainly_rock(const hall& h, const point local) {
  constexpr double margin = 0.5;
  if (hall_solid(h, local) <= relief_bound + margin) return false;
  if (core_distance(h, local) <= h.core_radius + 2.0 * core_relief + margin) return false;
  for (const hall_niche& niche : h.niches) {
    if (niche_distance(niche, local) <= 2.0 * niche_relief + margin) return false;
  }
  return true;
}

// Лодка в нише стоит у её дна; именно эту точку не должно быть видно с коридора.
point niche_target(const hall_niche& niche) {
  const point axis = normalize(subtract(niche.end, niche.elbow));
  return subtract(niche.end, scale(axis, 5.0));
}

// Каждый луч от оси коридора до укрытия упирается в скалу.
bool niche_hidden(const hall& h, const hall_niche& niche,
                  const std::function<bool(const point& local)>& rock) {
  const point target = niche_target(niche);
  const uint32_t stations = uint32_t(std::ceil(2.0 * h.core_half / 25.0));
  for (uint32_t s = 0; s <= stations; ++s) {
    const point from{-h.core_half + 2.0 * h.core_half * double(s) / double(stations), h.core_y, 0};
    const point ray = subtract(target, from);
    const double span = length(ray);
    bool blocked = false;
    for (double t = 0.0; t < span - 2.0 && !blocked; t += 1.5) {
      blocked = rock(add(from, scale(ray, t / span)));
    }
    if (!blocked) return false;
  }
  return true;
}

// Полуразмеры (в локальных осях), вне которых поле зала больше фоновой скалы.
point relevance_half(const hall& h) {
  const double grow = 1.0 + (rock_background + relief_bound) / smallest_radius(h);
  point half{h.half_length * grow, h.half_height * grow, h.half_width * grow};
  const double core_extent = h.core_radius + 2.0 * core_relief + rock_background;
  half.x = std::max(half.x, h.core_half + core_extent);
  half.y = std::max(half.y, std::abs(h.core_y) + core_extent);
  for (const hall_niche& niche : h.niches) {
    const double grow_niche = niche.radius * (1.0 + niche_flare) * niche_stretch +
                              2.0 * niche_relief + rock_background;
    const double grow_elbow = starter_sub_turn_radius + 2.0 * niche_relief + rock_background;
    for (const point p : {niche.mouth, niche.elbow, niche.end}) {
      const double extra = std::max(grow_niche, grow_elbow);
      half.x = std::max(half.x, std::abs(p.x) + extra);
      half.y = std::max(half.y, std::abs(p.y) + extra);
      half.z = std::max(half.z, std::abs(p.z) + extra);
    }
  }
  return half;
}

bool boxes_overlap(const point low_a, const point high_a, const point low_b, const point high_b) {
  return low_a.x <= high_b.x && high_a.x >= low_b.x && low_a.y <= high_b.y &&
         high_a.y >= low_b.y && low_a.z <= high_b.z && high_a.z >= low_b.z;
}

hall shape_hall(const uint64_t seed, const uint64_t key, const int rx, const int rz,
                const hall* previous, bool& stacked) {
  stacked = false;
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

  // Слой проходит через нижнюю половину зала: дно чуть ниже слоя, свод выше.
  const double region_x = (double(rx) + 0.5) * region_size;
  const double region_z = (double(rz) + 0.5) * region_size;
  const double vertical = h.half_height * (1.0 + (rock_background + relief_bound) /
                                           smallest_radius(h)) + 60.0;
  if (previous != nullptr && random01(seed, key, 8) < 0.6) {
    // Стопка: второй зал над или под первым и всегда врезается в него, так что
    // вместе они — одна двухъярусная полость с проломом между ярусами.
    stacked = true;
    h.centre.x = previous->centre.x + (random01(seed, key, 9) - 0.5) * 120.0;
    h.centre.z = previous->centre.z + (random01(seed, key, 10) - 0.5) * 120.0;
    const double gap = (previous->half_height + h.half_height) *
                       (0.6 + 0.15 * random01(seed, key, 11));
    double y = previous->centre.y + (random01(seed, key, 7) < 0.5 ? gap : -gap);
    if (y + vertical > world_y_max || y - vertical < world_y_min) {
      y = 2.0 * previous->centre.y - y;
    }
    h.centre.y = y;
  } else {
    h.centre.x = region_x + (random01(seed, key, 9) - 0.5) * 240.0;
    h.centre.z = region_z + (random01(seed, key, 10) - 0.5) * 240.0;
    h.centre.y = layer_height(seed, h.centre.x, h.centre.z) + 0.35 * h.half_height;
  }

  if (h.kind == hall_kind::pillared) {
    const uint32_t wanted = 4 + uint32_t(7.0 * random01(seed, key, 20));
    for (uint32_t attempt = 0; attempt < 48 && h.pillars.size() < wanted; ++attempt) {
      const uint64_t a = 100 + 4 * uint64_t(attempt);
      hall_pillar pillar{(random01(seed, key, a) - 0.5) * 1.5 * h.half_length,
                         (random01(seed, key, a + 1) - 0.5) * 1.6 * h.half_width,
                         5.0 + 7.0 * random01(seed, key, a + 2)};
      if (ellipse_level(h, pillar.u, pillar.v) < 0.3) continue;
      // Колонна целиком по высоте, поэтому держится в стороне от оси коридора.
      if (std::abs(pillar.v) < pillar.radius * (1.0 + pillar_flare) + h.core_radius +
                               2.0 * core_relief + 6.0) continue;
      const bool crowded = std::any_of(h.pillars.begin(), h.pillars.end(),
        [&](const hall_pillar& other) {
          return std::hypot(pillar.u - other.u, pillar.v - other.v) <
                 pillar.radius + other.radius + 18.0;
        });
      if (!crowded) h.pillars.push_back(pillar);
    }
  }
  const auto ledge_blocks_core = [&](const hall_ledge& ledge) {
    return box_distance({ledge.centre.x, h.core_y, 0}, ledge.centre, ledge.half) <
           h.core_radius + 2.0 * core_relief + 3.0;
  };
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
    if (!ledge_blocks_core(ledge)) h.ledges.push_back(ledge);
  }

  // Укрытия: ниша уходит в стену под углом к нормали, поэтому с оси коридора её дно
  // закрыто скалой. У логова их больше — там есть от кого прятаться.
  const uint32_t niches = h.kind == hall_kind::lair
    ? 4 + uint32_t(3.0 * random01(seed, key, 22))
    : 3 + uint32_t(3.0 * random01(seed, key, 22));
  const auto rock = [&](const point& local) { return certainly_rock(h, local); };
  for (uint32_t attempt = 0; attempt < 96 && h.niches.size() < niches; ++attempt) {
    const uint64_t a = 700 + 8 * uint64_t(attempt);
    const double side = random01(seed, key, a) < 0.5 ? -1.0 : 1.0;
    const double u = (random01(seed, key, a + 1) - 0.5) * 1.3 * h.half_length;
    const double radius = 7.0 + 1.5 * random01(seed, key, a + 2);
    // Нижняя треть стены: дно зала добавляет скалы между коридором и отводом.
    const double y_level = h.floor + radius * niche_stretch + 6.0 +
                           0.3 * h.half_height * random01(seed, key, a + 3);
    const double inner = 1.0 - (u / h.half_length) * (u / h.half_length) -
                         (y_level / h.half_height) * (y_level / h.half_height);
    if (inner <= 0.1) continue;
    const point wall{u, y_level, side * h.half_width * std::sqrt(inner)};
    const point normal = normalize({wall.x / (h.half_length * h.half_length),
                                    wall.y / (h.half_height * h.half_height),
                                    wall.z / (h.half_width * h.half_width)});
    point tangent{normal.z, 0, -normal.x};
    if (length(tangent) < 1e-3) continue;
    tangent = normalize(tangent);
    if (random01(seed, key, a + 4) < 0.5) tangent = scale(tangent, -1.0);
    hall_niche niche;
    niche.radius = radius;
    niche.mouth = subtract(wall, scale(normal, radius + 3.0));
    // Колено глубже рельефа стены и запаса трубки: между водой зала и отводом
    // остаётся скала при любом шуме.
    niche.elbow = add(wall, scale(normal, 32.0 + 4.0 * random01(seed, key, a + 5)));
    niche.end = add(niche.elbow, scale(tangent, 30.0 + 10.0 * random01(seed, key, a + 6)));
    // Вход в открытой воде зала, не в колонне и не в уступе.
    if (hall_solid(h, niche.mouth) > -(relief_bound + 1.0)) continue;
    const bool crowded = std::any_of(h.niches.begin(), h.niches.end(),
      [&](const hall_niche& other) {
        return length(subtract(other.end, niche.end)) < other.radius + radius + 14.0 ||
               length(subtract(other.mouth, niche.mouth)) < other.radius + radius + 10.0;
      });
    if (crowded) continue;
    h.niches.push_back(niche);
    if (!niche_hidden(h, niche, rock)) {
      h.niches.pop_back();
      continue;
    }
    // Навес над частью входов: уступ над нишей закрывает её и сверху.
    if (random01(seed, key, a + 7) < 0.5) {
      const double protrude = 14.0;
      const hall_ledge roof{{wall.x, wall.y + radius * niche_stretch + 2.0 * niche_relief + 3.0,
                             side * (std::abs(wall.z) - protrude / 2.0 + 3.0)},
                            {12.0, 2.0, protrude / 2.0 + 3.0}};
      // Ниша вырезается минимумом поверх уступа, поэтому навес ей не мешает.
      const bool clear = !ledge_blocks_core(roof);
      if (clear) {
        h.ledges.push_back(roof);
        h.niches.back().overhang = true;
      }
    }
  }
  // Каждая следующая ниша вырезает воду и могла открыть луч к уже поставленной.
  // Снимаем ставшие видимыми; снятие только возвращает скалу, поэтому оставшиеся
  // от этого видимыми не становятся. Навес снятой ниши остаётся простым уступом.
  for (size_t i = h.niches.size(); i-- > 0;) {
    if (!niche_hidden(h, h.niches[i], rock)) h.niches.erase(h.niches.begin() + ptrdiff_t(i));
  }
  return h;
}

std::vector<hall> compute_region_halls(const uint64_t seed, const int rx, const int rz) {
  const uint64_t key = devils_engine::utils::mix(seed, uint64_t(int64_t(rx)),
                                                 uint64_t(int64_t(rz)), 0x48414c4cull);
  const uint32_t count = random01(seed, key, 1) < 0.35 ? 2u : 1u;
  std::vector<hall> out;
  for (uint32_t i = 0; i < count; ++i) {
    bool stacked = false;
    out.push_back(shape_hall(seed, devils_engine::utils::mix(key, i), rx, rz,
                             out.empty() ? nullptr : &out.front(), stacked));
    if (stacked) {
      // У верхнего яруса нет плоского дна: его низ и есть пролом в нижний ярус.
      hall& upper = out.back().centre.y > out.front().centre.y ? out.back() : out.front();
      upper.floor = -upper.half_height;
    }
  }
  return out;
}

} // namespace

const char* name(const hall_kind value) {
  return value == hall_kind::lair ? "lair" : "pillared";
}

std::vector<hall> region_halls(const uint64_t seed, const int rx, const int rz) {
  // Чистая функция (seed, регион), а выбор ниш с проверкой лучами недёшев, и граф
  // спрашивает одни и те же регионы много раз. Кэш на поток: потоки viewer'а не
  // делят состояние, и результат не зависит от того, был ли он в кэше.
  struct entry {
    uint64_t seed;
    int rx, rz;
    std::vector<hall> halls;
  };
  thread_local std::vector<entry> cache;
  for (const entry& cached : cache) {
    if (cached.seed == seed && cached.rx == rx && cached.rz == rz) return cached.halls;
  }
  if (cache.size() >= 64) cache.erase(cache.begin(), cache.begin() + 32);
  cache.push_back({seed, rx, rz, compute_region_halls(seed, rx, rz)});
  return cache.back().halls;
}


std::vector<hall> halls_near(const uint64_t seed, const graph::window area) {
  // Самый длинный зал с запасом на рельеф и ниши: дальше этого его поле — скала.
  constexpr double reach = 520.0;
  const auto region = [](const double metres) {
    return int(std::floor(metres / double(region_size)));
  };
  std::vector<hall> out;
  for (int rz = region(area.min_z * double(sector_size) - reach);
       rz <= region(area.max_z * double(sector_size) + reach); ++rz) {
    for (int rx = region(area.min_x * double(sector_size) - reach);
         rx <= region(area.max_x * double(sector_size) + reach); ++rx) {
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

const hall* hall_owning_sector(const std::vector<hall>& halls, const uint64_t seed,
                               const int x, const int z) {
  const point base = base_position(seed, x, z);
  for (const hall& h : halls) {
    const point local = to_local(h, base);
    const double u = local.x / (h.half_length + 60.0), v = local.z / (h.half_width + 60.0);
    if (u * u + v * v > 1.0) continue;
    // Сектор принадлежит залу, если слой над сектором проходит сквозь его ярус.
    const double layer = layer_height(seed, base.x, base.z) - h.centre.y;
    if (layer >= h.floor - 40.0 && layer <= 0.8 * h.half_height) return &h;
  }
  return nullptr;
}

point hall_node_position(const hall& h, const uint64_t seed, const double x, const double z) {
  point local = to_local(h, {x, 0, z});
  const double inner_u = std::max(10.0, h.half_length - 45.0);
  const double inner_v = std::max(10.0, h.half_width - 45.0);
  const double level = (local.x / inner_u) * (local.x / inner_u) +
                       (local.z / inner_v) * (local.z / inner_v);
  if (level > 0.9) {
    const double shrink = std::sqrt(0.9 / level);
    local.x *= shrink;
    local.z *= shrink;
  }
  for (const hall_pillar& pillar : h.pillars) {
    const double du = local.x - pillar.u, dv = local.z - pillar.v;
    const double distance = std::hypot(du, dv);
    const double keep = pillar.radius * (1.0 + pillar_flare) + 14.0;
    if (distance >= keep) continue;
    const double ux = distance > 1e-6 ? du / distance : 0.0;
    const double uz = distance > 1e-6 ? dv / distance : 1.0;
    local.x = pillar.u + ux * keep;
    local.z = pillar.v + uz * keep;
  }
  const point world = to_world(h, local.x, 0, local.z);
  const double ceiling = h.half_height * std::sqrt(std::max(0.05, ellipse_level(h, local.x, local.z)));
  const double low = h.floor + 18.0, high = ceiling - 18.0;
  const double layer = layer_height(seed, world.x, world.z) - h.centre.y;
  local.y = low < high ? std::clamp(layer, low, high) : 0.5 * (low + high);
  return to_world(h, local);
}

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
  const double core = core_distance(h, local) - h.core_radius - core_relief +
                      core_relief * large;
  return std::min(air, core);
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
    if (std::hypot(du, dv) < pillar.radius * (1.0 + pillar_flare) + margin) return false;
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
  const graph route = generate_graph(seed, {int(std::floor(min_x / sector_size)) - 1,
                                            int(std::floor(min_z / sector_size)) - 1,
                                            int(std::floor(max_x / sector_size)) + 2,
                                            int(std::floor(max_z / sector_size)) + 2});
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
  if (h.centre.y - half.y < world_y_min || h.centre.y + half.y > world_y_max) {
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
  const adventure context{seed, {}, clearance_radius, {}, {}, {}, {h}};
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
  // Ниши: корпус лодки проходит по оси до дна, а дно не видно с оси коридора.
  const double hull_probe = starter_sub_beam / 2.0 + std::sqrt(3.0);
  for (const hall_niche& niche : h.niches) {
    if (niche.radius < hull_probe + surface_guard) return "hall niche is too narrow for the submarine";
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
    const double turn_probe = starter_sub_length / 2.0 + std::sqrt(3.0);
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
    if (!niche_hidden(h, niche, rock)) return "hall niche is visible from the creature corridor";
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
