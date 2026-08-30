#include "world_build.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
#include <cstdio>
#include <cmath>
#include <format>
#include <map>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include "devils_engine/utils/hash.h"

namespace devils_engine::pf09 {

namespace {

// Короче этого общий отрезок проходом не считается. Порог задан НЕ вкусом, а представлением: вершины
// хранятся в мировых `float`, и шаг между соседними представимыми числами растёт вместе с координатой.
// Зонируется ОКРЕСТНОСТЬ ПАРТИИ, а не планета — дальние города живут фоновой симуляцией и зон не имеют,
// — поэтому область держится у начала координат, где на пятидесяти километрах шаг `float` равен четырём
// миллиметрам, а не трём сантиметрам, как было на полумиллионе. Допуск ушёл вслед за ним в пять раз.
constexpr float edge_epsilon = 0.02f;
constexpr float collinear_epsilon = 0.03f; // отклонение конца ребра от прямой соседа в узкой фазе
constexpr double wall_inset_m = 1.2;  // насколько стены здания отступают от границы участка
constexpr double door_width_m = 2.4;
constexpr uint32_t plot_stride = 16;     // сколько личностей резервируется под один участок застройки

// Черновая зона до раздачи ключей. Списка соседей здесь НЕТ: связность выводится из геометрии позже, и
// это главное отличие новой модели — проход это общее ребро, а не строка в списке.
struct draft {
  std::vector<std::vector<glm::vec2>> parts; // выпуклые части одной зоны
  float low = 0.0f;
  float high = 0.0f;
  zone_level level = zone_level::interior;
  zone_kind kind = zone_kind::hall;
  uint32_t tags = 0;
  std::string name;
  uint64_t identity = 0;
  uint64_t parent_identity = 0;
  int32_t floor = 0;

  // Явные рёбра графа. Нужны там, где общего ребра НЕТ и быть не может: дорога между поселениями и
  // лестница на другой этаж. Лестница — самый честный случай: её верх и низ лежат ровно друг над другом,
  // общее ребро у них в плане идеальное, а прохода через него нет, потому что между ними перекрытие.
  // Поэтому связность и не может быть чистой функцией геометрии — её кладут в файл.
  struct graph_link {
    uint64_t identity = 0;
    uint32_t flags = 0;
  };
  std::vector<graph_link> graph_links;
  // Части хранятся В ЛОКАЛЬНЫХ координатах относительно `reference`, а мировые получаются сложением при
  // записи. Это не экономия и не удобство: точка, посчитанная сразу в мировых метрах, приходит уже
  // округлённой до трёх сантиметров, и короткое ребро двери длиной в два метра даёт из таких точек
  // направление с ошибкой `0.025` — на порядки больше кванта ключа прямой. Двери переставали соединяться
  // с комнатами, и это было видно как «из комнаты достижимы только комнаты».
  glm::vec2 reference{};
  int32_t sector_x = 0;
  int32_t sector_y = 0;
  // Сектор задан ЯВНО и не выводится из габарита. Здание — одна вещь, и его комнаты, стены, двери и
  // лестница обязаны лежать в одном файле: иначе центр габарита разводит их по разным секторам, лестница
  // с её верхним залом оказываются в разных, и связь между ними то разрешается, то нет — в зависимости
  // от того, что сейчас подгружено.
  bool anchored = false;
};

std::vector<glm::vec2> rectangle(const double x0, const double y0, const double x1, const double y1) {
  return {{float(x0), float(y0)}, {float(x1), float(y0)}, {float(x1), float(y1)}, {float(x0), float(y1)}};
}

zone_bounds bounds_of(const std::span<const glm::vec2> outline, const float low, const float high) {
  zone_bounds out{{1e30f, low, 1e30f}, {-1e30f, high, -1e30f}};
  for (const auto& point : outline) {
    out.lower.x = std::min(out.lower.x, point.x);
    out.lower.z = std::min(out.lower.z, point.y);
    out.upper.x = std::max(out.upper.x, point.x);
    out.upper.z = std::max(out.upper.z, point.y);
  }
  return out;
}

zone_bounds merge_bounds(const zone_bounds& a, const zone_bounds& b) {
  return {{std::min(a.lower.x, b.lower.x), std::min(a.lower.y, b.lower.y), std::min(a.lower.z, b.lower.z)},
          {std::max(a.upper.x, b.upper.x), std::max(a.upper.y, b.upper.y), std::max(a.upper.z, b.upper.z)}};
}

// Билинейная точка внутри четырёхугольника. Углы клеток смещены хешем, поэтому клетка — неправильный
// четырёхугольник; комнаты и двери внутри неё строятся В ПАРАМЕТРАХ этой клетки, а не в мировых метрах.
// Так соседние подфигуры получают буквально одни и те же вершины, и общее ребро остаётся точным.
glm::vec2 bilinear(const std::array<glm::vec2, 4>& quad, const double u, const double v) {
  // На краях параметра берём вершины и рёбра ТОЧНО, а не через интерполяцию. `mix(a, b, 1)` возвращает
  // `a + (b - a)`, что отличается от `b` на округление: при координатах в полмиллиона метров это три
  // сантиметра, и между стеной здания и соседней улицей открывалась щель такой ширины. Точка выборки в
  // неё проваливалась, и покрытие переставало быть полным ровно в одном месте из четырёх тысяч.
  const auto edge = [](const glm::vec2 a, const glm::vec2 b, const double t) {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    return glm::mix(a, b, float(t));
  };

  const auto bottom = edge(quad[0], quad[1], u);
  const auto top = edge(quad[3], quad[2], u);
  return edge(bottom, top, v);
}

uint64_t identity_of(const uint32_t domain, const uint64_t a, const uint64_t b = 0) {
  return (uint64_t(domain) << 56) | ((a & 0xffffffffull) << 24) | (b & 0xffffffull);
}

// Ребро, приведённое к своей прямой. Прямая — ключ корзины: делить отрезок могут только рёбра на одной
// прямой, и сравнивать имеет смысл лишь внутри корзины. Раньше ключом была координата оси, и это молча
// требовало, чтобы все фигуры были прямоугольниками по осям. Теперь ключ — сама прямая: направление,
// приведённое к каноническому знаку, и её смещение от начала координат.
struct edge_ref {
  uint32_t item = 0;      // индекс части в общем списке
  float begin = 0.0f;     // проекция концов на направление прямой
  float end = 0.0f;
  glm::vec2 origin{};     // концы В ЛОКАЛЬНЫХ координатах: узкой фазе нужна геометрия, а не проекции,
  glm::vec2 tail{};       // и она страдает от той же потери точности, что и ключ
  glm::vec2 direction{};
  glm::vec2 reference{};
};

struct line_key {
  int64_t dx = 0;
  int64_t dy = 0;
  int64_t offset = 0;
  int64_t place = 0;   // квантованное общее начало: рёбра разных поселений не смешиваются

