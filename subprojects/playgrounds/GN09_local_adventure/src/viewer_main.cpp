#include <exception>
#include "devils_engine/playground/cli.h"
#include "adventure.h"
#include "local_structure.h"
#include "surface.h"
#include "station.h"
#include "tunnel.h"
#include "cave.h"
#include "world_chain.h"

#include <algorithm>
#include <atomic>
#include <thread>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "devils_engine/playground/world_memory.h"
#include "devils_engine/playground/chunk_viewer.h"
#include "devils_engine/utils/prng.h"

namespace {

struct options {
  uint64_t seed = 0;
  double radius = 1.0;
  uint32_t frames = 0;
  double flight = 0.0;
  // Все аппаратные потоки, кроме главного и рендера: генерация чанков — основная нагрузка полёта.
  uint32_t workers = std::max(1u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1u);
  // Опережение: 6 слоёв чанков впереди окна по ходу (зависимости, не поля).
  uint32_t prefetch = 6;
  // Гарантия: камера не входит в чанк, пока куб 5x5x5 вокруг него не построен.
  int guard = 2;
  uint32_t mode = 0;
  bool grid = false;
  bool validation = false;
  bool site_exterior = false;
  bool site_roof = false;
  std::string dump;
  std::optional<uint32_t> start_edge;
  bool cave_lair = false; // --start-cave: из хода на логово, где первая встреча
  bool cave_hub = false; // --start-cave: из зала пещеры, а не от лодки
  bool site_window = false;
  std::optional<uint32_t> hall_outpost; // N-й аванпост залов стартового окна
  std::optional<uint32_t> start_tunnel; // N-й туннель стартового окна, взгляд из тамбура
  std::optional<uint32_t> start_site;
  std::optional<uint32_t> start_cave;
  std::optional<uint32_t> start_exit;
  std::optional<uint32_t> start_hall;
  bool hall_entrance = false;
  std::optional<uint32_t> hall_niche;
  int sector_x = 0, sector_z = 0;
  int window_horizontal = 4, window_vertical = 4;
  // Атмосфера: по умолчанию тёмная вода; `--surface` возвращает солнечный вид GN03.
  bool surface = false;
  uint32_t lines = 1; // 0 — без графа, 1 — с глубиной, 2 — и сквозь стены
  float fog = 0.0f, fog_density = 0.012f, lamp = 1.8f, ambient = 1.5f;
};



std::optional<int> parse(const int argc, char** argv, options& result) {
  CLI::App app{"GN09 local adventure viewer"};
  app.option_defaults()->always_capture_default();
  app.add_flag("--grid", result.grid, "grid");
  app.add_flag("--validation", result.validation, "validation");
  app.add_flag("--site-exterior", result.site_exterior, "site-exterior");
  app.add_flag("--site-roof", result.site_roof, "site-roof");
  app.add_flag("--site-window", result.site_window, "site-window");
  app.add_flag("--cave-hub", result.cave_hub, "cave-hub");
  app.add_flag("--cave-lair", result.cave_lair, "cave-lair");
  app.add_flag("--hall-entrance", result.hall_entrance, "hall-entrance");
  app.add_flag("--surface", result.surface, "surface");
  app.add_option("--seed", result.seed, "World seed");
  app.add_option("--radius", result.radius, "Clearance radius")->check(devils_engine::playground::finite_number)->check(CLI::Range(0.5, 3.0));
  app.add_option("--frames", result.frames, "Frame limit");
  app.add_option("--flight", result.flight, "Flight speed")->check(devils_engine::playground::finite_number)->check(CLI::NonNegativeNumber);
  app.add_option("--workers", result.workers, "Workers")->check(CLI::PositiveNumber);
  app.add_option("--prefetch", result.prefetch, "Prefetch layers");
  app.add_option("--guard", result.guard, "Guard radius");
  app.add_option("--view", result.mode, "Debug view")->check(CLI::Range(0, 3));
  app.add_option("--lines", result.lines, "Graph lines")->check(CLI::Range(0, 2));
  app.add_option("--fog", result.fog, "Fog")->check(devils_engine::playground::finite_number)->check(CLI::NonNegativeNumber);
  app.add_option("--fog-density", result.fog_density, "Fog density")->check(devils_engine::playground::finite_number)->check(CLI::NonNegativeNumber);
  app.add_option("--lamp", result.lamp, "Lamp")->check(devils_engine::playground::finite_number)->check(CLI::NonNegativeNumber);
  app.add_option("--ambient", result.ambient, "Ambient")->check(devils_engine::playground::finite_number)->check(CLI::NonNegativeNumber);
  app.add_option("--dump", result.dump, "Frame output");
  app.add_option("--start-edge", result.start_edge, "Start view index");
  app.add_option("--start-tunnel", result.start_tunnel, "Start view index");
  app.add_option("--hall-outpost", result.hall_outpost, "Start view index");
  app.add_option("--start-site", result.start_site, "Start view index");
  app.add_option("--start-cave", result.start_cave, "Start view index");
  app.add_option("--hall-niche", result.hall_niche, "Start view index");
  app.add_option("--start-hall", result.start_hall, "Start view index");
  app.add_option("--start-exit", result.start_exit, "Start view index");
  std::string mods;
  app.add_option("--mods", mods, "Ordered mod folders");
  std::array<int, 2> sector{result.sector_x, result.sector_z};
  std::array<int, 2> window{result.window_horizontal, result.window_vertical};
  devils_engine::playground::add_tuple(app, "--start-sector", sector, ',');
  devils_engine::playground::add_tuple(app, "--window", window, ',', [](const auto& v) { return v[0] >= 1 && v[0] <= 16 && v[1] >= 1 && v[1] <= 16; });
  if (auto code = devils_engine::playground::parse_cli(app, argc, argv)) return code;
  result.sector_x = sector[0]; result.sector_z = sector[1];
  result.window_horizontal = window[0]; result.window_vertical = window[1];
  if (app.count("--mods")) gn09::set_world_mods(mods);
  return std::nullopt;
}

// ГРАФ МАРШРУТОВ ТОНКИМИ ЛИНИЯМИ во viewer: рёбра, крестики узлов по теме, залы с петлёй
// патруля и нишами. Строится только граф и залы вокруг камеры — планы баз линиям не нужны, поэтому
// источник дешевле окна чанков. Это диагностика: ничего не вырезает и не проверяет.
uint32_t rgba(const double r, const double g, const double b) {
  const auto byte = [](const double v) { return uint32_t(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
  return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (255u << 24);
}

uint32_t theme_colour(const gn09::theme kind) {
  switch (kind) {
    case gn09::theme::outpost: return rgba(0.37, 0.77, 0.42);
    case gn09::theme::derelict: return rgba(0.77, 0.41, 0.25);
    case gn09::theme::grotto: return rgba(0.31, 0.61, 0.88);
    case gn09::theme::vent: return rgba(0.88, 0.63, 0.23);
    default: return rgba(0.62, 0.66, 0.70);
  }
}

uint32_t resident_colour(const gn09::creature_class resident) {
  switch (resident) {
    case gn09::creature_class::lord: return rgba(0.85, 0.31, 0.82);
    case gn09::creature_class::beast: return rgba(0.91, 0.53, 0.23);
    default: return rgba(0.44, 0.72, 0.79);
  }
}

glm::dvec3 vec(const gn09::point p) { return {p.x, p.y, p.z}; }

glm::dvec3 hall_point(const gn09::hall& h, const double u, const double y, const double v) {
  const glm::dvec3 forward{h.forward.x, 0.0, h.forward.z};
  const glm::dvec3 right{-h.forward.z, 0.0, h.forward.x};
  return vec(h.centre) + forward * u + right * v + glm::dvec3(0.0, y, 0.0);
}

std::vector<devils_engine::gn03::debug_line> graph_lines(const uint64_t seed, const glm::dvec3 observer) {
  using devils_engine::gn03::debug_line;
  static constexpr double reach = 480.0;
  const auto sector = [](const double v) { return int(std::floor(v / gn09::sector_size())); };
  const gn09::graph::window area{sector(observer.x - reach), sector(observer.z - reach),
                                 sector(observer.x + reach) + 1, sector(observer.z + reach) + 1};
  const gn09::graph route = gn09::generate_graph(seed, area);
  std::vector<debug_line> lines;
  const uint32_t edge_colour = rgba(0.47, 0.78, 0.86);
  for (const gn09::edge& e : route.edges)
    lines.push_back({vec(route.nodes[e.a].centre), vec(route.nodes[e.b].centre), edge_colour});
  for (const gn09::node& n : route.nodes) {
    const double half = gn09::is_base(n.kind) ? 5.0 : n.kind == gn09::theme::empty ? 1.5 : 3.0;
    const glm::dvec3 c = vec(n.centre);
    const uint32_t colour = theme_colour(n.kind);
    for (int axis = 0; axis < 3; ++axis) {
      glm::dvec3 d{};
      d[axis] = half;
      lines.push_back({c - d, c + d, colour});
    }
  }
  for (const gn09::hall& h : gn09::halls_near(seed, area)) {
    const uint32_t colour = resident_colour(h.resident);
    const auto closed = [&](const std::vector<glm::dvec3>& points, const uint32_t c) {
      for (size_t i = 0; i < points.size(); ++i)
        lines.push_back({points[i], points[(i + 1) % points.size()], c});
    };
    std::vector<glm::dvec3> loop;
    for (const gn09::point& p : h.loop) loop.push_back(hall_point(h, p.x, p.y, p.z));
    closed(loop, colour);
    // Горизонтальное сечение эллипсоида на высоте центра и продольное вертикальное, тусклее.
    const uint32_t outline = rgba(0.30, 0.42, 0.46);
    std::vector<glm::dvec3> flat, upright;
    for (int i = 0; i < 64; ++i) {
      const double a = i * 6.283185307179586 / 64.0;
      flat.push_back(hall_point(h, h.half_length * std::cos(a), 0.0, h.half_width * std::sin(a)));
      upright.push_back(hall_point(h, h.half_length * std::cos(a), h.half_height * std::sin(a), 0.0));
    }
    closed(flat, outline);
    closed(upright, outline);
    for (const gn09::hall_niche& niche : h.niches) {
      const uint32_t c = niche.overhang ? rgba(0.95, 0.82, 0.42) : rgba(0.62, 0.83, 0.42);
      const glm::dvec3 mouth = hall_point(h, niche.mouth.x, niche.mouth.y, niche.mouth.z);
      const glm::dvec3 elbow = hall_point(h, niche.elbow.x, niche.elbow.y, niche.elbow.z);
      const glm::dvec3 end = hall_point(h, niche.end.x, niche.end.y, niche.end.z);
      lines.push_back({mouth, elbow, c});
      lines.push_back({elbow, end, c});
    }
  }
  return lines;
}

// Зерно мира, которое сейчас строит просмотрщик: опережение готовит зависимости именно этого мира.
std::atomic<uint64_t> current_seed{0};

devils_engine::gn03::factory_builder make_builder(const double radius) {
  return [radius](const std::vector<std::pair<std::string, double>>&, const uint64_t seed) {
    current_seed = seed;
    return [radius, seed]() -> devils_engine::gn03::chunk_generator {
      // Чанк — экземпляр генератора chunk_field цепочки (окно 5x5 вокруг его сектора, пещеры мест
      // fan-in, поле `solid`). Кэш экземпляров общий для всех потоков потока чанков, поэтому соседние
      // чанки делят окно и пещеры, а мир не зависит от того, какой поток и в каком порядке их строит.
      return [radius, seed](const devils_engine::originator::chunk_key& key,
                            devils_engine::gn03::chunk_mesh& mesh) {
        using clock = std::chrono::steady_clock;
        const auto begin = clock::now();
        const gn09::chunk_instance made = gn09::chunk_from_chain(seed, radius, int(key.x), int(key.y), int(key.z));
        const auto field_done = clock::now();
        // Рельеф и постройки — выходы того же экземпляра chunk_field (марширующие тетраэдры библиотеки и
        // блок structure_surface).
        const gn09::surface_mesh surface = gn09::surface_from_chain(made);
        mesh.key = key;
        mesh.vertices.resize(surface.size());
        mesh.props.clear();
        mesh.biomes_used = 1;
        for (size_t i = 0; i < surface.size(); ++i) {
          const gn09::surface_vertex& source = surface[i];
          auto& target = mesh.vertices[i];
          const double p[3]{source.position.x, source.position.y, source.position.z};
          const double n[3]{source.normal.x, source.normal.y, source.normal.z};
          for (int axis = 0; axis < 3; ++axis) {
            target.position[axis] = devils_engine::gn03::encode_local_position(p[axis], gn09::chunk::cells);
            target.normal[axis] = int8_t(std::lround(std::clamp(n[axis], -1.0, 1.0) * 127.0));
          }
          target.chunk = 0;
          target.padding = source.shade;
        }
        // Предметы — выход props того же экземпляра chunk_field: залежи пещер мест в коробке чанка; имя
        // стабильно от ключа узла и номера предмета в его пещере.
        const double origin[3]{double(key.x * gn09::chunk::cells), double(key.y * gn09::chunk::cells),
                               double(key.z * gn09::chunk::cells)};
        const auto& kinds = gn09::cave_feature_kinds();
        const auto props = made.instance->field("props", "position");
        const auto prop_kind = made.instance->field("props", "kind"), prop_node = made.instance->field("props", "node"),
                   prop_index = made.instance->field("props", "index");
        for (size_t i = 0, count = size_t(made.instance->field("surface_counts", "props").get(0)); i < count; ++i) {
          devils_engine::gn03::chunk_prop prop;
          for (int axis = 0; axis < 3; ++axis) prop.position[axis] = props.get(i, uint32_t(axis)) - origin[axis];
          prop.normal[1] = 1.0;
          prop.size = 1.8;
          const std::string_view kind = kinds.at(size_t(prop_kind.get(i)));
          prop.kind = kind == "egg_cluster" || kind == "monster_group" ? 3u :
                      kind == "thermal_crystal" || kind == "ice_plug" ? 2u : 1u;
          prop.origin = uint32_t(devils_engine::utils::mix(seed, devils_engine::originator::read_key_word(prop_node, i),
                                                          uint64_t(prop_index.get(i)), 0x524553ull));
          mesh.props.push_back(prop);
        }
        const auto done = clock::now();
        mesh.field_milliseconds = std::chrono::duration<double, std::milli>(field_done - begin).count();
        mesh.surface_milliseconds = std::chrono::duration<double, std::milli>(done - field_done).count();
      };
    };
  };
}

} // namespace

int main(const int argc, char** argv) try {
  options cli;
  if (auto code = parse(argc, argv, cli)) return *code;
  const gn09::adventure start_world = gn09::generate(cli.seed,
    {cli.sector_x, cli.sector_z, cli.sector_x + 2, cli.sector_z + 2}, cli.radius);
  if (cli.start_edge && *cli.start_edge >= start_world.route.edges.size()) {
    std::cerr << "start edge is outside the starting graph\n";
    return 2;
  }
  if (cli.start_site && (*cli.start_site >= start_world.route.nodes.size() ||
                         !gn09::is_base(start_world.route.nodes[*cli.start_site].kind))) {
    std::cerr << "start site is not a built base in the starting graph\n";
    return 2;
  }
  if (cli.start_site && cli.start_edge) {
    std::cerr << "choose either start site or start edge\n";
    return 2;
  }
  if (cli.start_cave && (*cli.start_cave >= start_world.route.nodes.size() ||
      start_world.route.nodes[*cli.start_cave].kind == gn09::theme::empty ||
      gn09::is_base(start_world.route.nodes[*cli.start_cave].kind) ||
      cli.start_site || cli.start_edge || cli.site_exterior || cli.start_exit)) {
    std::cerr << "start cave must name a natural site and use no other start view\n";
    return 2;
  }
  if (cli.start_hall && (*cli.start_hall >= start_world.halls.size() || cli.start_site ||
                         cli.start_edge || cli.start_cave)) {
    std::cerr << "start hall must name one of the starting world's halls and no other start\n";
    return 2;
  }
  if (cli.site_exterior && !cli.start_site && !cli.hall_outpost) {
    std::cerr << "site exterior view requires --start-site\n";
    return 2;
  }
  if (cli.site_roof && ((!cli.start_site && !cli.hall_outpost) || cli.site_exterior || cli.start_exit)) {
    std::cerr << "roof view requires --start-site and no other site view\n";
    return 2;
  }
  if (cli.start_exit && (!cli.start_site || *cli.start_exit >= 2 || cli.site_exterior)) {
    std::cerr << "emergency exit view requires --start-site and --start-exit=0 or 1\n";
    return 2;
  }
  gn09::point start = start_world.route.nodes[start_world.route.entry].centre;
  gn09::point target;
  if (cli.start_tunnel) {
    std::vector<std::shared_ptr<const gn09::built_tunnel>> tunnels;
    for (const gn09::station_anchor& a : gn09::station_anchors(start_world)) {
      auto t = gn09::tunnel_for(start_world, a);
      if (t && std::none_of(tunnels.begin(), tunnels.end(), [&](const auto& o) { return o->key == t->key; })) {
        tunnels.push_back(std::move(t));
      }
    }
    if (*cli.start_tunnel >= tunnels.size()) {
      std::cerr << "the starting window has " << tunnels.size() << " tunnels\n";
      return 2;
    }
    // Из ноги первой станции вдоль тамбура и первого пролёта.
    const auto& pieces = tunnels[*cli.start_tunnel]->pieces;
    const gn09::tunnel_piece& leg = pieces.front();
    const double back = leg.hx + 2.0; // в помещении, в двух метрах перед проёмом
    start = {leg.centre.x - leg.x.x * back, leg.centre.y + 0.2, leg.centre.z - leg.x.z * back};
    const gn09::tunnel_piece& chamber = pieces[1];
    target = chamber.centre;
  } else if (cli.start_site || cli.hall_outpost) {
    std::shared_ptr<const gn09::built_station> built;
    std::string label;
    if (cli.start_site) {
      built = gn09::station_for(start_world, *cli.start_site);
      label = "base " + std::to_string(*cli.start_site);
    } else {
      std::vector<gn09::station_anchor> outposts;
      for (const gn09::station_anchor& a : gn09::station_anchors(start_world)) {
        if (a.hall_key != 0) outposts.push_back(a);
      }
      if (*cli.hall_outpost >= outposts.size()) {
        std::cerr << "the starting window has " << outposts.size() << " hall outposts\n";
        return 2;
      }
      built = gn09::station_for(start_world, outposts[*cli.hall_outpost]);
      label = "hall outpost " + std::to_string(*cli.hall_outpost);
    }
    if (!built) {
      std::cerr << label << " has no station\n";
      return 2;
    }
    const gn09::station_plan& plan = built->plan;
    const gn09::station_site& site = plan.site;
    const auto centre_of = [&](const gn09::station_room& r, const double y) {
      return gn09::station_to_world(site, (r.u0 + r.u1) * 0.25, y, (r.v0 + r.v1) * 0.25);
    };
    // Самое дальнее от шлюза помещение — туда смотрит камера изнутри.
    const gn09::station_room* far = &plan.rooms.back();
    for (const gn09::station_room& r : plan.rooms) {
      if (std::hypot(r.u0 + r.u1, r.v0 + r.v1) > std::hypot(far->u0 + far->u1, far->v0 + far->v1)) far = &r;
    }
    if (cli.site_window) {
      // Снаружи на окно скоса: первая стеклянная клетка, камера в воде напротив неё.
      const gn09::station_voxels& v = built->voxels;
      bool found = false;
      for (int32_t y = 0; y < v.height && !found; ++y) {
        for (int32_t k = 0; k < v.depth && !found; ++k) for (int32_t i = 0; i < v.width && !found; ++i) {
          const size_t index = (size_t(y) * v.depth + k) * v.width + i;
          if (!v.solid[index] || v.shade[index] != gn09::station_glass_shade) continue;
          target = gn09::station_to_world(site, (v.i0 + i + 0.5) * gn09::station_cell(),
                                          (v.y0 + y + 0.5) * gn09::station_cell(),
                                          (v.k0 + k + 0.5) * gn09::station_cell());
          // Наружу — от середины станции через окно, затем с отклонением.
          gn09::point mid{};
          for (const gn09::station_room& r : plan.rooms) {
            const gn09::point c = centre_of(r, 1.5);
            mid = {mid.x + c.x, mid.y + c.y, mid.z + c.z};
          }
          const double base_angle = std::atan2(target.z - mid.z * 1.0 / plan.rooms.size(),
                                               target.x - mid.x * 1.0 / plan.rooms.size());
          const auto outside_rooms = [&](const gn09::point p) {
            const double du = (p.x - site.origin.x) * std::cos(site.yaw) + (p.z - site.origin.z) * std::sin(site.yaw);
            const double dv = -(p.x - site.origin.x) * std::sin(site.yaw) + (p.z - site.origin.z) * std::cos(site.yaw);
            for (const gn09::station_room& r : plan.rooms) {
              if (du > (r.u0 - 3) * gn09::station_cell() && du < (r.u1 + 3) * gn09::station_cell() &&
                  dv > (r.v0 - 3) * gn09::station_cell() && dv < (r.v1 + 3) * gn09::station_cell()) return false;
            }
            return true;
          };
          for (const double distance : {7.0, 5.0, 9.0, 4.0}) {
            for (int a = 0; a < 9 && !found; ++a) {
              const double angle = base_angle + (a % 2 ? 1 : -1) * ((a + 1) / 2) * 0.25;
              const gn09::point candidate{target.x + distance * std::cos(angle), target.y + 1.0,
                                          target.z + distance * std::sin(angle)};
              if (outside_rooms(candidate) && gn09::density(start_world, candidate) < -1.0) {
                start = candidate;
                found = true;
              }
            }
            if (found) break;
          }
        }
      }
      if (!found) {
        std::cerr << label << " has no glazed bevel with open water in front\n";
        return 2;
      }
    } else if (cli.site_roof) {
      // Снаружи: открытая точка над станцией со стороны воды, взгляд на неё.
      const gn09::point middle = centre_of(plan.rooms[plan.rooms.size() / 2], 1.5);
      target = middle;
      start = gn09::point{site.berth.x, site.berth.y + 6.0, site.berth.z};
      for (const double lift : {6.0, 10.0, 4.0, 14.0}) {
        const gn09::point candidate{site.berth.x, site.berth.y + lift, site.berth.z};
        if (gn09::density(start_world, candidate) < -0.5) { start = candidate; break; }
      }
    } else if (cli.start_exit) {
      std::cerr << "stations have no emergency exits yet\n";
      return 2;
    } else if (cli.site_exterior) {
      // Со стороны полости, сбоку от дока: видны шлюз, коридор шлюза и тело у линии породы.
      const gn09::station_room& airlock = plan.rooms[0];
      const gn09::point lock = centre_of(airlock, 1.5);
      target = gn09::point{(lock.x + site.origin.x) * 0.5, (lock.y + site.origin.y + 1.5) * 0.5,
                           (lock.z + site.origin.z) * 0.5};
      start = site.berth;
      const gn09::point side{-std::sin(site.yaw), 0.0, std::cos(site.yaw)};
      for (const double shift : {14.0, -14.0, 9.0, -9.0, 5.0}) {
        const gn09::point candidate{site.node_centre.x + side.x * shift, (site.node_centre.y + lock.y) * 0.5 + 3.0,
                                    site.node_centre.z + side.z * shift};
        if (gn09::density(start_world, candidate) < -1.0) { start = candidate; break; }
      }
    } else {
      // Изнутри: из первой комнаты за шлюзом через её дальнюю дверь в следующую.
      const gn09::station_door& door = plan.doors.size() > 1 ? plan.doors[1] : plan.doors[0];
      const gn09::station_room& from = plan.rooms[door.a];
      const gn09::station_room& into = plan.rooms[door.b];
      const gn09::point gap = gn09::station_to_world(site, (door.u0 + door.u1) * 0.25, 1.6,
                                                     (door.v0 + door.v1) * 0.25);
      const gn09::point room = centre_of(from, 1.6);
      start = gn09::point{room.x + (room.x - gap.x) * 0.6, 1.6 + site.origin.y, room.z + (room.z - gap.z) * 0.6};
      if (gn09::station_room_quarters(from, int32_t(std::floor(((start.x - site.origin.x) * std::cos(site.yaw) +
          (start.z - site.origin.z) * std::sin(site.yaw)) / gn09::station_cell())),
          int32_t(std::floor((-(start.x - site.origin.x) * std::sin(site.yaw) +
          (start.z - site.origin.z) * std::cos(site.yaw)) / gn09::station_cell()))) != 0xF) start = room;
      target = centre_of(into, 1.6);
      (void)far;
    }
    std::cout << "GN09 station " << gn09::name(site.pose) << " with " << plan.rooms.size()
              << " rooms\n";
  } else if (cli.start_hall) {
    // Внутри зала на оси коридора существ, взгляд вдоль зала.
    const gn09::hall& h = start_world.halls[*cli.start_hall];
    const auto at = [&](const double u, const double y) {
      return gn09::point{h.centre.x + u * h.forward.x, h.centre.y + y, h.centre.z + u * h.forward.z};
    };
    // На петле патруля, взгляд вдоль неё.
    const gn09::point right{-h.forward.z, 0, h.forward.x};
    const auto loop_point = [&](const gn09::point l) {
      return gn09::point{h.centre.x + l.x * h.forward.x + l.z * right.x, h.centre.y + l.y,
                         h.centre.z + l.x * h.forward.z + l.z * right.z};
    };
    start = loop_point(h.loop[0]);
    target = loop_point(h.loop[1]);
    (void)at;
    if (cli.hall_niche && *cli.hall_niche < h.niches.size()) {
      // Из зала на вход ниши: отступ от входа по нормали стены внутрь зала.
      const gn09::point right{-h.forward.z, 0, h.forward.x};
      const auto world = [&](const gn09::point local) {
        return gn09::point{h.centre.x + local.x * h.forward.x + local.z * right.x,
                           h.centre.y + local.y,
                           h.centre.z + local.x * h.forward.z + local.z * right.z};
      };
      const gn09::hall_niche& niche = h.niches[*cli.hall_niche];
      const gn09::point inward{niche.mouth.x - niche.elbow.x, niche.mouth.y - niche.elbow.y,
                               niche.mouth.z - niche.elbow.z};
      const double span = std::sqrt(inward.x * inward.x + inward.y * inward.y + inward.z * inward.z);
      start = world({niche.mouth.x + inward.x / span * 35.0, niche.mouth.y + inward.y / span * 35.0 + 6.0,
                     niche.mouth.z + inward.z / span * 35.0});
      target = world(niche.elbow);
      std::cout << "GN09 niche " << *cli.hall_niche << (niche.overhang ? " under an overhang" : "") << '\n';
    } else if (cli.hall_entrance) {
      // Снаружи у входа маршрута, взгляд вдоль ребра внутрь зала.
      const auto entrances = gn09::hall_entrances(cli.seed, h);
      if (!entrances.empty()) {
        start = entrances.front().first;
        target = entrances.front().second;
      }
    }
    std::cout << "GN09 hall " << *cli.start_hall << ' ' << gn09::name(h.kind) << ' '
              << gn09::name(h.resident) << " body r=" << h.body_radius << " half="
              << h.half_length << 'x' << h.half_width << 'x' << h.half_height << " m\n";
  } else if (cli.start_cave) {
    // Из полости узла, где стоит лодка, в устье пещеры; --cave-hub — из расширенного
    // зала в первую ветку.
    const gn09::site local = gn09::materialize_site(start_world, *cli.start_cave);
    if (!local.has_cave) {
      std::cerr << "site " << *cli.start_cave << " has no diver cave\n";
      return 2;
    }
    const gn09::point mouth = local.rooms[local.mouth].centre;
    if (cli.cave_lair) {
      // Из соседней по связи комнаты хода на логово.
      uint32_t from = local.mouth;
      for (const gn09::room_link& link : local.links) {
        if (link.b == local.lair && link.a != 0) from = link.a;
        if (link.a == local.lair && link.b != 0 && from == local.mouth) from = link.b;
      }
      start = local.rooms[from].centre;
      target = local.rooms[local.lair].centre;
    } else if (cli.cave_hub) {
      uint32_t hub = 0;
      for (uint32_t i = 0; i < local.rooms.size(); ++i) {
        if (hub == 0 && local.rooms[i].purpose == "chamber") hub = i; // первая крупная ниша
      }
      start = local.rooms[hub].centre;
      target = local.rooms[hub + 1].centre; // горловина первой ветки
    } else {
      start = {local.centre.x + (mouth.x - local.centre.x) * 0.35, local.centre.y + (mouth.y - local.centre.y) * 0.35,
               local.centre.z + (mouth.z - local.centre.z) * 0.35};
      target = mouth;
    }
  } else if (cli.start_edge) {
    const gn09::edge link = start_world.route.edges[*cli.start_edge];
    const gn09::point a = start_world.route.nodes[link.a].centre;
    target = start_world.route.nodes[link.b].centre;
    start = {(a.x + target.x) / 2.0, (a.y + target.y) / 2.0,
             (a.z + target.z) / 2.0};
  } else {
    for (const gn09::edge link : start_world.route.edges) {
      if (link.a != start_world.route.entry && link.b != start_world.route.entry) continue;
      target = start_world.route.nodes[link.a == start_world.route.entry ? link.b : link.a].centre;
      break;
    }
  }
  devils_engine::gn03::world_memory memory;
  devils_engine::gn03::viewer_options viewer;
  viewer.window_title = "GN09 — caves and outposts";
  viewer.overlay_title = "GN09 local adventure";
  viewer.overlay_summary = "Caves, built outposts, 34 x 12 m capsule route; GN03 chunk viewer";
  viewer.overlay_controls = "WASD/QE fly | Shift fast | M view | G chunk grid | L graph | N new seed | U overlay | Esc quit";
  viewer.seed = cli.seed;
  viewer.chunk_cells = gn09::chunk::cells;
  viewer.cell_size = 1.0;
  viewer.window.horizontal_radius = cli.window_horizontal;
  viewer.window.vertical_radius = cli.window_vertical;
  // Подводное восприятие: туман доходит до полной плотности за чанк до края окна по любой оси,
  // фон ровно цвета воды — подгрузка чанков не видна. Дальше видеть — шире окно (`--window`).
  viewer.underwater = !cli.surface;
  viewer.rock_palette = true;
  viewer.fog_distance = cli.fog;
  viewer.fog_density = cli.fog_density;
  viewer.lamp = cli.lamp;
  viewer.ambient = cli.ambient;
  // Граф маршрутов тонкими линиями; L переключает: выкл / с глубиной / сквозь стены.
  viewer.lines = graph_lines;
  viewer.lines_mode = cli.lines;
  // Окно больше стартового 7^3 нужно залам; арена растёт вместе с объёмом окна.
  const size_t window_chunks = size_t(2 * cli.window_horizontal + 1) *
                               size_t(2 * cli.window_horizontal + 1) *
                               size_t(2 * cli.window_vertical + 1);
  viewer.arena_vertices = std::max<size_t>(2u << 20, window_chunks * 6000);
  viewer.workers = cli.workers;
  // Опережение: зависимости чанков впереди по ходу (окна, места, пещеры, станции, туннели) — всё,
  // кроме самого поля чанка.
  viewer.prefetch_layers = cli.prefetch;
  viewer.guard_radius = cli.guard;
  viewer.prefetch = [radius = cli.radius](const devils_engine::originator::chunk_key& key) {
    gn09::world_chain(radius).prefetch("chunk_field", current_seed.load(),
                                       {{uint64_t(key.x), uint64_t(key.y), uint64_t(key.z)}});
  };
  viewer.flight_speed = cli.flight;
  viewer.frames = cli.frames;
  viewer.mode = cli.mode;
  viewer.grid = cli.grid;
  viewer.validation = cli.validation;
  viewer.dump_path = cli.dump;
  viewer.memory = &memory;
  viewer.start = {int64_t(std::floor(start.x / gn09::chunk::cells)),
                  int64_t(std::floor(start.y / gn09::chunk::cells)),
                  int64_t(std::floor(start.z / gn09::chunk::cells))};
  viewer.start_offset_valid = true;
  viewer.start_offset = {start.x - double(viewer.start.x * gn09::chunk::cells),
                         start.y - double(viewer.start.y * gn09::chunk::cells),
                         start.z - double(viewer.start.z * gn09::chunk::cells)};
  const double dx = target.x - start.x, dy = target.y - start.y, dz = target.z - start.z;
  std::cout << "GN09 start biome=" << gn09::name(gn09::biome_at(cli.seed, start))
            << " sector=" << cli.sector_x << ',' << cli.sector_z << '\n';
  viewer.start_yaw = float(std::atan2(dz, dx));
  viewer.start_pitch = float(std::atan2(dy, std::hypot(dx, dz)));
#ifdef __GLIBC__
  // Сборки генератора выделяют и отпускают буферы под объявленную ёмкость в разных потоках, и куча glibc
  // держит пик у себя: без возврата системе долгий полёт дорастал до 2+ ГБ RSS при ~0.6 ГБ живой кучи.
  std::jthread trimmer([](const std::stop_token stop) {
    while (!stop.stop_requested()) {
      for (int i = 0; i < 100 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      malloc_trim(0);
    }
  });
#endif
  return devils_engine::gn03::run_viewer(viewer, make_builder(cli.radius));
} catch (const std::exception& error) {
  std::cerr << "GN09 viewer: " << error.what() << '\n';
  return 1;
}
