#include "cave.h"
#include "surface.h"
#include "station.h"
#include "tunnel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>

#include "local_structure.h"
#include "world_chain.h"
#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/originator/generator_resource.h"
#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/skeleton.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"

namespace gn09 {

namespace originator = devils_engine::originator;

// Поле пещеры по первым room_count комнатам и link_count связям (прежняя часть
// растущей пещеры), без проверки коробки.
double cave_field_prefix(uint64_t seed, const site& local, double clearance_radius, point position,
                         size_t room_count, size_t link_count);

namespace {

constexpr double pi = 3.14159265358979323846;

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const point a) { return std::sqrt(dot(a, a)); }
point normalized(const point a) { return scale(a, 1.0 / length(a)); }
point cross(const point a, const point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double segment_distance(const point p, const point a, const point b) {
  const point ab = subtract(b, a);
  const double t = std::clamp(dot(subtract(p, a), ab) / dot(ab, ab), 0.0, 1.0);
  return length(subtract(p, add(a, scale(ab, t))));
}

bool resource_cave(const site_recipe recipe) { return recipe != site_recipe::nest_cave; }

bool is_entrance(const room& r) { return r.purpose == "mouth" || r.purpose == "entrance"; }

// ---------------------------------------------------------------------------------------
// Форма. Отрезок с галереей — уменьшенная галерея маршрута: эллипс сечения с отдельными
// полом и сводом, раздутие вдоль, стены искажены шумом в обе стороны. Камера — эллипсоид
// (вытянут по своему курсу, сплюснут по вертикали) с тем же шумом. Под формой всегда
// лежит гарантированное ядро: капсула R + охранный слой с неровностью только наружу,
// поэтому шум может сузить красивую форму, но не проход.

constexpr double shape_noise = 1.1; // наибольшее искажение формы, в единицах поля

double shape_noise_at(const uint64_t seed, const point p) {
  return 0.8 * value_noise(seed + 26, p, 6.0) + 0.3 * value_noise(seed + 27, p, 2.5);
}

struct gallery_frame { point axis, right, up; double span; };

gallery_frame frame_of(const point a, const point b) {
  gallery_frame f;
  const point d = subtract(b, a);
  f.span = length(d);
  f.axis = scale(d, 1.0 / f.span);
  f.right = std::abs(f.axis.y) < 0.9 ? normalized(cross(f.axis, {0, 1, 0})) : point{1, 0, 0};
  f.up = cross(f.right, f.axis);
  return f;
}

double bump(const double t, const double centre, const double span) {
  const double q = std::max(0.0, 1.0 - std::abs(t - centre) / span);
  return q * q * (3.0 - 2.0 * q);
}

// Полуоси сечения галереи в доле t: ширина, свод, пол.
struct section { double width, roof, floor; };
section section_at(const room_link& link, const double t) {
  const double pulse = link.bulge * bump(t, link.bulge_at, 0.35);
  return {link.width + pulse, link.roof + 0.7 * pulse, link.floor + 0.2 * pulse};
}

double gallery_largest(const room_link& link) {
  return std::max({link.width + link.bulge, link.roof + 0.7 * link.bulge, link.floor + 0.2 * link.bulge});
}

// Значение формы галереи без шума в точке q.
double gallery_shape(const room_link& link, const point a, const gallery_frame& f, const point q) {
  const double along = dot(subtract(q, a), f.axis);
  const double t = std::clamp(along / f.span, 0.0, 1.0);
  const point offset = subtract(q, add(a, scale(f.axis, std::clamp(along, 0.0, f.span))));
  const double side = dot(offset, f.right), height = dot(offset, f.up);
  const double beyond = std::max({-along, along - f.span, 0.0});
  const section s = section_at(link, t);
  const double vertical = height >= 0.0 ? s.roof : s.floor;
  const double smallest = std::min(s.width, vertical);
  // Множитель — наибольшая полуось: поверхность та же (k = 1), а значение снаружи не
  // меньше расстояния без неё, так что коробка пещеры остаётся тесной.
  const double largest = gallery_largest(link);
  return (std::sqrt((side / s.width) * (side / s.width) + (height / vertical) * (height / vertical) +
                    (beyond / smallest) * (beyond / smallest)) - 1.0) * largest;
}

struct chamber_axes { double along, vertical, across, yaw; };
chamber_axes axes_of(const room& r) { return {r.radius * r.stretch, r.radius * r.squash, r.radius, r.yaw}; }

point chamber_local(const room& r, const point q) {
  const point d = subtract(q, r.centre);
  const double c = std::cos(r.yaw), s = std::sin(r.yaw);
  return {d.x * c + d.z * s, d.y, -d.x * s + d.z * c};
}

double chamber_shape(const room& r, const point q) {
  const chamber_axes a = axes_of(r);
  const point l = chamber_local(r, q);
  const double largest = std::max({a.along, a.vertical, a.across});
  return (std::sqrt((l.x / a.along) * (l.x / a.along) + (l.y / a.vertical) * (l.y / a.vertical) +
                    (l.z / a.across) * (l.z / a.across)) - 1.0) * largest;
}

bool is_chamber(const room& r, const double core) { return r.radius > core + 0.01; }

// Нижняя оценка формы с наибольшей полуосью E на расстоянии d без сдвига: k не меньше
// d/E, а множитель — E, поэтому значение не меньше d - E (минус шум и сдвиг).
double shape_bound(const double d, const double largest) {
  return d - cave_warp_amount - largest - shape_noise;
}
// Расстояние без сдвига, за которым форма заведомо больше value.
double shape_reach(const double largest, const double value) {
  return largest + value + shape_noise + cave_warp_amount;
}

// Коробка, за которой каждое слагаемое поля пещеры больше фоновой скалы (3.46).
void bound_cave(site& local, const double core) {
  constexpr double beyond = 3.5;
  point low{local.centre.x - 1.0, local.centre.y - 1.0, local.centre.z - 1.0};
  point high{local.centre.x + 1.0, local.centre.y + 1.0, local.centre.z + 1.0};
  const auto grow = [&](const point p, const double r) {
    low = {std::min(low.x, p.x - r), std::min(low.y, p.y - r), std::min(low.z, p.z - r)};
    high = {std::max(high.x, p.x + r), std::max(high.y, p.y + r), std::max(high.z, p.z + r)};
  };
  const double guarantee = core + cave_relief + beyond;
  for (size_t i = 0; i < local.rooms.size(); ++i) {
    const room& r = local.rooms[i];
    double reach = i == 0 ? r.radius + beyond : guarantee;
    if (i > 0 && is_chamber(r, core)) {
      const chamber_axes a = axes_of(r);
      reach = std::max(reach, shape_reach(std::max({a.along, a.vertical, a.across}), beyond));
    }
    grow(r.centre, reach);
  }
  for (const room_link& link : local.links) {
    if (link.width <= 0.0) continue;
    const double reach = shape_reach(gallery_largest(link), beyond);
    grow(local.rooms[link.a].centre, reach);
    grow(local.rooms[link.b].centre, reach);
  }
  local.low = low;
  local.high = high;
}

// Скелет пещеры как граф originator: узел — центр комнаты, комната 0 (полость узла)
// непроходима — через неё пути внутри пещеры не считаются.
struct cave_graph {
  std::vector<std::array<double, 3>> nodes;
  std::vector<std::array<uint32_t, 2>> edges;
  std::vector<uint8_t> passable;
  explicit cave_graph(const site& local) {
    for (const room& r : local.rooms) nodes.push_back({r.centre.x, r.centre.y, r.centre.z});
    for (const room_link& l : local.links) edges.push_back({l.a, l.b});
    passable.assign(nodes.size(), 1);
    if (!passable.empty()) passable[0] = 0;
  }
  originator::skeleton_graph view() const { return {nodes, edges, passable}; }
};

// Путь по связям от ближайшего входа до каждой комнаты.
std::vector<double> path_lengths(const site& local) {
  const cave_graph graph(local);
  return originator::skeleton_distance(graph.view(), local.entrances);
}

// Луч из точки лодки к цели упирается в породу итогового поля.
// threshold — насколько глубоко в породе луч должен пройти хоть в одной точке:
// генератор требует 1 м, чтобы соседние лучи независимой проверки тоже упирались.
template <typename Field>
bool sight_blocked(Field& field, const point from, const point to, const double step,
                   const double threshold = 0.25) {
  const point delta = subtract(from, to);
  const double span = length(delta);
  const int steps = std::max(1, int(std::ceil(span / step)));
  // От цели к лодке: гнездо закрыто ближними стенами, и выход случается рано.
  for (int i = 1; i < steps; ++i) {
    if (field(add(to, scale(delta, double(i) / steps))) > threshold) return true;
  }
  return false;
}

std::vector<point> nest_targets(const room& nest) {
  const double r = 0.4 * nest.radius;
  return {nest.centre, add(nest.centre, {r, 0, 0}), add(nest.centre, {-r, 0, 0}),
          add(nest.centre, {0, 0, r}), add(nest.centre, {0, 0, -r}),
          add(nest.centre, {0, 0.5 * r * nest.squash, 0})};
}

// Точки, где бывает лодка возле пещеры: узлы маршрута рядом с её входами с шаром
// поворота, капсулы рёбер, и предзалы пещеры, куда лодка заходит носом.
std::vector<point> boat_points_near(const adventure& context, const site& cave, const double step,
                                    const double near = 60.0) {
  std::vector<point> entrances;
  for (const uint32_t e : cave.entrances) entrances.push_back(cave.rooms[e].centre);
  const auto near_entrance = [&](const point p) {
    for (const point& e : entrances) if (length(subtract(p, e)) < near) return true;
    return false;
  };
  std::vector<point> out;
  for (const node& n : context.route.nodes) {
    if (length(subtract(n.centre, cave.centre)) > cave_extent + 60.0) continue;
    for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
      const point p = (x || y || z) ? add(n.centre, scale(normalized({double(x), double(y), double(z)}),
                                                          0.75 * starter_sub_turn_radius))
                                    : n.centre;
      if (near_entrance(p)) out.push_back(p);
    }
  }
  for (const edge link : context.route.edges) {
    const point a = context.route.nodes[link.a].centre, b = context.route.nodes[link.b].centre;
    if (segment_distance(cave.centre, a, b) > cave_extent + 60.0) continue;
    const gallery_frame f = frame_of(a, b);
    for (double s = step; s < f.span; s += step) {
      const point c = add(a, scale(f.axis, s));
      for (const point offset : {point{}, scale(f.right, 3.0), scale(f.right, -3.0), scale(f.up, 3.0), scale(f.up, -3.0)}) {
        const point p = add(c, offset);
        if (near_entrance(p)) out.push_back(p);
      }
    }
  }
  for (const room& r : cave.rooms) {
    if (r.purpose == "vestibule") out.push_back(r.centre);
  }
  return out;
}

// Что отвергло вариант (GN09_CAVE_DEBUG).
enum cave_refusal : uint8_t { refused_world, refused_older, refused_rock, refused_self, refused_sight, refusal_count };

using cave_list = std::vector<std::shared_ptr<const site>>;

const std::array<point, 14>& ring_directions() {
  static const std::array<point, 14> out = [] {
    std::array<point, 14> list{};
    size_t n = 0;
    for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
      if ((x == 0) + (y == 0) + (z == 0) == 2 || (x && y && z)) list[n++] = normalized({double(x), double(y), double(z)});
    }
    return list;
  }();
  return out;
}