  bool operator<(const line_key& other) const noexcept {
    if (place != other.place) return place < other.place;
    if (dx != other.dx) return dx < other.dx;
    if (dy != other.dy) return dy < other.dy;
    return offset < other.offset;
  }
};

int64_t quantise(const double value, const double scale) { return int64_t(std::llround(value * scale)); }

// Каноническая прямая ребра. Знак направления фиксируется, иначе одно и то же ребро, обойдённое с разных
// сторон, попало бы в две разные корзины и проход бы не нашёлся.
// Ключ считается в координатах ОТНОСИТЕЛЬНО общего начала, и это не косметика. Направление короткого
// ребра, посчитанное из абсолютных координат в полмиллиона метров, имеет ошибку около `2e-3`; смещение
// прямой умножает её на саму координату и получает сотни метров. Ребро и его подотрезок оказывались в
// разных корзинах, и проход «дверь — комната» не находился: их было `140` вместо тысяч, при том что
// «комната — комната» работало, потому что там концы рёбер совпадают побитно.
//
// Общее начало берётся из зоны и одинаково у всех фигур одного поселения. Рёбер, общих у двух разных
// поселений, не бывает, поэтому начало входит в ключ и заодно разделяет их.
bool line_of(const glm::vec2 a, const glm::vec2 b, line_key& key, float& begin, float& end, glm::vec2& origin,
             glm::vec2& direction) {
  auto delta = b - a;
  const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
  if (length < 1.0e-5f) return false;

  delta /= length;
  if (delta.x < -1.0e-6f || (std::abs(delta.x) <= 1.0e-6f && delta.y < 0.0f)) delta = -delta;

  // Смещение прямой квантуется ГРУБО, и это не небрежность. Координаты доходят до полумиллиона метров,
  // где шаг `float` уже около трёх сантиметров; посчитанное через направление смещение расходится у двух
  // отрезков одной прямой на десятые доли метра. Тонкий ключ разносил их по разным корзинам, и общего
  // ребра «не находилось». Ключ теперь только группирует кандидатов, а решает узкая фаза.
  const double cross = double(delta.x) * double(a.y) - double(delta.y) * double(a.x);
  key = {quantise(delta.x, 256.0), quantise(delta.y, 256.0), quantise(cross, 8.0)};

  origin = a;
  direction = delta;
  begin = delta.x * a.x + delta.y * a.y;   // проекции тоже локальные, иначе тот же дрейф
  end = delta.x * b.x + delta.y * b.y;
  if (begin > end) std::swap(begin, end);
  return true;
}

} // namespace

build_stats build_world(const territory& map, const locality_config& local, const build_options& options) {
  const auto begin_time = std::chrono::steady_clock::now();

  const double low_x = double(options.sector_x) * sector_span_m;
  const double low_y = double(options.sector_y) * sector_span_m;
  const double high_x = low_x + double(options.sector_side) * sector_span_m;
  const double high_y = low_y + double(options.sector_side) * sector_span_m;

  const auto inside = [&](const glm::dvec2& point) {
    return point.x >= low_x && point.x < high_x && point.y >= low_y && point.y < high_y;
  };

  std::vector<draft> drafts;

  // --- крупные уровни: узлы графа без формы ---
  //
  // На политическом уровне достаточно графа между владениями. Форма там всё равно не квадратная, а на
  // вопрос «кому принадлежит эта земля» отвечает разбиение из `territory`, которое стоит 244 КБ на весь
  // мир. Поэтому зона здесь АБСТРАКТНАЯ: узел без многоугольника, связанный явными рёбрами.
  struct placed_node {
    zone_id node = invalid_zone;
    glm::dvec2 centre{};
  };

  const auto gather = [&](const tier value) {
    std::vector<placed_node> out;
    for (uint64_t index = 0; index < map.cell_count(value); ++index) {
      const auto id = make_zone(value, uint32_t(index));
      const auto* node = map.find_authored(id);
      if (node == nullptr || node->id != id) continue;

      const auto centre = map.node_centre_m(id);
      if (!inside(centre)) continue;
      out.push_back({id, centre});
    }
    return out;
  };

  const auto baronies = gather(tier::barony);
  const auto districts = gather(tier::district);

  const auto emit_graph = [&](const std::vector<placed_node>& nodes, const double reach, const zone_level level,
                              const zone_kind kind, const uint32_t domain, const auto& parent_identity) {
    for (const auto& item : nodes) {
      draft entry{};
      entry.level = level;
      entry.kind = kind;
      entry.identity = identity_of(domain, item.node);
      entry.parent_identity = parent_identity(item.node);
      entry.name = std::format("{} {}", zone_kind_name(kind), index_of(item.node));
      entry.sector_x = sector_of(item.centre.x);
      entry.sector_y = sector_of(item.centre.y);

      for (const auto& other : nodes) {
        if (other.node == item.node) continue;
        const double dx = other.centre.x - item.centre.x;
        const double dy = other.centre.y - item.centre.y;
        if (dx * dx + dy * dy > reach * reach) continue;
        entry.graph_links.push_back({identity_of(domain, other.node), uint32_t(portal_flags::graph)});
      }
      drafts.push_back(std::move(entry));
    }
  };

  emit_graph(baronies, map.tier_span_m(tier::barony) * 1.6, zone_level::political, zone_kind::holding, 1,
             [](const zone_id) { return uint64_t(0); });
  emit_graph(districts, map.tier_span_m(tier::district) * 1.6, zone_level::regional, zone_kind::market, 2,
             [&](const zone_id node) { return identity_of(1, map.ancestor_at(node, tier::barony)); });

  // --- поселения: фигура на карте, дороги между ними — рёбра без геометрии ---

  locality_field field(map, local);
  std::vector<placed_node> settlements;

  const double locale_span = map.tier_span_m(tier::locale);
  for (int64_t y = int64_t(low_y / locale_span) - 1; y <= int64_t(high_y / locale_span) + 1; ++y) {
    for (int64_t x = int64_t(low_x / locale_span) - 1; x <= int64_t(high_x / locale_span) + 1; ++x) {
      const glm::dvec2 probe{(double(x) + 0.5) * locale_span, (double(y) + 0.5) * locale_span};
      const auto node = map.resolve(probe, tier::locale);
      if (field.placed_at(node) == locality_kind::none) continue;

      const auto centre = map.node_centre_m(node);
      if (!inside(centre)) continue;

      const bool collides = std::any_of(settlements.begin(), settlements.end(), [&](const placed_node& item) {
        return item.node == node || (std::abs(item.centre.x - centre.x) < local.extent_m &&
                                     std::abs(item.centre.y - centre.y) < local.extent_m);
      });
      if (collides) continue;
      settlements.push_back({node, centre});
    }
  }

  const auto emit_shape = [&](draft entry) {
    if (!entry.anchored) {
      zone_bounds box{{1e30f, entry.low, 1e30f}, {-1e30f, entry.high, -1e30f}};
      for (const auto& part : entry.parts) {
        box = merge_bounds(box, bounds_of(part, entry.low, entry.high));
      }
      entry.sector_x = sector_of(double(box.lower.x + entry.reference.x + box.upper.x + entry.reference.x) * 0.5);
      entry.sector_y = sector_of(double(box.lower.z + entry.reference.y + box.upper.z + entry.reference.y) * 0.5);
    }
    drafts.push_back(std::move(entry));
  };

  for (const auto& item : settlements) {
    const double half = local.extent_m * 0.5;
    draft entry{};
    // Локально, как и всё остальное: мировые координаты добавит запись.
    entry.parts.push_back(rectangle(-half, -half, half, half));
    entry.high = 40.0f;
    entry.level = zone_level::local;
    entry.kind = zone_kind::settlement;
    entry.identity = identity_of(3, item.node);
    entry.parent_identity = identity_of(2, map.ancestor_at(item.node, tier::district));
    entry.name = std::format("settlement {}", index_of(item.node));
    entry.reference = glm::vec2{float(item.centre.x), float(item.centre.y)};

    for (const auto& other : settlements) {
      if (other.node == item.node) continue;
      const double dx = other.centre.x - item.centre.x;
      const double dy = other.centre.y - item.centre.y;
      if (dx * dx + dy * dy > 12000.0 * 12000.0) continue;
      entry.graph_links.push_back({identity_of(3, other.node), uint32_t(portal_flags::graph)});
    }
    emit_shape(std::move(entry));
  }

  // --- внутренний уровень: места из выпуклых частей ---
  //
  // Выпуклая фигура мельче осмысленного места. Бар — несколько помещений внутри здания, площадь —
  // несколько уличных клеток, задний двор — пара соседних. Поэтому здесь клетки СОБИРАЮТСЯ в места, и
  // зоной становится место, а клетка — его частью. Иерархия строится над местами: место → квартал →
  // поселение, и именно на неё опирается вопрос вроде «кто держит этот район».
  //
  // Стены — тоже места, просто непроходимые. Игровая поверхность покрыта целиком: между комнатой и улицей
  // нет пустоты, там стоит стена, и у неё есть рёбра, соседи и владелец.

  const uint32_t rooms_per_plot = local.room_side * local.room_side;
  const uint32_t stride = rooms_per_plot + 1;
  const double inset_u = wall_inset_m / (local.extent_m / double(local.plot_side));
  const double door_u = door_width_m / (local.extent_m / double(local.plot_side));
  const uint32_t side = local.plot_side;

  for (const auto& item : settlements) {
    locality place(map, local, item.node, field.placed_at(item.node));
    const double half = local.extent_m * 0.5;
    const double cell = local.extent_m / double(side);
    const auto settlement_identity = identity_of(3, item.node);
    const glm::vec2 reference{float(item.centre.x), float(item.centre.y)};

    const auto corner = [&](const int64_t cx, const int64_t cy) {
      const glm::dvec2 base{-half + double(cx) * cell, -half + double(cy) * cell};
      const bool edge = cx == 0 || cy == 0 || cx == int64_t(side) || cy == int64_t(side);
      if (edge) return glm::vec2{float(base.x), float(base.y)};

      const auto h = utils::splitmix(uint64_t(cx) * 73856093ull ^ uint64_t(cy) * 19349663ull,
                                     uint64_t(item.node) + 1ull);
      const double ox = (double(h & 0xffffull) / 65535.0 - 0.5) * cell * 0.26;
      const double oy = (double((h >> 20) & 0xffffull) / 65535.0 - 0.5) * cell * 0.26;
      return glm::vec2{float(base.x + ox), float(base.y + oy)};
    };
    const auto cell_quad = [&](const uint32_t px, const uint32_t py) {
      return std::array<glm::vec2, 4>{corner(px, py), corner(px + 1, py), corner(px + 1, py + 1),
                                      corner(px, py + 1)};
    };
    const auto sub_quad = [&](const std::array<glm::vec2, 4>& quad, const double u0, const double v0,
                              const double u1, const double v1) {
      return std::vector<glm::vec2>{bilinear(quad, u0, v0), bilinear(quad, u1, v0), bilinear(quad, u1, v1),
                                    bilinear(quad, u0, v1)};
    };
    const auto role_at = [&](const uint32_t px, const uint32_t py) {
      return place.role((py * side + px) * stride);
    };

    // Кварталы: сетка мест делится на четверти. Квартал абстрактен — у него нет своей формы, он лишь
    // отвечает на вопрос «где это» и служит тем, чем владеют и что держат.
    const uint32_t district_side = 2;
    for (uint32_t dy = 0; dy < district_side; ++dy) {
      for (uint32_t dx = 0; dx < district_side; ++dx) {
        draft entry{};
        entry.kind = zone_kind::district;
        entry.identity = identity_of(5, item.node, dy * district_side + dx);
        entry.parent_identity = settlement_identity;
        entry.name = std::format("district {},{}", dx, dy);
        entry.reference = reference;
        entry.sector_x = sector_of(double(reference.x));
        entry.sector_y = sector_of(double(reference.y));
        drafts.push_back(std::move(entry));
      }
    }
    const auto district_of = [&](const uint32_t px, const uint32_t py) {
      const uint32_t dx = std::min(px * district_side / side, district_side - 1);
      const uint32_t dy = std::min(py * district_side / side, district_side - 1);
      return identity_of(5, item.node, dy * district_side + dx);
    };

    // Клетки собираются в места волной по одинаковой роли. Улицы дополнительно режутся перекрёстками:
    // отрезок улицы между перекрёстками — самостоятельное место, а перекрёсток — своё.
    const auto crossroad_at = [&](const uint32_t px, const uint32_t py) {
      if (role_at(px, py) != zone_role::street) return false;
      const bool horizontal = (px > 0 && role_at(px - 1, py) == zone_role::street) ||
                              (px + 1 < side && role_at(px + 1, py) == zone_role::street);
      const bool vertical = (py > 0 && role_at(px, py - 1) == zone_role::street) ||
                            (py + 1 < side && role_at(px, py + 1) == zone_role::street);
      return horizontal && vertical;
    };

    std::vector<uint32_t> group(size_t(side) * side, 0xffffffffu);
    std::vector<std::vector<uint32_t>> groups;
    std::vector<zone_kind> group_kind;

    const auto flood = [&](const uint32_t seed, const zone_kind kind, const bool cross_wave) {
      const uint32_t id = uint32_t(groups.size());
      groups.emplace_back();
      group_kind.push_back(kind);

      std::vector<uint32_t> queue{seed};
      group[seed] = id;
      for (size_t head = 0; head < queue.size(); ++head) {
        const uint32_t plot = queue[head];
        groups[id].push_back(plot);
        if (!cross_wave) continue;

        const uint32_t px = plot % side;
        const uint32_t py = plot / side;
        constexpr int32_t offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& offset : offsets) {
          const int64_t nx = int64_t(px) + offset[0];
          const int64_t ny = int64_t(py) + offset[1];
          if (nx < 0 || ny < 0 || nx >= int64_t(side) || ny >= int64_t(side)) continue;

          const uint32_t next = uint32_t(ny) * side + uint32_t(nx);
          if (group[next] != 0xffffffffu) continue;
          if (role_at(uint32_t(nx), uint32_t(ny)) != role_at(px, py)) continue;
          if (kind == zone_kind::street && crossroad_at(uint32_t(nx), uint32_t(ny))) continue;
          if (kind == zone_kind::street && district_of(uint32_t(nx), uint32_t(ny)) != district_of(px, py)) continue;

          group[next] = id;
          queue.push_back(next);
        }
      }
    };

    for (uint32_t plot = 0; plot < side * side; ++plot) {
      if (group[plot] != 0xffffffffu) continue;
      const uint32_t px = plot % side;
      const uint32_t py = plot / side;

      switch (role_at(px, py)) {
        case zone_role::street:
          if (crossroad_at(px, py)) {
            flood(plot, zone_kind::crossroad, false);
          } else {
            flood(plot, zone_kind::street, true);
          }
          break;
        case zone_role::yard: flood(plot, zone_kind::yard, true); break;
        default: break; // здания разбираются отдельно: у них два места, зал и стены
      }
    }

    // Перекрёсток с вероятностью становится площадью и вбирает соседние отрезки: город без площади —
    // это план, а не город, и место сбора должно быть чем-то, на что можно показать.
    for (uint32_t id = 0; id < groups.size(); ++id) {
      if (group_kind[id] != zone_kind::crossroad) continue;
      if ((utils::splitmix(uint64_t(id) + 1ull, uint64_t(item.node) ^ 0x59a2eull) & 0xffffull) > 0x3000ull) continue;

      group_kind[id] = zone_kind::square;
      const uint32_t plot = groups[id].front();
      const uint32_t px = plot % side;
      const uint32_t py = plot / side;

      constexpr int32_t offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& offset : offsets) {
        const int64_t nx = int64_t(px) + offset[0];
        const int64_t ny = int64_t(py) + offset[1];
        if (nx < 0 || ny < 0 || nx >= int64_t(side) || ny >= int64_t(side)) continue;

        const uint32_t next = uint32_t(ny) * side + uint32_t(nx);
        const uint32_t owner = group[next];
        if (owner == 0xffffffffu || owner == id) continue;
        if (group_kind[owner] != zone_kind::street || groups[owner].size() != 1) continue;

        groups[id].push_back(next);
        groups[owner].clear();
        group[next] = id;
      }
    }

