#include "cave.h"
#include "surface.h"
#include "station.h"
#include "tunnel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <mutex>
#include <set>
#include <tuple>

#include "local_structure.h"
#include "world_chain.h"
#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/originator/primitive_field.h"
#include "devils_engine/originator/skeleton.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"

namespace gn09 {

namespace originator = devils_engine::originator;

namespace {

point add(const point a, const point b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
point scale(const point a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const point a) { return std::sqrt(dot(a, a)); }
point normalized(const point a) { return scale(a, 1.0 / length(a)); }
point cross(const point a, const point b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Два направления поперёк оси.
std::array<point, 2> across(const point axis) {
  const point reference = std::abs(axis.y) < 0.85 ? point{0.0, 1.0, 0.0} : point{1.0, 0.0, 0.0};
  const point right = normalized(cross(axis, reference));
  return {right, cross(right, axis)};
}

bool is_entrance(const room& r) { return r.purpose == "mouth" || r.purpose == "entrance"; }

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

originator::instance_key place_key(const point centre, const uint64_t node_key) {
  const auto home = sector_of(centre);
  return {{uint64_t(int64_t(home[0])), uint64_t(int64_t(home[1])), node_key}};
}

// site пещеры — представление экземпляра генератора cave: узел места из окна его канонической
// окрестности (по ключу экземпляра через цепочку), скелет — из выходов rooms, links, state.
// Нет пещеры — одна полость узла.
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
  out->rooms.push_back({at.centre, std::max(starter_sub_turn_radius(), core + 0.5), "main_chamber"});

  const auto state = [&](const std::string_view name) { return instance.field("state", name).get(0); };
  if (state("built") != 0.0) {
    const auto& purposes = cave_purposes();
    const auto rooms = size_t(state("rooms")), links = size_t(state("links"));
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
               roof = field("links", "roof");
    for (size_t l = 0; l < links; ++l) {
      room_link link{uint32_t(a.get(l)), uint32_t(b.get(l))};
      if (link.a >= rooms || link.b >= rooms) devils_engine::utils::error{}("GN09: cave link {} leaves the rooms", l);
      link.width = width.get(l);
      link.roof = roof.get(l);
      out->links.push_back(link);
    }
    out->pillars = uint32_t(state("pillars"));
    out->mouth = uint32_t(state("mouth"));
    out->nest = uint32_t(state("nest"));
    out->lair = uint32_t(state("lair"));
    out->lair_kind = uint8_t(state("lair_kind"));
    out->radio_target = state("radio");
    out->scale = state("scale");
    out->deposits = uint32_t(state("deposits"));
    out->collect = state("collect");
    out->budget = state("budget");
    out->tour = state("tour");
    out->estimate = state("estimate");
    const auto throat_a = field("state", "throat_a"), throat_b = field("state", "throat_b");
    out->throat_a = {throat_a.get(0, 0), throat_a.get(0, 1), throat_a.get(0, 2)};
    out->throat_b = {throat_b.get(0, 0), throat_b.get(0, 1), throat_b.get(0, 2)};
    out->throat_core = state("throat_core");
    out->blocks_sub = state("blocks_sub") != 0.0;
    if (out->rooms.empty() || out->rooms[0].purpose != "main_chamber") {
      devils_engine::utils::error{}("GN09: the cave layout must keep room 0 as the node chamber");
    }
  }
  // Коробка скелета с запасом на формы — только для рисунка.
  out->low = out->high = out->centre;
  for (const room& r : out->rooms) {
    const double reach = r.radius + 4.0;
    out->low = {std::min(out->low.x, r.centre.x - reach), std::min(out->low.y, r.centre.y - reach),
                std::min(out->low.z, r.centre.z - reach)};
    out->high = {std::max(out->high.x, r.centre.x + reach), std::max(out->high.y, r.centre.y + reach),
                 std::max(out->high.z, r.centre.z + reach)};
  }
  return out;
}

// Канонический экземпляр окна места.
std::shared_ptr<const originator::generator_result> canonical_window(const uint64_t seed, const double clearance_radius,
                                                                      const point centre) {
  const auto home = sector_of(centre);
  return world_chain(clearance_radius).get("window", seed, window_key({home[0] - 1, home[1] - 1, home[0] + 2, home[1] + 2}));
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
  tools.add_field_tools();
}

double cave_core(const double clearance_radius) { return clearance_radius + surface_guard(); }

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

std::shared_ptr<const originator::generator_result> cave_instance(const adventure& world, const uint32_t node_id) {
  const node& at = world.route.nodes.at(node_id);
  return world_chain(world.clearance_radius).get("cave", world.seed, place_key(at.centre, at.key));
}

std::shared_ptr<const site> cave_for(const adventure& world, const uint32_t node_id) {
  return site_view(*cave_instance(world, node_id), world.clearance_radius);
}

std::vector<originator::result_ptr> hall_regions_for(const originator::result_ptr& window,
                                                      const double clearance) {
  auto& chain = world_chain(clearance);
  const auto area = chain.get("area", window->seed(), window->key());
  const auto keys = area->field("area_regions", "key");
  const auto count = size_t(area->field("area_counts", "regions").get(0));
  std::vector<originator::result_ptr> regions;
  regions.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    regions.push_back(chain.get("halls", window->seed(),
                    {{originator::read_key_word(keys, i, 0), originator::read_key_word(keys, i, 1), 0}}));
  }
  return regions;
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
  // Два пути к строкам одного поля: из буферов окна (так их видит цепочка) и хостом (prepare_field) той же
  // канонической окрестности. DS и хост могут по-разному сгруппировать операции double;
  // допускаем только погрешность округления, изменение формы на 0.1% такая проверка ловит.
  const auto window = canonical_window(world.seed, world.clearance_radius, at.centre);
  const auto regions = hall_regions_for(window, world.clearance_radius);
  const std::vector<originator::field_term_source> terms{
    {"gallery", {window}}, {"route", {window}}, {"lobe", {window}}, {"pocket", {window}}, {"turn", {window}},
    {"hall", regions}};
  const originator::primitive_field natural(world_shapes(world.clearance_radius), terms, world.seed);
  originator::primitive_field::sampler samples(natural, 16.0);
  const adventure context = canonical_context(world.seed, at.centre, world.clearance_radius);
  field_cache reference(context, false, 16.0, false);
  for (uint32_t i = 0; i < 256; ++i) {
    const auto h = devils_engine::utils::mix(world.seed, at.key, i);
    const point p{at.centre.x + (devils_engine::utils::prng_normalize(h) - 0.5) * 2.0 * cave_extent(),
                  at.centre.y + (devils_engine::utils::prng_normalize(devils_engine::utils::mix(h, 1)) - 0.5) * 60.0,
                  at.centre.z + (devils_engine::utils::prng_normalize(devils_engine::utils::mix(h, 2)) - 0.5) * 2.0 * cave_extent()};
    const double library = samples({p.x, p.y, p.z}), host = reference(p);
    if (std::abs(library - host) > 1e-10 * std::max({1.0, std::abs(library), std::abs(host)})) {
      return std::format("natural field from primitives {} differs from cave_density {} at ({}, {}, {})", library,
                         host, p.x, p.y, p.z);
    }
  }
  return {};
}

cave_check verify_cave(const adventure& world, const site& local) {
  cave_check out;
  if (!local.has_cave) return out;
  // Жёсткое: устройство пещеры.
  if (local.mouth >= local.rooms.size() || local.nest >= local.rooms.size() || local.lair >= local.rooms.size() ||
      local.rooms[local.nest].purpose != "nest" || local.rooms[local.mouth].purpose != "mouth" || local.entrances.empty()) {
    out.refusal = "cave has no mouth, lair or nest";
    return out;
  }
  if (local.recipe == site_recipe::nest_cave && local.lair != local.nest && local.rooms[local.lair].purpose != "lair") {
    out.refusal = "cave lair is not a lair";
    return out;
  }
  for (const uint32_t e : local.entrances) {
    if (e >= local.rooms.size() || !is_entrance(local.rooms[e])) { out.refusal = "cave entrance is not a mouth"; return out; }
  }
  for (const uint32_t plug : local.plugs) {
    if (plug >= local.rooms.size() || local.rooms[plug].purpose != "neck") { out.refusal = "cave plug is not a neck"; return out; }
  }
  const size_t pockets = size_t(std::count_if(local.rooms.begin(), local.rooms.end(),
                                              [](const room& r) { return r.purpose.starts_with("pocket:"); }));
  if (local.recipe != site_recipe::nest_cave && pockets != local.deposits) {
    out.refusal = "resource cave pockets do not match its deposits";
    return out;
  }
  const std::vector<double> reach = path_lengths(local);
  double deepest = 0.0;
  for (size_t i = 1; i < reach.size(); ++i) {
    if (reach[i] < 0.0) { out.refusal = "cave room is not reachable from an entrance"; return out; }
    deepest = std::max(deepest, reach[i]);
  }

  // Мягкое (до полевых проверок: отказ ниже их не стирает): логово по пути (не у самого входа), бюджет
  // времени, гнездо глубоко и не у устья.
  if (reach[local.lair] < 0.4 * deepest - 1e-6) out.soft |= cave_check::lair_shallow;
  if (local.budget > 0.0 && std::abs(local.estimate - local.budget) > 0.5 * local.budget) out.soft |= cave_check::time_off;
  if (length(subtract(local.rooms[local.nest].centre, local.rooms[local.mouth].centre)) < 20.0 || reach[local.nest] < 25.0) {
    out.soft |= cave_check::nest_exposed;
  }

  // Поле окна и ЭТОЙ пещеры (то, что обещает генератор), и вода без неё — окно.
  const node& at = world.route.nodes.at(local.node_id);
  const auto window = canonical_window(world.seed, world.clearance_radius, at.centre);
  const auto regions = hall_regions_for(window, world.clearance_radius);
  const auto own = cave_instance(world, local.node_id);
  const originator::field_term_source own_terms[] = {
    {"gallery", {window}}, {"route", {window}}, {"lobe", {window}}, {"pocket", {window}}, {"turn", {window}},
    {"cave_gallery", {own}}, {"cave_lobe", {own}}, {"cave_turn", {own}}, {"cave_pillar", {own}}, {"hall", regions}};
  const originator::primitive_field own_field(world_shapes(world.clearance_radius), own_terms, world.seed);
  originator::primitive_field::sampler own_sample(own_field, 16.0);
  const auto field = [&](const point p) { return own_sample({p.x, p.y, p.z}); };
  // Чужая вода — окно (маршрут, залы). Пещеры соседних мест друг друга резать вправе.
  const originator::field_term_source other_terms[] = {
    {"gallery", {window}}, {"route", {window}}, {"lobe", {window}}, {"pocket", {window}}, {"turn", {window}}, {"hall", regions}};
  const originator::primitive_field other_field(world_shapes(world.clearance_radius), other_terms, world.seed);
  originator::primitive_field::sampler other_sample(other_field, 16.0);

  // Ныряльщик проходит каждый ход: на оси и в четырёх точках на R + √3 от неё — вода (сетка 1 м не
  // ставит треугольник в сферу R). Отрезок из полости узла до устья — вода полости, его не проверяем.
  const double probe = world.clearance_radius + std::sqrt(3.0);
  bool runs_into_other = false;
  for (const room_link& link : local.links) {
    if (link.a == 0 || link.b == 0) continue;
    const point a = local.rooms[link.a].centre, b = local.rooms[link.b].centre;
    const double span = length(subtract(b, a));
    const point axis = scale(subtract(b, a), 1.0 / span);
    const auto side = across(axis);
    const int steps = std::max(1, int(std::ceil(span)));
    for (int s = 0; s <= steps; ++s) {
      const point c = add(a, scale(axis, span * s / steps));
      for (const point p : {c, add(c, scale(side[0], probe)), add(c, scale(side[0], -probe)), add(c, scale(side[1], probe)),
                            add(c, scale(side[1], -probe))}) {
        if (field(p) >= 0.0) {
          out.refusal = std::format("a diver does not pass the cave at ({}, {}, {})", p.x, p.y, p.z);
          return out;
        }
      }
      // Мягко: ось пещеры дальше горла в воде окна — пещера вскрыла галерею маршрута или зал.
      // Входы (устье и вторые входы) по замыслу выходят в воду окна.
      if (!is_entrance(local.rooms[link.a]) && !is_entrance(local.rooms[link.b]) && other_sample({c.x, c.y, c.z}) < 0.0) {
        runs_into_other = true;
      }
    }
  }
  if (runs_into_other) out.soft |= cave_check::other_water;

  // Лодка не проходит горло: ни в одном сечении узкой части нет круга радиуса корпуса из воды САМОЙ
  // пещеры (центры круга — сетка 0.5 м вокруг оси; горло вправе быть щелью — широким в одну сторону,
  // узким в другую). Чужая вода рядом с горлом — не горло: от неё скелет держит породу, а вскрытие
  // считается мягко выше.
  const originator::field_term_source cave_terms[] = {{"cave_gallery", {own}}, {"cave_lobe", {own}}, {"cave_turn", {own}}};
  const originator::primitive_field cave_field(world_shapes(world.clearance_radius), cave_terms, world.seed);
  originator::primitive_field::sampler cave_sample(cave_field, 16.0);
  const auto cave_only = [&](const point p) { return cave_sample({p.x, p.y, p.z}); };
  // Узкий отрезок [a, b]: ни в одном сечении нет круга радиуса корпуса из воды самой пещеры.
  const auto boat_fits = [&](const point a, const point b) -> std::string {
    const point span = subtract(b, a);
    const double along = length(span);
    if (along < 1.0) return "cave throat has no narrow part";
    const point axis = scale(span, 1.0 / along);
    const auto side = across(axis);
    const int steps = std::max(1, int(std::ceil(along)));
    std::array<point, 12> ring{};
    for (int k = 0; k < 12; ++k) {
      const double angle = 2.0 * 3.14159265358979323846 * k / 12.0;
      ring[size_t(k)] = add(scale(side[0], cave_hull_radius() * std::cos(angle)), scale(side[1], cave_hull_radius() * std::sin(angle)));
    }
    for (int s = 0; s <= steps; ++s) {
      const point c = add(a, scale(axis, along * s / steps));
      for (double u = -10.0; u <= 10.0; u += 0.5) {
        for (double v = -10.0; v <= 10.0; v += 0.5) {
          const point centre = add(c, add(scale(side[0], u), scale(side[1], v)));
          if (cave_only(centre) >= 0.0) continue;
          bool open = true;
          for (const point& r : ring) {
            if (cave_only(add(centre, r)) >= 0.0) { open = false; break; }
          }
          if (open) return std::format("the boat fits into the cave throat at ({}, {}, {})", centre.x, centre.y, centre.z);
        }
      }
    }
    return {};
  };
  if (local.blocks_sub) {
    if (auto refusal = boat_fits(local.throat_a, local.throat_b); !refusal.empty()) { out.refusal = refusal; return out; }
    // Вторые входы — тоже горла: от входа до огибающей ниши, откуда ход растёт.
    for (const room_link& link : local.links) {
      for (const auto& [e, inner] : {std::pair{link.a, link.b}, std::pair{link.b, link.a}}) {
        if (e == local.mouth || inner == 0 || local.rooms[e].purpose != "entrance") continue;
        const point a = local.rooms[e].centre, b = local.rooms[inner].centre;
        const double span = length(subtract(b, a)), stop = local.rooms[inner].radius + 4.0;
        if (span <= stop + 1.0) { out.refusal = "a second cave entrance has no narrow part"; return out; }
        const point end = add(a, scale(subtract(b, a), (span - stop) / span));
        if (auto refusal = boat_fits(a, end); !refusal.empty()) { out.refusal = "second entrance: " + refusal; return out; }
      }
    }
  }
  return out;
}

} // namespace gn09