// Точки кольца породы вокруг нового куска: по направлению — наибольшая стена формы
// (полуось с шумом и сдвигом) или гарантированного ядра, и ещё ring. Шаг вдоль
// отрезка step, направлений в сечении around.
template <typename Visit>
bool for_each_ring_point(const site& cave, const double core, const room_link& link, const double ring,
                         const double step, const int around, const double phase, Visit&& visit) {
  const point a = cave.rooms[link.a].centre, b = cave.rooms[link.b].centre;
  const gallery_frame f = frame_of(a, b);
  const int steps = std::max(1, int(std::ceil(f.span / step)));
  for (int s = 0; s <= steps; ++s) {
    const double t = double(s) / steps;
    const point c = add(a, scale(f.axis, f.span * t));
    for (int k = 0; k < around; ++k) {
      const double angle = (k + phase * (s % 2)) * 2.0 * pi / around;
      const double cs = std::cos(angle), sn = std::sin(angle);
      double wall = core + cave_relief;
      if (link.width > 0.0) {
        const section sec = section_at(link, t);
        const double vertical = sn >= 0.0 ? sec.roof : sec.floor;
        const double r = 1.0 / std::sqrt((cs / sec.width) * (cs / sec.width) + (sn / vertical) * (sn / vertical));
        wall = std::max(wall, r * (1.0 + shape_noise / gallery_largest(link)) + cave_warp_amount);
      }
      if (!visit(add(c, scale(add(scale(f.right, cs), scale(f.up, sn)), wall + ring)))) return false;
    }
  }
  return true;
}