    const auto kind_label = [](const zone_kind kind) { return std::string(zone_kind_name(kind)); };

    for (uint32_t id = 0; id < groups.size(); ++id) {
      if (groups[id].empty()) continue;

      draft entry{};
      entry.kind = group_kind[id];
      entry.high = 0.2f;
      entry.identity = identity_of(4, item.node, 40000u + id);
      entry.parent_identity = district_of(groups[id].front() % side, groups[id].front() / side);
      entry.name = std::format("{} {}", kind_label(group_kind[id]), id);
      entry.reference = reference;
      if (group_kind[id] != zone_kind::yard) entry.tags = uint32_t(zone_flags::road);

      for (const auto plot : groups[id]) {
        // Массив берётся ОДИН раз: два вызова давали два разных временных объекта, и диапазон собирался
        // из начала одного и конца другого — часть выходила с восемью вершинами вместо четырёх.
        const auto quad = cell_quad(plot % side, plot / side);
        entry.parts.push_back({quad.begin(), quad.end()});
      }
      emit_shape(std::move(entry));
    }

    // Здания: зал из помещений, стены как непроходимое место, двери как отдельные маленькие места и —
    // у части домов — второй этаж. Этаж поднят над первым на перекрытие, поэтому вывод связей по общим
    // рёбрам его с первым не соединяет: в плане фигуры лежат ровно друг над другом, «общее ребро» у них
    // идеальное, а прохода через него нет. Соединяет их ЛЕСТНИЦА — связь, записанная в файл явно. Это и
    // есть ответ на вопрос, зачем графу лежать в данных, если рёбра и так совпадают: совпадение рёбер не
    // равносильно проходу ни в одну сторону — бывают совпавшие рёбра без прохода и проходы без рёбер.
    for (uint32_t plot = 0; plot < side * side; ++plot) {
      const uint32_t px = plot % side;
      const uint32_t py = plot / side;
      if (role_at(px, py) != zone_role::building) continue;

      const auto quad = cell_quad(px, py);
      const auto building_identity = identity_of(4, item.node, plot * plot_stride);
      const auto upper_hall_identity = identity_of(4, item.node, plot * plot_stride + 8);
      const bool two_storey = (utils::splitmix(building_identity, 0x51a12cull) & 0xffffull) < 0x7000ull;

      draft shell{};
      shell.kind = zone_kind::building;
      shell.identity = building_identity;
      shell.parent_identity = district_of(px, py);
      shell.name = std::format("building {},{}", px, py);
      shell.reference = reference;
      const int32_t plot_sector_x = sector_of(double(reference.x) + double(quad[0].x));
      const int32_t plot_sector_y = sector_of(double(reference.y) + double(quad[0].y));
      shell.sector_x = plot_sector_x;
      shell.sector_y = plot_sector_y;
      drafts.push_back(std::move(shell));

      // Всё, из чего состоит это здание, приписано ОДНОМУ сектору — тому же, что и само здание.
      const auto anchor_here = [&](draft& target) {
        target.anchored = true;
        target.sector_x = plot_sector_x;
        target.sector_y = plot_sector_y;
      };

      const double step = (1.0 - 2.0 * inset_u) / double(local.room_side);
      const auto room_quad = [&](const uint32_t room) {
        const uint32_t rx = room % local.room_side;
        const uint32_t ry = room / local.room_side;
        return sub_quad(quad, inset_u + double(rx) * step, inset_u + double(ry) * step,
                        inset_u + double(rx + 1) * step, inset_u + double(ry + 1) * step);
      };

      // Где ставить двери: со сторон, выходящих на улицу или двор.
      std::array<bool, 4> door{};
      constexpr int32_t offsets[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
      for (uint32_t d = 0; d < 4; ++d) {
        const int64_t nx = int64_t(px) + offsets[d][0];
        const int64_t ny = int64_t(py) + offsets[d][1];
        if (nx < 0 || ny < 0 || nx >= int64_t(side) || ny >= int64_t(side)) continue;
        const auto neighbour = role_at(uint32_t(nx), uint32_t(ny));
        door[d] = neighbour == zone_role::street || neighbour == zone_role::yard;
      }

      // Кольцо стен, из которого вырезаны дверные проёмы. Оно и делает покрытие полным: между залом и
      // улицей больше нет пустоты, там стоит непроходимое место со своими рёбрами и соседями.
      const double gap_low = 0.5 - door_u * 0.5;
      const double gap_high = 0.5 + door_u * 0.5;
      const auto wall_ring = [&](draft& target, const std::array<bool, 4>& gaps) {
        const auto band = [&](const bool horizontal, const double from, const double to, const bool near_side) {
          const double a = near_side ? 0.0 : 1.0 - inset_u;
          const double b = a + inset_u;
          if (to - from < 1.0e-6) return;
          target.parts.push_back(horizontal ? sub_quad(quad, from, a, to, b) : sub_quad(quad, a, from, b, to));
        };

        // d: 0 = низ, 1 = верх, 2 = лево, 3 = право. Низ и верх идут во всю ширину, бока — только между ними.
        for (uint32_t d = 0; d < 2; ++d) {
          const bool near_side = d == 0;
          if (gaps[d]) {
            band(true, 0.0, gap_low, near_side);
            band(true, gap_high, 1.0, near_side);
          } else {
            band(true, 0.0, 1.0, near_side);
          }
        }
        for (uint32_t d = 2; d < 4; ++d) {
          const bool near_side = d == 2;
          if (gaps[d]) {
            band(false, inset_u, gap_low, near_side);
            band(false, gap_high, 1.0 - inset_u, near_side);
          } else {
            band(false, inset_u, 1.0 - inset_u, near_side);
          }
        }
      };

      // Первый этаж. У двухэтажного дома последнее помещение отдано лестнице: она такое же место со
      // своими рёбрами, просто вдобавок к соседям по плану у неё есть сосед этажом выше.
      const uint32_t stair_room = two_storey ? rooms_per_plot - 1 : rooms_per_plot;

      draft hall{};
      hall.kind = zone_kind::hall;
      hall.high = storey_height_m;
      hall.identity = identity_of(4, item.node, plot * plot_stride + 1);
      hall.parent_identity = building_identity;
      hall.name = std::format("hall of {},{}", px, py);
      hall.reference = reference;
      hall.tags = uint32_t(zone_flags::indoor);
      for (uint32_t room = 0; room < rooms_per_plot; ++room) {
        if (room == stair_room) continue;
        hall.parts.push_back(room_quad(room));
      }
      anchor_here(hall);
      emit_shape(std::move(hall));

      if (two_storey) {
        draft stair{};
        stair.kind = zone_kind::stair;
        stair.high = storey_height_m;
        stair.identity = identity_of(4, item.node, plot * plot_stride + 7);
        stair.parent_identity = building_identity;
        stair.name = std::format("stair of {},{}", px, py);
        stair.reference = reference;
        stair.tags = uint32_t(zone_flags::indoor);
        stair.parts.push_back(room_quad(stair_room));
        stair.graph_links.push_back({upper_hall_identity, portal_flags::climb | portal_flags::graph});
        anchor_here(stair);
        emit_shape(std::move(stair));
      }

      // Начальный замок ставится только там, где дверь НЕ ЕДИНСТВЕННАЯ. У дома с одной дверью запертая
      // дверь означает запечатанный зал, и это не «интересное препятствие», а кусок города, куда игра
      // никогда не попадёт. То же правило, что и у замков на рёбрах: запирать можно лишь то, без чего
      // связность остаётся.
      const uint32_t door_count = uint32_t(std::count(door.begin(), door.end(), true));
      uint32_t first_door = 4;
      for (uint32_t d = 0; d < 4; ++d) {
        if (door[d] && first_door == 4) first_door = d;
      }

      for (uint32_t d = 0; d < 4; ++d) {
        if (!door[d]) continue;
        draft entry{};
        if (offsets[d][0] == 0) {
          const double v0 = offsets[d][1] < 0 ? 0.0 : 1.0 - inset_u;
          entry.parts.push_back(sub_quad(quad, 0.5 - door_u * 0.5, v0, 0.5 + door_u * 0.5, v0 + inset_u));
        } else {
          const double u0 = offsets[d][0] < 0 ? 0.0 : 1.0 - inset_u;
          entry.parts.push_back(sub_quad(quad, u0, 0.5 - door_u * 0.5, u0 + inset_u, 0.5 + door_u * 0.5));
        }
        entry.high = storey_height_m;
        // Дверь — это МЕСТО, а не свойство ребра. Заперта не улица и не комната: перекрыт кусок
        // поверхности между ними, и закрыть его достаточно один раз, а не на каждом из его рёбер.
        entry.kind = zone_kind::door;
        entry.identity = identity_of(4, item.node, plot * plot_stride + 2 + d);
        entry.parent_identity = building_identity;
        entry.name = std::format("door of {},{} side {}", px, py, d);
        entry.reference = reference;
        // Часть дверей заперта с самого начала. Это НАЧАЛЬНОЕ состояние в файле, а не приговор: рантайм
        // переключает его, и маршрут обязан меняться вслед, без пересборки секторов.
        if (door_count > 1 && d == first_door &&
            (utils::splitmix(entry.identity, 0x9d0c1ull) & 0xffffull) < 0x6000ull) {
          entry.tags |= uint32_t(zone_flags::closed);
        }
        anchor_here(entry);
        emit_shape(std::move(entry));
      }

      draft walls{};
      walls.kind = zone_kind::wall;
      walls.high = storey_height_m;
      walls.identity = identity_of(4, item.node, plot * plot_stride + 6);
      walls.parent_identity = building_identity;
      walls.name = std::format("walls of {},{}", px, py);
      walls.reference = reference;
      walls.tags = zone_flags::impassable | zone_flags::indoor;
      wall_ring(walls, door);
      anchor_here(walls);
      emit_shape(std::move(walls));

      if (!two_storey) continue;

      // Второй этаж. Зазор `storey_gap_m` — это перекрытие: без него вывод связей по общим рёбрам
      // соединил бы верхний зал с нижним прямо сквозь пол, потому что в плане они совпадают.
      const float upper_low = storey_height_m + storey_gap_m;

      draft top{};
      top.kind = zone_kind::hall;
      top.low = upper_low;
      top.high = upper_low + storey_height_m;
      top.floor = 1;
      top.identity = upper_hall_identity;
      top.parent_identity = building_identity;
      top.name = std::format("upper hall of {},{}", px, py);
      top.reference = reference;
      top.tags = uint32_t(zone_flags::indoor);
      for (uint32_t room = 0; room < rooms_per_plot; ++room) {
        top.parts.push_back(room_quad(room));
      }
      anchor_here(top);
      emit_shape(std::move(top));

      draft top_walls{};
      top_walls.kind = zone_kind::wall;
      top_walls.low = upper_low;
      top_walls.high = upper_low + storey_height_m;
      top_walls.floor = 1;
      top_walls.identity = identity_of(4, item.node, plot * plot_stride + 9);
      top_walls.parent_identity = building_identity;
      top_walls.name = std::format("upper walls of {},{}", px, py);
      top_walls.reference = reference;
      top_walls.tags = zone_flags::impassable | zone_flags::indoor;
      wall_ring(top_walls, std::array<bool, 4>{}); // наверху дверей наружу нет: туда попадают по лестнице
      anchor_here(top_walls);
      emit_shape(std::move(top_walls));
    }
  }

  // --- связность выводится из геометрии ---

  struct part_index {
    uint32_t draft = 0;
    uint32_t part = 0;
  };
  std::vector<part_index> items;
  std::vector<std::vector<uint32_t>> draft_items(drafts.size());

  for (uint32_t index = 0; index < drafts.size(); ++index) {
    for (uint32_t part = 0; part < drafts[index].parts.size(); ++part) {
      draft_items[index].push_back(uint32_t(items.size()));
      items.push_back({index, part});
    }
  }

  std::map<line_key, std::vector<edge_ref>> buckets;
  for (uint32_t item = 0; item < items.size(); ++item) {
    const auto& outline = drafts[items[item].draft].parts[items[item].part];
    for (size_t i = 0; i < outline.size(); ++i) {
      line_key key{};
      float begin = 0.0f;
      float end = 0.0f;
      glm::vec2 origin{};
      glm::vec2 direction{};
      const auto reference = drafts[items[item].draft].reference;
      if (!line_of(outline[i], outline[(i + 1) % outline.size()], key, begin, end, origin, direction)) continue;
      (void)origin;
      key.place = quantise(double(reference.x), 1.0 / 64.0) * 1000003 + quantise(double(reference.y), 1.0 / 64.0);
      buckets[key].push_back({item, begin, end, outline[i], outline[(i + 1) % outline.size()], direction, reference});
    }
  }