template <typename Visit>
bool for_each_ring_point(const room& r, const double core, const double ring, Visit&& visit) {
  for (const point& dir : ring_directions()) {
    double wall = core + cave_relief;
    if (is_chamber(r, core)) {
      const chamber_axes a = axes_of(r);
      const double c = std::cos(a.yaw), s = std::sin(a.yaw);
      const point l{dir.x * c + dir.z * s, dir.y, -dir.x * s + dir.z * c};
      const double radius = 1.0 / std::sqrt((l.x / a.along) * (l.x / a.along) + (l.y / a.vertical) * (l.y / a.vertical) +
                                            (l.z / a.across) * (l.z / a.across));
      wall = std::max(wall, radius * (1.0 + shape_noise / std::max({a.along, a.vertical, a.across})) + cave_warp_amount);
    }
    if (!visit(add(r.centre, scale(dir, wall + ring)))) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------
// Генератор пещеры — ресурс originator (`generator/cave`): как пещера растёт, решают шаги на lua
// и числа конфига, а здесь только БЛОКИ, которые эти шаги зовут, и перевод буферов в site.
//
// Блок держит то, что хост обязан гарантировать при любом скрипте, в том числе из мода: часть
// скелета в природной породе с кольцом, не дальше границы места (иначе отсечение пещер по коробке
// cave_reach соврёт), не сходится со своей прежней частью и с пещерами соседних мест. Поэтому граница
// места здесь константа хоста, а не параметр, который скрипт мог бы раздвинуть.
//
// Блоки сами находят контекст раскладки через цепочку по ключу пещеры и зерну (вход `layout_meta`): окно
// канонической окрестности (генератор context -> window), узел места, старших соседей (elders места ->
// их пещеры). Исполнителя хоста вокруг шагов нет. Между вызовами блоки помнят только ЧИСТЫЕ
// мемоизации: точки лодки (функция окна, центра и входов) и подготовленную выборку поля — обе держат
// то, от чего зависят, поэтому совпадение указателя значит то же самое окно и то же самое поле.

// Природное поле канонической окрестности — поле из примитивов `natural` генератора cave, через
// сэмплер с подготовленной областью.
struct natural_field {
  std::shared_ptr<const originator::primitive_field> field;
  originator::primitive_field::sampler samples;
  double operator()(const point p) { return samples({p.x, p.y, p.z}); }
};

struct design_context {
  std::shared_ptr<const adventure> context_handle;
  const adventure* context = nullptr;
  cave_list older;
  natural_field* base = nullptr;
  double core = 0.0;
  // Скелет текущей раскладки, собранный из буферов вызова. Назначения не нужны блокам, кроме
  // признака входа, — он и записывается.
  site cave;
  std::array<uint32_t, refusal_count> why{};
  double fits_ms = 0.0, sight_ms = 0.0;
};

// Счётчики отказов раскладки (GN09_CAVE_DEBUG): по ключу пещеры, печатаются при разборе её site.
struct design_stats {
  std::array<uint32_t, refusal_count> why{};
  double fits_ms = 0.0, sight_ms = 0.0;
};
std::mutex stats_lock;
std::map<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>, design_stats> stats_by_cave;

// Контекст раскладки из ключа пещеры (входы 13 — ключ, 14 — зерно) и радиуса прохода (параметр).
design_context resolve_design(const originator::tool_call& call) {
  const auto key_field = call.input(13).read();
  const uint64_t seed = originator::read_key_word(call.input(14).read(), 0);
  const originator::instance_key key{{originator::read_key_word(key_field, 0, 0),
                                      originator::read_key_word(key_field, 0, 1),
                                      originator::read_key_word(key_field, 0, 2)}};
  const double clearance = call.params->number("clearance");
  auto& chain = world_chain(clearance);
  const auto context = chain.get("context", seed, {{key.words[0], key.words[1]}});
  const auto canonical_corner = context->field("canonical", "corner"), canonical_size = context->field("canonical", "size");
  const auto window = chain.get("window", seed, {{originator::read_key_word(canonical_corner, 0, 0),
                                                  originator::read_key_word(canonical_corner, 0, 1),
                                                  originator::read_key_word(canonical_size, 0, 0)}});
  design_context d;
  d.context_handle = world_view(*window);
  if (d.context_handle == nullptr) devils_engine::utils::error{}("GN09: a cave window came without its world");
  d.context = d.context_handle.get();
  const node* at = nullptr;
  for (const node& n : d.context->route.nodes) if (n.key == key.words[2]) at = &n;
  if (at == nullptr) devils_engine::utils::error{}("GN09: cave node {:x} is not in its window", key.words[2]);
  const auto place = chain.get("place", seed, key);
  const auto elders = size_t(place->field("site", "elders").get(0));
  const auto sector = place->field("elders", "sector"), elder_node = place->field("elders", "node");
  for (size_t i = 0; i < elders; ++i) {
    const auto elder = chain.get("cave", seed, {{originator::read_key_word(sector, i, 0), originator::read_key_word(sector, i, 1),
                                                 originator::read_key_word(elder_node, i, 0)}});
    if (auto local = site_view(*elder, clearance); local->has_cave) d.older.push_back(std::move(local));
  }
  d.core = cave_core(clearance);
  d.cave.centre = at->centre;
  return d;
}

// Подготовленная выборка природного поля, общая для вызовов блоков одной сборки (держит поле).
natural_field& natural_of() {
  thread_local std::optional<natural_field> memo;
  const auto& field = originator::current_field_handle("natural");
  if (!memo || memo->field != field) {
    memo.reset();
    memo.emplace(natural_field{field, originator::primitive_field::sampler(*field, 24.0)});
  }
  return *memo;
}

void add_stats(const originator::tool_call& call, const design_context& d) {
  if (!std::getenv("GN09_CAVE_DEBUG")) return;
  const auto key_field = call.input(13).read();
  const uint64_t seed = originator::read_key_word(call.input(14).read(), 0);
  const originator::instance_key key{{originator::read_key_word(key_field, 0, 0),
                                      originator::read_key_word(key_field, 0, 1),
                                      originator::read_key_word(key_field, 0, 2)}};
  std::lock_guard guard(stats_lock);
  auto& stats = stats_by_cave[{seed, key.words[0], key.words[1], key.words[2]}];
  for (size_t i = 0; i < refusal_count; ++i) stats.why[i] += d.why[i];
  stats.fits_ms += d.fits_ms;
  stats.sight_ms += d.sight_ms;
}

struct milliseconds_into {
  double& into;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  ~milliseconds_into() {
    into += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  }
};

// Входы блоков скелета, по порядку: комнаты (центр, радиус, сплющенность, вытянутость, курс,
// признаки) и связи (концы, полуширина, свод, пол, раздутие, где раздутие).
void read_skeleton(const originator::tool_call& call, site& cave) {
  const auto rooms = size_t(std::max<int64_t>(call.params->integer("rooms"), 0));
  const auto links = size_t(std::max<int64_t>(call.params->integer("links"), 0));
  if (rooms < 2 || rooms > call.input(0).count() || links > call.input(6).count()) {
    devils_engine::utils::error{}("GN09 step '{}': {} got {} rooms and {} links, the buffers hold {} and {}",
                                  call.step_name, call.tool_name, rooms, links, call.input(0).count(), call.input(6).count());
  }
  const auto centre = call.input(0).read(), radius = call.input(1).read(), squash = call.input(2).read(),
             stretch = call.input(3).read(), yaw = call.input(4).read(), flags = call.input(5).read();
  cave.rooms.resize(rooms);
  cave.entrances.clear();
  for (size_t i = 0; i < rooms; ++i) {
    room& r = cave.rooms[i];
    r.centre = {centre.get(i, 0), centre.get(i, 1), centre.get(i, 2)};
    r.radius = radius.get(i);
    r.squash = squash.get(i);
    r.stretch = stretch.get(i);
    r.yaw = yaw.get(i);
    const bool entrance = (uint32_t(flags.get(i)) & 1u) != 0;
    r.purpose = entrance ? "entrance" : "";
    if (entrance) cave.entrances.push_back(uint32_t(i));
  }
  const auto a = call.input(6).read(), b = call.input(7).read(), width = call.input(8).read(),
             roof = call.input(9).read(), floor = call.input(10).read(), bulge = call.input(11).read(),
             bulge_at = call.input(12).read();
  cave.links.resize(links);
  for (size_t l = 0; l < links; ++l) {
    room_link& link = cave.links[l];
    link.a = uint32_t(a.get(l));
    link.b = uint32_t(b.get(l));
    if (link.a >= rooms || link.b >= rooms) {
      devils_engine::utils::error{}("GN09 step '{}': {} link {} joins rooms {}-{}, only {} exist", call.step_name,
                                    call.tool_name, l, link.a, link.b, rooms);
    }
    link.width = width.get(l);
    link.roof = roof.get(l);
    link.floor = floor.get(l);
    link.bulge = bulge.get(l);
    link.bulge_at = bulge_at.get(l);
  }
  cave.has_cave = true;
}

// Новые комнаты и связи после отметки (from_rooms, from_links): в мире, не дальше cave_extent, в
// породе с кольцом (кроме входов), в стороне от своей прежней части и от пещер старших мест.
bool part_fits(design_context& d, const size_t from_rooms, const size_t from_links) {
  const milliseconds_into timer{d.fits_ms};
  const adventure& context = *d.context;
  const site& cave = d.cave;
  const double core = d.core;
  for (size_t i = std::max<size_t>(from_rooms, 1); i < cave.rooms.size(); ++i) {
    const room& r = cave.rooms[i];
    const chamber_axes a = axes_of(r);
    const double reach = std::max({a.along, a.vertical, a.across, core}) + cave_relief + cave_rock_ring;
    if (r.centre.y - reach < world_y_min + 2.0 || r.centre.y + reach > world_y_max - 2.0 ||
        length(subtract(r.centre, cave.centre)) + reach > cave_extent) {
      ++d.why[refused_world];
      return false;
    }
  }
  for (size_t l = from_links; l < cave.links.size(); ++l) {
    const room_link& link = cave.links[l];
    if (link.width <= 0.0) continue;
    const double reach = gallery_largest(link) + cave_relief + cave_rock_ring;
    for (const uint32_t end : {link.a, link.b}) {
      if (length(subtract(cave.rooms[end].centre, cave.centre)) + reach > cave_extent) {
        ++d.why[refused_world];
        return false;
      }
    }
  }
  std::vector<point> doors;
  for (const uint32_t e : cave.entrances) doors.push_back(cave.rooms[e].centre);
  // Прежняя часть пещеры: новая не подходит к ней ближе кольца породы, кроме места стыка, —
  // иначе ходы сольются и путь вглубь сократится.
  std::vector<std::pair<point, double>> joints;
  for (size_t l = from_links; l < cave.links.size(); ++l) {
    const room_link& link = cave.links[l];
    for (const uint32_t end : {link.a, link.b}) {
      if (end >= from_rooms) continue;
      const room& r = cave.rooms[end];
      const chamber_axes a = axes_of(r);
      joints.push_back({r.centre, std::max({a.along, a.vertical, a.across, core}) + gallery_largest(link) + 6.0});
    }
  }
  const auto check = [&](const point p) {
    for (const point& door : doors) if (length(subtract(p, door)) < 10.0) return true;
    if ((*d.base)(p) <= 0.0) { ++d.why[refused_rock]; return false; }
    bool joint = false;
    for (const auto& [at, size] : joints) joint = joint || length(subtract(p, at)) < size;
    if (!joint && cave_field_prefix(context.seed, cave, context.clearance_radius, p, from_rooms, from_links) <= 0.0) {
      ++d.why[refused_self];
      return false;
    }
    for (const auto& other : d.older) {
      if (local_cave_field(context.seed, *other, context.clearance_radius, p) <= 0.0) {
        ++d.why[refused_older];
        return false;
      }
    }
    return true;
  };
  for (size_t l = from_links; l < cave.links.size(); ++l) {
    const room_link& link = cave.links[l];
    if (link.a == 0 || link.b == 0) continue;
    if (!for_each_ring_point(cave, core, link, cave_rock_ring, 1.0, 12, 0.5, check)) return false;
  }
  for (size_t i = std::max<size_t>(from_rooms, 1); i < cave.rooms.size(); ++i) {
    if (is_entrance(cave.rooms[i])) continue;
    if (!for_each_ring_point(cave.rooms[i], core, cave_rock_ring, check)) return false;
  }
  return true;
}

// Из лодки цель не видна: каждый луч где-то проходит породу глубже метра.
bool target_hidden(design_context& d, const uint32_t target) {
  const milliseconds_into timer{d.sight_ms};
  const adventure& context = *d.context;
  site& cave = d.cave;
  bound_cave(cave, d.core);
  // Точки лодки зависят только от входов: пересчитываются, когда входы меняются.
  std::vector<point> doors;
  for (const uint32_t e : cave.entrances) doors.push_back(cave.rooms[e].centre);
  const auto same = [](const std::vector<point>& l, const std::vector<point>& r) {
    if (l.size() != r.size()) return false;
    for (size_t i = 0; i < l.size(); ++i) {
      if (l[i].x != r[i].x || l[i].y != r[i].y || l[i].z != r[i].z) return false;
    }
    return true;
  };
  // Точки лодки — функция окна, центра пещеры и входов (назначения комнат во время раскладки пусты):
  // мемоизация держит окно, поэтому совпадение указателя значит то же самое окно.
  struct boat_memo {
    std::shared_ptr<const adventure> context;
    point centre;
    std::vector<point> doors, boat;
  };
  thread_local boat_memo boats;
  if (boats.context != d.context_handle || boats.centre.x != cave.centre.x || boats.centre.y != cave.centre.y ||
      boats.centre.z != cave.centre.z || !same(doors, boats.doors)) {
    boats.boat = doors.empty() ? std::vector<point>{} : boat_points_near(context, cave, 5.0, 45.0);
    boats.context = d.context_handle;
    boats.centre = cave.centre;
    boats.doors = std::move(doors);
  }
  const std::vector<point>& boat = boats.boat;
  // Внутри своей пещеры поле уже не больше порога — остальные слагаемые не нужны.
  const auto final_field = [&](const point p) {
    double field = local_cave_field(context.seed, cave, context.clearance_radius, p);
    if (field <= 1.0) return field;
    for (const auto& other : d.older) field = std::min(field, local_cave_field(context.seed, *other, context.clearance_radius, p));
    return field <= 1.0 ? field : std::min(field, (*d.base)(p));
  };
  const std::vector<point> targets = nest_targets(cave.rooms[target]);
  for (size_t t = 0; t < 4; ++t) {
    for (const point& from : boat) {
      // Шаг 1 м при пороге 1 м только строже: тонкую перемычку генератор может пропустить и
      // отвергнуть вариант, но не принять видимое гнездо.
      if (!sight_blocked(final_field, from, targets[t], 1.0, 1.0)) { ++d.why[refused_sight]; return false; }
    }
  }
  return true;
}

void tool_cave_fits(const originator::tool_call& call, const size_t, const size_t) {
  design_context d = resolve_design(call);
  d.base = &natural_of();
  read_skeleton(call, d.cave);
  const auto from_rooms = size_t(std::max<int64_t>(call.params->integer("from_rooms"), 0));
  const auto from_links = size_t(std::max<int64_t>(call.params->integer("from_links"), 0));
  if (from_rooms > d.cave.rooms.size() || from_links > d.cave.links.size()) {
    devils_engine::utils::error{}("GN09 step '{}': cave_fits checks from room {} and link {} of {} and {}", call.step_name,
                                  from_rooms, from_links, d.cave.rooms.size(), d.cave.links.size());
  }
  call.output(0).write().set(0, part_fits(d, from_rooms, from_links) ? 1.0 : 0.0);
  add_stats(call, d);
}

void tool_cave_hidden(const originator::tool_call& call, const size_t, const size_t) {
  design_context d = resolve_design(call);
  d.base = &natural_of();
  read_skeleton(call, d.cave);
  const int64_t target = call.params->integer("target", -1);
  if (target < 1 || size_t(target) >= d.cave.rooms.size()) {
    devils_engine::utils::error{}("GN09 step '{}': cave_hidden target {} is not a room of the cave", call.step_name, target);
  }
  call.output(0).write().set(0, target_hidden(d, uint32_t(target)) ? 1.0 : 0.0);
  add_stats(call, d);
}

// Вдоль каждого луча (откуда, направление) от from до to с шагом step: первая точка, где природное
// поле — порода (want = rock) или вода (want = water). Ответ — пройденное расстояние или -1.
void add_cave_tools(originator::tool_registry& tools) {
  using originator::aperture;
  tools.add(originator::tool_description{.name = "cave_fits", .shape = aperture::sequential, .input_count = 15,
                                         .output_count = 1, .body = tool_cave_fits,
                                         .footprint = originator::no_temporary_memory});
  tools.add(originator::tool_description{.name = "cave_hidden", .shape = aperture::sequential, .input_count = 15,
                                         .output_count = 1, .body = tool_cave_hidden,
                                         .footprint = originator::no_temporary_memory});
}

originator::instance_key place_key(const point centre, const uint64_t node_key) {
  const auto home = sector_of(centre);
  return {{uint64_t(int64_t(home[0])), uint64_t(int64_t(home[1])), node_key}};
}

// site пещеры — представление экземпляра генератора cave: узел места из окна его канонической
// окрестности (по ключу экземпляра через цепочку), скелет — из выходов rooms, links, pillars, state.
// Нет пещеры — одна полость узла, как до раскладки.
std::shared_ptr<const site> make_site(const originator::generator_result& instance, const double clearance_radius) {
  auto& chain = world_chain(clearance_radius);
  const auto& key = instance.key();
  const auto context = chain.get("context", instance.seed(), {{key.words[0], key.words[1]}});
  const auto corner = context->field("canonical", "corner"), size = context->field("canonical", "size");
  const auto world = world_view(*chain.get("window", instance.seed(), {{originator::read_key_word(corner, 0, 0),
                                                                        originator::read_key_word(corner, 0, 1),
                                                                        originator::read_key_word(size, 0, 0)}}));
  uint32_t node_id = 0;
  while (node_id < world->route.nodes.size() && world->route.nodes[node_id].key != key.words[2]) ++node_id;
  if (node_id == world->route.nodes.size()) {
    devils_engine::utils::error{}("GN09: cave node {:x} is not in its window", key.words[2]);
  }
  const node& at = world->route.nodes[node_id];
  const double core = cave_core(clearance_radius);
  auto out = std::make_shared<site>();
  out->node_id = node_id;
  out->kind = at.kind;
  out->recipe = recipe_of(world->seed, at);
  out->centre = at.centre;
  const double main_radius = std::max(starter_sub_turn_radius, core + 0.5);
  out->rooms.push_back({at.centre, main_radius, "main_chamber"});
  bound_cave(*out, core);

  const auto state = [&](const std::string_view name) { return instance.field("state", name).get(0); };
  const bool built = state("built") != 0.0;
  if (built) {
    const auto& purposes = cave_purposes();
    const auto rooms = size_t(state("rooms")), links = size_t(state("links")), pillars = size_t(state("pillars"));
    const auto field = [&](const std::string_view buffer, const std::string_view name) {
      return instance.field(buffer, name);
    };
    const auto centre = field("rooms", "centre"), radius = field("rooms", "radius"), squash = field("rooms", "squash"),
               stretch = field("rooms", "stretch"), yaw = field("rooms", "yaw"), purpose = field("rooms", "purpose"),
               flags = field("rooms", "flags");
    out->rooms.clear();
    out->has_cave = true;
    for (size_t i = 0; i < rooms; ++i) {
      const auto code = size_t(purpose.get(i));
      if (code >= purposes.size()) devils_engine::utils::error{}("GN09: cave room {} has purpose {} beyond the list", i, code);
      room r{{centre.get(i, 0), centre.get(i, 1), centre.get(i, 2)}, radius.get(i), purposes[code], squash.get(i)};
      r.stretch = stretch.get(i);
      r.yaw = yaw.get(i);
      out->rooms.push_back(std::move(r));
      const auto bits = uint32_t(flags.get(i));
      if (bits & 1u) out->entrances.push_back(uint32_t(i));
      if (bits & 2u) out->plugs.push_back(uint32_t(i));
    }
    const auto a = field("links", "a"), b = field("links", "b"), width = field("links", "width"),
               roof = field("links", "roof"), floor = field("links", "floor"), bulge = field("links", "bulge"),
               bulge_at = field("links", "bulge_at");
    for (size_t l = 0; l < links; ++l) {
      room_link link{uint32_t(a.get(l)), uint32_t(b.get(l))};
      if (link.a >= rooms || link.b >= rooms) devils_engine::utils::error{}("GN09: cave link {} leaves the rooms", l);
      link.width = width.get(l);
      link.roof = roof.get(l);
      link.floor = floor.get(l);
      link.bulge = bulge.get(l);
      link.bulge_at = bulge_at.get(l);
      out->links.push_back(link);
    }
    const auto pillar_base = field("pillars", "base"), pillar_radius = field("pillars", "radius"),
               pillar_height = field("pillars", "height");
    for (size_t k = 0; k < pillars; ++k) {
      out->pillars.push_back({{pillar_base.get(k, 0), pillar_base.get(k, 1), pillar_base.get(k, 2)},
                              pillar_radius.get(k), pillar_height.get(k)});
    }
    out->mouth = uint32_t(state("mouth"));
    out->nest = uint32_t(state("nest"));
    out->lair = uint32_t(state("lair"));
    out->lair_kind = uint8_t(state("lair_kind"));
    out->radio_target = state("radio");
    out->gap_target = state("gap");
    if (out->rooms.empty() || out->rooms[0].purpose != "main_chamber") {
      devils_engine::utils::error{}("GN09: the cave layout must keep room 0 as the node chamber");
    }
    bound_cave(*out, core);
  }

  if (std::getenv("GN09_CAVE_DEBUG")) {
    design_stats stats;
    {
      std::lock_guard guard(stats_lock);
      if (const auto found = stats_by_cave.find({instance.seed(), key.words[0], key.words[1], key.words[2]}); found != stats_by_cave.end()) {
        stats = found->second;
        stats_by_cave.erase(found);
      }
    }
    std::fprintf(stderr, "cave %016llx %s: %s after %u tries (world %u older %u rock %u self %u sight %u), fits %.0f ms, sight %.0f ms\n",
                 (unsigned long long)at.key, name(out->recipe), built ? "built" : "NONE", uint32_t(state("tries")),
                 stats.why[0], stats.why[1], stats.why[2], stats.why[3], stats.why[4], stats.fits_ms, stats.sight_ms);
  }
  return out;
}

} // namespace

std::shared_ptr<const site> site_view(const originator::generator_result& instance, const double clearance_radius) {
  return instance.view<site>([clearance_radius](const originator::generator_result& made) {
    return make_site(made, clearance_radius);
  });
}

void add_world_tools(originator::tool_registry& tools) {
  tools.add_standard_tools();
  tools.add_skeleton_tools();
  tools.add_volume_tools();
  tools.add_graph_tools();
  add_hall_tools(tools);
  add_station_tools(tools);
  add_tunnel_tools(tools);
  add_surface_tools(tools);
  tools.add_field_tools();
  add_cave_tools(tools);
}

double cave_core(const double clearance_radius) { return clearance_radius + surface_guard; }

double cave_wall_relief(const uint64_t seed, const point position) {
  return 0.45 * (0.5 + 0.5 * value_noise(seed + 21, position, 2.2)) +
         0.35 * (0.5 + 0.5 * value_noise(seed + 22, position, 5.0));
}

point cave_warp(const uint64_t seed, const point position) {
  // По оси не больше amount / √3: длина сдвига не больше amount.
  constexpr double axis = cave_warp_amount / 1.7320508075688772;
  return {position.x + axis * value_noise(seed + 23, position, 8.0),
          position.y + axis * value_noise(seed + 24, position, 8.0),
          position.z + axis * value_noise(seed + 25, position, 8.0)};
}

double local_cave_field(const uint64_t seed, const site& local, const double clearance_radius,
                        const point position) {
  if (position.x < local.low.x || position.x > local.high.x || position.y < local.low.y ||
      position.y > local.high.y || position.z < local.low.z || position.z > local.high.z) return 1e9;
  return cave_field_prefix(seed, local, clearance_radius, position, local.rooms.size(), local.links.size());
}

double cave_field_prefix(const uint64_t seed, const site& local, const double clearance_radius,
                         const point position, const size_t room_count, const size_t link_count) {
  const room& main_chamber = local.rooms.front();
  double field = length(subtract(position, main_chamber.centre)) - main_chamber.radius;
  if (!local.has_cave) return field;
  // Без шума: у каждого куска нижняя оценка. Если вся пещера дальше 3.5, её слагаемое
  // заведомо больше фоновой скалы (3.46) и точное значение не нужно; вызывающие
  // сравнивают с порогами меньше метра.
  const double core = cave_core(clearance_radius);
  const size_t rooms = room_count;
  thread_local std::vector<double> bounds;
  bounds.resize(rooms + link_count);
  double nearest = 1e30;
  for (size_t i = 1; i < rooms; ++i) {
    const room& r = local.rooms[i];
    const double d = length(subtract(position, r.centre));
    double bound = d - core - cave_relief;
    if (is_chamber(r, core)) {
      const chamber_axes a = axes_of(r);
      bound = std::min(bound, shape_bound(d, std::max({a.along, a.vertical, a.across})));
    }
    bounds[i] = bound;
    nearest = std::min(nearest, bound);
  }
  for (size_t l = 0; l < link_count; ++l) {
    const room_link& link = local.links[l];
    const double d = segment_distance(position, local.rooms[link.a].centre, local.rooms[link.b].centre);
    double bound = d - core - cave_relief;
    if (link.width > 0.0) bound = std::min(bound, shape_bound(d, gallery_largest(link)));
    bounds[rooms + l] = bound;
    nearest = std::min(nearest, bound);
  }
  if (nearest > 3.5) return field;
  // Поле считается в сдвинутой шумом точке: оси петляют, сечения не круглые. Ядро —
  // радиусы шире на сдвиг, неровность только наружу; форма — с шумом в обе стороны.
  const double relief = cave_wall_relief(seed, position);
  const double noise = shape_noise_at(seed, position);
  const point warped = cave_warp(seed, position);
  const double guarantee = core + cave_warp_amount + relief;
  // Колонны: порода внутри форм ниш и ходов (но не гарантированного ядра — колонны
  // стоят в стороне от него). Положительно внутри колонны.
  double pillar = -1e30;
  for (const cave_pillar& c : local.pillars) {
    const double t = (position.y - c.base.y) / c.height;
    if (t < 0.0 || t > 1.0) continue;
    const double flare = 2.0 * t - 1.0;
    const double r = c.radius * (1.0 + 0.3 * flare * flare);
    pillar = std::max(pillar, r - std::hypot(position.x - c.base.x, position.z - c.base.z) + 0.4 * noise);
  }
  for (size_t i = 1; i < rooms; ++i) {
    if (bounds[i] >= field) continue;
    const room& r = local.rooms[i];
    field = std::min(field, length(subtract(warped, r.centre)) - guarantee);
    if (is_chamber(r, core)) field = std::min(field, std::max(chamber_shape(r, warped) - noise, pillar));
  }
  // Включая отрезок от центра узла до устья: он почти весь в воде полости, но у
  // косой стены воронка устья должна вместить проход.
  for (size_t l = 0; l < link_count; ++l) {
    if (bounds[rooms + l] >= field) continue;
    const room_link& link = local.links[l];
    const point a = local.rooms[link.a].centre, b = local.rooms[link.b].centre;
    field = std::min(field, segment_distance(warped, a, b) - guarantee);
    if (link.width > 0.0) field = std::min(field, std::max(gallery_shape(link, a, frame_of(a, b), warped) - noise, pillar));
  }
  return field;
}

double cave_depth(const site& local) {
  if (!local.has_cave || local.nest >= local.rooms.size()) return 0.0;
  return path_lengths(local)[local.nest];
}

double cave_tour_length(const site& local) {
  if (!local.has_cave) return 0.0;
  std::vector<uint32_t> stops;
  for (uint32_t i = 1; i < local.rooms.size(); ++i) if (local.rooms[i].purpose.starts_with("pocket:")) stops.push_back(i);
  if (stops.empty() && local.nest < local.rooms.size()) stops.push_back(local.nest);
  const cave_graph graph(local);
  return originator::skeleton_tour(graph.view(), local.mouth, stops);
}

double cave_room_depth(const site& local, const uint32_t room) {
  if (!local.has_cave || room >= local.rooms.size()) return 0.0;
  return path_lengths(local)[room];
}

std::vector<point> boat_points(const adventure& context, const uint32_t node_id, const double step) {
  site probe;
  probe.centre = context.route.nodes.at(node_id).centre;
  probe.rooms.push_back({probe.centre, 0.0, "mouth"});
  probe.entrances = {0};
  return boat_points_near(context, probe, step);
}

std::shared_ptr<const originator::generator_result> cave_instance(const adventure& world, const uint32_t node_id) {
  const node& at = world.route.nodes.at(node_id);
  return world_chain(world.clearance_radius).get("cave", world.seed, place_key(at.centre, at.key));
}

std::shared_ptr<const site> cave_for(const adventure& world, const uint32_t node_id) {
  return site_view(*cave_instance(world, node_id), world.clearance_radius);
}

std::string verify_cave_instance(const adventure& world, const uint32_t node_id) {
  const node& at = world.route.nodes.at(node_id);
  const auto key = place_key(at.centre, at.key);
  // Место видно из многих окон проверки; пересобирать его достаточно один раз.
  static std::mutex lock;
  static std::set<std::tuple<uint64_t, uint64_t, originator::instance_key>> done;
  {
    std::lock_guard guard(lock);
    if (!done.emplace(world.seed, std::bit_cast<uint64_t>(world.clearance_radius), key).second) return {};
  }
  if (auto differs = world_chain(world.clearance_radius).verify("cave", world.seed, key); !differs.empty()) {
    return differs;
  }
  // Два пути к строкам одного поля: из буферов окна (так их видит цепочка) и объектом хоста
  // (prepare_field) той же канонической окрестности. Формулы у них общие — словарь форм, — поэтому
  // сверка ловит порчу при записи и чтении буферов; значения обязаны совпасть БИТ В БИТ.
  const auto home = sector_of(at.centre);
  const auto window = world_chain(world.clearance_radius)
                        .get("window", world.seed, window_key({home[0] - 1, home[1] - 1, home[0] + 2, home[1] + 2}));
  const std::vector<originator::field_term_source> terms{
    {"gallery", {window}}, {"turn", {window}}, {"hall", {window}}};
  const originator::primitive_field natural(world_shapes(world.clearance_radius), terms, world.seed);
  originator::primitive_field::sampler samples(natural, 16.0);
  const adventure context = canonical_context(world.seed, at.centre, world.clearance_radius);
  field_cache reference(context, false, 16.0, false);
  for (uint32_t i = 0; i < 256; ++i) {
    const auto h = devils_engine::utils::mix(world.seed, at.key, i);
    const point p{at.centre.x + (devils_engine::utils::prng_normalize(h) - 0.5) * 2.0 * cave_extent,
                  at.centre.y + (devils_engine::utils::prng_normalize(devils_engine::utils::mix(h, 1)) - 0.5) * 60.0,
                  at.centre.z + (devils_engine::utils::prng_normalize(devils_engine::utils::mix(h, 2)) - 0.5) * 2.0 * cave_extent};
    const double library = samples({p.x, p.y, p.z}), host = reference(p);
    if (library != host) {
      return std::format("natural field from primitives {} differs from cave_density {} at ({}, {}, {})", library,
                         host, p.x, p.y, p.z);
    }
  }
  return {};
}

cave_check verify_cave(const adventure& world, const site& local) {
  cave_check out;
  if (!local.has_cave) return out;
  // Жёсткое: устройство пещеры. Мягкое: обещания глубины, скрытости и породы.
  if (local.mouth >= local.rooms.size() || local.nest >= local.rooms.size() || local.lair >= local.rooms.size() ||
      local.rooms[local.nest].purpose != "nest" || local.rooms[local.mouth].purpose != "mouth" || local.entrances.empty()) {
    out.refusal = "cave has no mouth, lair or nest";
    return out;
  }
  for (const uint32_t e : local.entrances) {
    if (e >= local.rooms.size() || !is_entrance(local.rooms[e])) { out.refusal = "cave entrance is not a mouth"; return out; }
  }
  for (const uint32_t plug : local.plugs) {
    if (plug >= local.rooms.size() || local.rooms[plug].purpose != "neck") { out.refusal = "cave plug is not a branch neck"; return out; }
  }
  if (resource_cave(local.recipe) &&
      std::count_if(local.rooms.begin(), local.rooms.end(), [](const room& r) { return r.purpose.starts_with("pocket:"); }) < 2) {
    out.refusal = "resource cave has fewer than two pockets";
    return out;
  }
  const std::vector<double> reach = path_lengths(local);
  for (size_t i = 1; i < reach.size(); ++i) {
    if (reach[i] < 0.0) { out.refusal = "cave room is not reachable from an entrance"; return out; }
  }
  // Глубина заплыва этой пещеры: монстр может встретиться где угодно, но не у самого
  // входа (не ближе 0.4 N); залежи и крупное гнездо — за радиосвязью (не ближе 3/4 N).
  if (reach[local.lair] < 0.4 * local.radio_target - 1e-6) out.soft |= cave_check::lair_shallow;
  const double goal = 0.75 * (local.radio_target + local.gap_target) - 1e-6;
  for (uint32_t i = 1; i < local.rooms.size(); ++i) {
    const bool is_goal = local.rooms[i].purpose.starts_with("pocket:") ||
                         (local.recipe == site_recipe::nest_cave && i == local.nest);
    if (is_goal && reach[i] < goal) out.soft |= cave_check::goal_shallow;
  }
  const adventure context = canonical_context(world.seed, local.centre, world.clearance_radius);
  field_cache final_field(context, true, 16.0);
  field_cache natural_field(context, false, 16.0);
  const auto field = [&](const point p) { return final_field(p); };
  // Гнездо: другие точки лодки и цели, чем у генератора, шаг луча 0.4 м.
  const std::vector<point> boat = boat_points_near(context, local, 5.5);
  const room& nest = local.rooms[local.nest];
  const double r = 0.3 * nest.radius;
  for (const point& target : {nest.centre, add(nest.centre, {r, -0.4 * r * nest.squash, r}),
                              add(nest.centre, {-r, 0.4 * r * nest.squash, -r})}) {
    for (const point& from : boat) {
      if (field(from) >= 0.0) continue; // лодка в породе не стоит
      if (!sight_blocked(field, from, target, 0.4)) {
        out.soft |= cave_check::nest_visible;
        break;
      }
    }
    if (out.soft & cave_check::nest_visible) break;
  }
  // Стены в природной породе: на 0.5 м за наибольшей стеной формы (внутри кольца
  // генератора, другие точки), кроме входов и мест, где рядом своя часть пещеры.
  const double core = cave_core(world.clearance_radius);
  std::vector<point> doors;
  for (const uint32_t e : local.entrances) doors.push_back(local.rooms[e].centre);
  const auto wall_point = [&](const point p) {
    for (const point& door : doors) if (length(subtract(p, door)) < 11.0) return true;
    if (local_cave_field(world.seed, local, world.clearance_radius, p) < 0.5) return true;
    if (natural_field(p) <= 0.0) {
      out.soft |= cave_check::wall_open;
      return false;
    }
    return true;
  };
  for (const room_link& link : local.links) {
    if (link.a == 0 || link.b == 0) continue;
    if (!for_each_ring_point(local, core, link, 0.5, 0.7, 6, 0.0, wall_point)) break;
  }
  // Постройки не вскрывают пещеру (жёстко): на границе гарантированного ядра итоговое
  // поле совпадает с природным.
  for (const room_link& link : local.links) {
    if (link.a == 0 || link.b == 0) continue;
    const point a = local.rooms[link.a].centre, b = local.rooms[link.b].centre;
    const gallery_frame f = frame_of(a, b);
    const int steps = std::max(1, int(std::ceil(f.span)));
    for (int s = 0; s <= steps; ++s) {
      const point c = add(a, scale(f.axis, f.span * s / steps));
      for (const point dir : {f.right, f.up, scale(f.right, -1.0), scale(f.up, -1.0)}) {
        const point p = add(c, scale(dir, core));
        bool near_door = false;
        for (const point& door : doors) near_door = near_door || length(subtract(p, door)) < core + cave_relief + 4.5;
        if (near_door) continue;
        if (field(p) < natural_field(p) - 1e-9) {
          out.refusal = "a structure breaks into the cave";
          return out;
        }
      }
    }
  }
  return out;
}

} // namespace gn09