  struct raw_portal {
    uint32_t a = 0;
    uint32_t b = 0;
    glm::vec2 from{};
    glm::vec2 to{};
  };
  std::vector<raw_portal> raw;

  const auto consider = [&](const edge_ref& first_edge, const edge_ref& second_edge) {
    if (first_edge.item == second_edge.item) return;

    const float lower = std::max(first_edge.begin, second_edge.begin);
    const float upper = std::min(first_edge.end, second_edge.end);
    if (upper - lower < edge_epsilon) return;

    // Узкая фаза решает то, что ключ только предположил: концы второго ребра обязаны лежать НА прямой
    // первого. Допуск безопасен, потому что ближайшая параллельная прямая — стена здания — отстоит на
    // метр двадцать; а слишком МАЛЫЙ допуск отсекал настоящие двери, пока координаты были планетарными.
    const auto normal = glm::vec2{-first_edge.direction.y, first_edge.direction.x};
    const float side_a = glm::dot(second_edge.origin - first_edge.origin, normal);
    const float side_b = glm::dot(second_edge.tail - first_edge.origin, normal);
    if (std::abs(side_a) > collinear_epsilon || std::abs(side_b) > collinear_epsilon) return;

    const auto& first = drafts[items[first_edge.item].draft];
    const auto& second = drafts[items[second_edge.item].draft];

    // Проход выводится только МЕЖДУ ЗОНАМИ ОДНОГО УРОВНЯ. Уровни — это разные карты взаимодействий, а не
    // ярусы одной; ребро между отрезком улицы и прямоугольником поселения, внутри которого улица лежит,
    // означало бы, что персонаж может «войти в поселение» как в комнату. Ловилось это косвенно: у
    // поселения высота сорок метров, и его контур соединялся с ВТОРЫМИ ЭТАЖАМИ пограничных зданий.
    if (first.level != second.level) return;

    // Перекрытие между этажами: диапазоны высот обязаны пересекаться, иначе общего ребра в плане мало.
    if (first.high <= second.low || second.high <= first.low) return;

    // Отрезок восстанавливается в локальных координатах и только потом возвращается в мировые.
    const auto base = first_edge.origin - first_edge.direction * glm::dot(first_edge.origin, first_edge.direction);
    raw.push_back({first_edge.item, second_edge.item,
                   first_edge.reference + base + first_edge.direction * lower,
                   first_edge.reference + base + first_edge.direction * upper});
  };

  for (const auto& [key, list] : buckets) {
    for (size_t i = 0; i < list.size(); ++i) {
      for (size_t j = i + 1; j < list.size(); ++j) {
        consider(list[i], list[j]);
      }
    }

    // Соседняя корзина по смещению просматривается тоже: дрейф `float` может развести два отрезка одной
    // прямой через границу кванта, и без этого прохода они бы друг друга не увидели.
    const auto neighbour = buckets.find(line_key{key.dx, key.dy, key.offset + 1, key.place});
    if (neighbour == buckets.end()) continue;
    for (const auto& near : list) {
      for (const auto& far : neighbour->second) {
        consider(near, far);
      }
    }
  }

  // --- раздача ключей ---

  std::vector<std::vector<uint32_t>> per_sector(size_t(options.sector_side) * options.sector_side);
  const auto slot_of = [&](const draft& entry) -> int64_t {
    const int64_t sx = entry.sector_x - options.sector_x;
    const int64_t sy = entry.sector_y - options.sector_y;
    if (sx < 0 || sy < 0 || sx >= int64_t(options.sector_side) || sy >= int64_t(options.sector_side)) return -1;
    return sy * int64_t(options.sector_side) + sx;
  };

  for (uint32_t index = 0; index < drafts.size(); ++index) {
    const auto slot = slot_of(drafts[index]);
    if (slot >= 0) per_sector[size_t(slot)].push_back(index);
  }

  std::vector<zone_key> keys(drafts.size(), invalid_key);
  std::map<uint64_t, zone_key> by_identity;
  std::map<uint64_t, uint32_t> draft_by_identity;
  for (uint32_t index = 0; index < drafts.size(); ++index) {
    draft_by_identity[drafts[index].identity] = index;
  }
  for (uint32_t y = 0; y < options.sector_side; ++y) {
    for (uint32_t x = 0; x < options.sector_side; ++x) {
      const auto& list = per_sector[size_t(y) * options.sector_side + x];
      for (uint32_t local_index = 0; local_index < list.size(); ++local_index) {
        const auto key = make_key(options.sector_x + int32_t(x), options.sector_y + int32_t(y), local_index);
        keys[list[local_index]] = key;
        by_identity[drafts[list[local_index]].identity] = key;
      }
    }
  }

  // Связность проходимого — контракт генерации, а не надежда. Стены стали непроходимыми, и внутренний
  // двор квартала остаётся отрезанным, если ни одно смежное здание не выходит наружу: раньше двор
  // соприкасался с постройкой напрямую, теперь между ними стена. Такие карманы вскрываются проёмом в
  // стене — ограниченная починка, а не молчаливое согласие с непроходимым куском города.
  {
    std::vector<std::vector<uint32_t>> neighbours(items.size());
    for (const auto& item : raw) {
      neighbours[item.a].push_back(item.b);
      neighbours[item.b].push_back(item.a);
    }

    std::vector<uint32_t> owner(items.size());
    for (uint32_t i = 0; i < owner.size(); ++i) {
      owner[i] = i;
    }
    const auto root_of = [&](uint32_t node) {
      while (owner[node] != node) {
        owner[node] = owner[owner[node]];
        node = owner[node];
      }
      return node;
    };
    const auto passable_item = [&](const uint32_t item) {
      const auto& entry = drafts[items[item].draft];
      return (entry.tags & uint32_t(zone_flags::impassable)) == 0 && !entry.parts.empty();
    };
    const auto join = [&](const uint32_t a, const uint32_t b) {
      const uint32_t ra = root_of(a);
      const uint32_t rb = root_of(b);
      if (ra != rb) owner[ra] = rb;
    };

    for (const auto& item : raw) {
      if (passable_item(item.a) && passable_item(item.b)) join(item.a, item.b);
    }

    std::map<uint32_t, uint32_t> weight;
    for (uint32_t i = 0; i < items.size(); ++i) {
      if (passable_item(i)) ++weight[root_of(i)];
    }

    uint32_t main_root = 0;
    uint32_t main_weight = 0;
    for (const auto& [component, count] : weight) {
      if (count > main_weight) {
        main_weight = count;
        main_root = component;
      }
    }

    // Стена вскрывается, если разделяет РАЗНЫЕ компоненты. Требовать, чтобы одной из них была главная,
    // оказалось мало: карман бывает отделён от города не одной стеной, а цепочкой, и тогда он ждёт, пока
    // вскроется соседний. Слияние любых двух компонент доводит починку до конца за несколько проходов, а
    // число вскрытых стен всё равно ограничено числом карманов.
    (void)main_root;
    for (uint32_t pass = 0; pass < 12; ++pass) {
      bool changed = false;
      for (uint32_t item = 0; item < items.size(); ++item) {
        if (passable_item(item)) continue;

        uint32_t first_component = 0xffffffffu;
        bool separates = false;
        for (const auto next : neighbours[item]) {
          if (!passable_item(next)) continue;
          const uint32_t component = root_of(next);
          if (first_component == 0xffffffffu) first_component = component;
          else if (component != first_component) separates = true;
        }
        if (!separates) continue;

        drafts[items[item].draft].tags &= ~uint32_t(zone_flags::impassable);
        for (const auto next : neighbours[item]) {
          if (passable_item(next)) join(item, next);
        }
        changed = true;
      }
      if (!changed) break;
    }
  }

  // Остов строится на тех рёбрах, которые попадут в файлы, и только потом ставятся замки на лишних.
  std::vector<uint32_t> parent(items.size());
  for (uint32_t i = 0; i < parent.size(); ++i) {
    parent[i] = i;
  }
  const auto root = [&](uint32_t node) {
    while (parent[node] != node) {
      parent[node] = parent[parent[node]];
      node = parent[node];
    }
    return node;
  };

  const auto passable_draft = [&](const uint32_t item) {
    return (drafts[items[item].draft].tags & uint32_t(zone_flags::impassable)) == 0;
  };

  std::vector<uint32_t> flags(raw.size(), uint32_t(portal_flags::open));
  for (uint32_t i = 0; i < raw.size(); ++i) {
    if (keys[items[raw[i].a].draft] == invalid_key || keys[items[raw[i].b].draft] == invalid_key) continue;

    // Ребро в НЕПРОХОДИМОЕ место в остове не участвует. Иначе стена даёт мнимую связность: два двора,
    // соединённые только через неё, считаются уже связанными, и настоящая дверь между ними объявляется
    // лишним ребром и запирается. Замок тогда отрезает кусок города, хотя правило замков ровно об
    // обратном — запирать только то, без чего связность остаётся.
    if (!passable_draft(raw[i].a) || !passable_draft(raw[i].b)) continue;
    if (drafts[items[raw[i].a].draft].kind == zone_kind::door ||
        drafts[items[raw[i].b].draft].kind == zone_kind::door) {
      flags[i] |= uint32_t(portal_flags::door);
    }

    const uint32_t ra = root(raw[i].a);
    const uint32_t rb = root(raw[i].b);
    if (ra != rb) {
      parent[ra] = rb;
      continue;
    }
    if ((utils::splitmix(uint64_t(i) + 1ull, 0x10c6edull) & 0xffffull) < 0x2000ull) {
      flags[i] |= uint32_t(portal_flags::locked);
    }
  }

  std::vector<std::vector<zone_portal>> outgoing(items.size());
  for (uint32_t i = 0; i < raw.size(); ++i) {
    const auto& a = items[raw[i].a];
    const auto& b = items[raw[i].b];
    if (keys[a.draft] == invalid_key || keys[b.draft] == invalid_key) continue;
    outgoing[raw[i].a].push_back({keys[b.draft], b.part, raw[i].from, raw[i].to, flags[i]});
    outgoing[raw[i].b].push_back({keys[a.draft], a.part, raw[i].from, raw[i].to, flags[i]});
  }
  // Связь без геометрии кладётся В ОБЕ СТОРОНЫ. Односторонняя запись означала бы лестницу, по которой
  // можно подняться и нельзя спуститься, и заметилось бы это только тем, что персонаж застрял наверху.
  // Связь без геометрии кладётся В ОБЕ СТОРОНЫ. Односторонняя запись означала бы лестницу, по которой
  // можно подняться и нельзя спуститься, и заметилось бы это только тем, что персонаж застрял наверху.
  //
  // У зоны С ФОРМОЙ связь висит на первой части: у лестницы есть геометрия, и её ребро графа принадлежит
  // этой геометрии. У зоны БЕЗ формы частей нет, и связь идёт в собственный список записи — раньше она
  // на этом месте просто пропадала, и весь политический уровень уезжал на диск набором изолированных
  // узлов, чего не замечала ни одна проверка, потому что все они смотрели на порталы частей.
  std::vector<std::vector<zone_portal>> node_links(drafts.size());
  const auto attach = [&](const uint32_t index, const zone_portal& portal) {
    if (draft_items[index].empty()) node_links[index].push_back(portal);
    else outgoing[draft_items[index][0]].push_back(portal);
  };

  for (uint32_t index = 0; index < drafts.size(); ++index) {
    if (keys[index] == invalid_key) continue;
    for (const auto& link : drafts[index].graph_links) {
      const auto found = by_identity.find(link.identity);
      const auto target = draft_by_identity.find(link.identity);
      if (found == by_identity.end() || target == draft_by_identity.end()) continue;
      attach(index, {found->second, 0, {}, {}, link.flags});
      attach(target->second, {keys[index], 0, {}, {}, link.flags});
    }
  }

  build_stats stats{};
  stats.settlements = uint32_t(settlements.size());

  for (uint32_t y = 0; y < options.sector_side; ++y) {
    for (uint32_t x = 0; x < options.sector_side; ++x) {
      const auto& list = per_sector[size_t(y) * options.sector_side + x];

      zone_sector out{};
      out.x = options.sector_x + int32_t(x);
      out.y = options.sector_y + int32_t(y);
      out.names.push_back('\0');

      for (const auto index : list) {
        const auto& item = drafts[index];
        zone_record record{};
        record.key = keys[index];
        record.level = item.level;
        record.kind = item.kind;
        record.tags = item.tags;
        record.floor = item.floor;

        const auto parent_key = by_identity.find(item.parent_identity);
        record.parent = parent_key == by_identity.end() ? invalid_key : parent_key->second;

        record.name_offset = uint32_t(out.names.size());
        out.names.insert(out.names.end(), item.name.begin(), item.name.end());
        out.names.push_back('\0');

        record.part_begin = uint32_t(out.parts.size());
        record.part_count = uint32_t(item.parts.size());
        record.bounds = zone_bounds{{1e30f, item.low, 1e30f}, {-1e30f, item.high, -1e30f}};

        for (uint32_t part = 0; part < item.parts.size(); ++part) {
          zone_part entry{};
          entry.vertex_begin = uint32_t(out.vertices.size());
          entry.vertex_count = uint32_t(item.parts[part].size());
          for (const auto& point : item.parts[part]) {
            out.vertices.push_back(point + item.reference);
          }

          entry.bounds = bounds_of({out.vertices.end() - entry.vertex_count, out.vertices.end()}, item.low, item.high);
          record.bounds = merge_bounds(record.bounds, entry.bounds);

          const auto& list_for_part = outgoing[draft_items[index][part]];
          entry.portal_begin = uint32_t(out.portals.size());
          out.portals.insert(out.portals.end(), list_for_part.begin(), list_for_part.end());
          entry.portal_count = uint32_t(out.portals.size()) - entry.portal_begin;

          out.parts.push_back(entry);
        }
        if (item.parts.empty()) record.bounds = zone_bounds{};

        record.link_begin = uint32_t(out.portals.size());
        out.portals.insert(out.portals.end(), node_links[index].begin(), node_links[index].end());
        record.link_count = uint32_t(out.portals.size()) - record.link_begin;

        out.zones.push_back(record);
      }

      out.fingerprint = compute_fingerprint(out);
      write_sector(sector_path(options.root, out.x, out.y), out);

      ++stats.sectors;
      stats.zones += uint32_t(out.zones.size());
      stats.links += uint32_t(out.portals.size());
      stats.bytes += out.byte_size();
    }
  }

  stats.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin_time).count();
  return stats;
}

} // namespace devils_engine::pf09
