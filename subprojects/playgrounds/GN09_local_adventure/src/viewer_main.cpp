#include "adventure.h"
#include "local_structure.h"
#include "surface.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "memory.h"
#include "viewer.h"
#include "devils_engine/utils/prng.h"

namespace {

struct options {
  uint64_t seed = 0;
  double radius = 1.5;
  uint32_t frames = 0;
  uint32_t mode = 0;
  bool grid = false;
  bool validation = false;
  bool site_exterior = false;
  bool site_roof = false;
  std::string dump;
  std::optional<uint32_t> start_edge;
  std::optional<uint32_t> start_site;
  std::optional<uint32_t> start_cave;
  std::optional<uint32_t> start_exit;
  std::optional<uint32_t> start_hall;
  bool hall_throat = false;
  int sector_x = 0, sector_z = 0;
  int window_horizontal = 3, window_vertical = 3;
};

template <typename Number>
bool parse_number(const std::string_view source, Number& target) {
  const auto [end, error] = std::from_chars(source.data(), source.data() + source.size(), target);
  return error == std::errc() && end == source.data() + source.size();
}

bool parse(const int argc, char** argv, options& result) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help") {
      std::cout << "GN09_local_adventure_viewer [--seed=N] [--radius=R] [--frames=N] "
                   "[--view=0..3] [--start-edge=N|--start-site=N|--start-cave=N] "
                   "[--site-exterior|--site-roof|--start-exit=0..1] "
                   "[--start-hall=N [--hall-throat]] [--window=H,V] "
                   "[--start-sector=X,Z] [--dump=PATH] "
                   "[--grid] [--validation]\n";
      return false;
    }
    if (arg == "--grid") { result.grid = true; continue; }
    if (arg == "--validation") { result.validation = true; continue; }
    if (arg == "--site-exterior") { result.site_exterior = true; continue; }
    if (arg == "--site-roof") { result.site_roof = true; continue; }
    if (arg == "--hall-throat") { result.hall_throat = true; continue; }
    if (arg.starts_with("--start-sector=")) {
      const std::string_view pair = arg.substr(15);
      const size_t comma = pair.find(',');
      if (comma != std::string_view::npos &&
          parse_number(pair.substr(0, comma), result.sector_x) &&
          parse_number(pair.substr(comma + 1), result.sector_z)) continue;
    }
    if (arg.starts_with("--window=")) {
      const std::string_view pair = arg.substr(9);
      const size_t comma = pair.find(',');
      if (comma != std::string_view::npos &&
          parse_number(pair.substr(0, comma), result.window_horizontal) &&
          parse_number(pair.substr(comma + 1), result.window_vertical) &&
          result.window_horizontal >= 1 && result.window_horizontal <= 16 &&
          result.window_vertical >= 1 && result.window_vertical <= 16) continue;
    }
    if (arg.starts_with("--seed=") && parse_number(arg.substr(7), result.seed)) continue;
    if (arg.starts_with("--frames=") && parse_number(arg.substr(9), result.frames)) continue;
    if (arg.starts_with("--view=") && parse_number(arg.substr(7), result.mode) &&
        result.mode <= 3) continue;
    uint32_t edge = 0;
    if (arg.starts_with("--start-edge=") && parse_number(arg.substr(13), edge)) {
      result.start_edge = edge;
      continue;
    }
    if (arg.starts_with("--start-site=") && parse_number(arg.substr(13), edge)) {
      result.start_site = edge;
      continue;
    }
    if (arg.starts_with("--start-cave=") && parse_number(arg.substr(13), edge)) {
      result.start_cave = edge;
      continue;
    }
    if (arg.starts_with("--start-hall=") && parse_number(arg.substr(13), edge)) {
      result.start_hall = edge;
      continue;
    }
    if (arg.starts_with("--start-exit=") && parse_number(arg.substr(13), edge)) {
      result.start_exit = edge;
      continue;
    }
    if (arg.starts_with("--radius=") && parse_number(arg.substr(9), result.radius) &&
        std::isfinite(result.radius) && result.radius >= 0.5 && result.radius <= 3.0) continue;
    if (arg.starts_with("--dump=") && arg.size() > 7) {
      result.dump = arg.substr(7);
      continue;
    }
    std::cerr << "unknown or invalid option: " << arg << '\n';
    return false;
  }
  return true;
}

int64_t floor_div(const int64_t number, const int64_t divisor) {
  const int64_t quotient = number / divisor;
  return quotient - (number % divisor < 0);
}

devils_engine::gn03::factory_builder make_builder(const double radius) {
  return [radius](const std::vector<std::pair<std::string, double>>&, const uint64_t seed) {
    return [radius, seed]() -> devils_engine::gn03::chunk_generator {
      // Мир окна строится на блок 3x3 сектора с запасом в 2 сектора: соседние чанки
      // делят его, а поток держит несколько последних блоков. Окна разного размера
      // дают те же сэмплы, поэтому выбор блока не меняет мир.
      struct cached_world {
        int block_x = 0, block_z = 0;
        uint64_t used = 0;
        std::shared_ptr<const gn09::adventure> world;
      };
      std::vector<cached_world> worlds;
      uint64_t clock_tick = 0;
      return [radius, seed, worlds = std::move(worlds), clock_tick]
             (const devils_engine::originator::chunk_key& key,
              devils_engine::gn03::chunk_mesh& mesh) mutable {
        using clock = std::chrono::steady_clock;
        const auto begin = clock::now();
        const int sx = int(floor_div(key.x, gn09::sector_size / gn09::chunk::cells));
        const int sz = int(floor_div(key.z, gn09::sector_size / gn09::chunk::cells));
        const int bx = int(floor_div(sx, 3)), bz = int(floor_div(sz, 3));
        auto found = std::find_if(worlds.begin(), worlds.end(), [&](const cached_world& w) {
          return w.block_x == bx && w.block_z == bz;
        });
        if (found == worlds.end()) {
          if (worlds.size() >= 6) {
            found = std::min_element(worlds.begin(), worlds.end(),
              [](const cached_world& a, const cached_world& b) { return a.used < b.used; });
          } else {
            found = worlds.insert(worlds.end(), cached_world{});
          }
          found->block_x = bx;
          found->block_z = bz;
          found->world = std::make_shared<const gn09::adventure>(gn09::generate(seed,
            {3 * bx - 2, 3 * bz - 2, 3 * bx + 5, 3 * bz + 5}, radius));
        }
        found->used = ++clock_tick;
        const gn09::adventure* cached = found->world.get();
        const gn09::field_region region = gn09::prepare_chunk_field(*cached, int(key.x),
                                                                   int(key.y), int(key.z));
        // Сплошная скала или вода внутри зала: поверхности нет, сэмплы не нужны.
        const bool uniform = gn09::uniform_sign(region) != 0;
        const gn09::chunk volume = uniform ? gn09::chunk{int(key.x), int(key.y), int(key.z), {}}
          : gn09::generate_chunk(region, int(key.x), int(key.y), int(key.z));
        const auto field_done = clock::now();
        gn09::surface_mesh surface;
        if (!uniform) {
          surface = gn09::generate_surface(region, volume);
          gn09::append_outpost_surface(*cached, volume, surface);
        }
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
        // Ресурсы принадлежат конкретному чанку по мировому положению, а их имя
        // стабильно от ключа узла и индекса залежи при повторной генерации.
        const double origin_x = double(key.x * gn09::chunk::cells);
        const double origin_y = double(key.y * gn09::chunk::cells);
        const double origin_z = double(key.z * gn09::chunk::cells);
        for (const gn09::node& at : cached->route.nodes) {
          if (at.kind != gn09::theme::grotto && at.kind != gn09::theme::vent) continue;
          if (std::abs(at.centre.x - origin_x) > 40.0 ||
              std::abs(at.centre.y - origin_y) > 40.0 ||
              std::abs(at.centre.z - origin_z) > 40.0) continue;
          const gn09::local_structure structure = gn09::materialize_local_structure(*cached, at.id);
          for (uint32_t i = 0; i < structure.features.size(); ++i) {
            const gn09::local_feature& feature = structure.features[i];
            const double x = feature.centre.x - origin_x;
            const double y = feature.centre.y - origin_y;
            const double z = feature.centre.z - origin_z;
            if (x < 0.0 || x >= gn09::chunk::cells || y < 0.0 || y >= gn09::chunk::cells ||
                z < 0.0 || z >= gn09::chunk::cells) continue;
            devils_engine::gn03::chunk_prop prop;
            prop.position[0] = x;
            prop.position[1] = y;
            prop.position[2] = z;
            prop.normal[1] = 1.0;
            prop.size = 1.8;
            prop.kind = feature.kind == "egg_cluster" ? 3u :
                        feature.kind == "thermal_crystal" ? 2u : 1u;
            prop.origin = uint32_t(devils_engine::utils::mix(seed, at.key, i, 0x524553ull));
            mesh.props.push_back(prop);
          }
        }
        const auto done = clock::now();
        mesh.field_milliseconds = std::chrono::duration<double, std::milli>(field_done - begin).count();
        mesh.surface_milliseconds = std::chrono::duration<double, std::milli>(done - field_done).count();
      };
    };
  };
}

} // namespace

int main(const int argc, char** argv) {
  options cli;
  if (!parse(argc, argv, cli)) return 2;
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
  if (cli.site_exterior && !cli.start_site) {
    std::cerr << "site exterior view requires --start-site\n";
    return 2;
  }
  if (cli.site_roof && (!cli.start_site || cli.site_exterior || cli.start_exit)) {
    std::cerr << "roof view requires --start-site and no other site view\n";
    return 2;
  }
  if (cli.start_exit && (!cli.start_site || *cli.start_exit >= 2 || cli.site_exterior)) {
    std::cerr << "emergency exit view requires --start-site and --start-exit=0 or 1\n";
    return 2;
  }
  gn09::point start = start_world.route.nodes[start_world.route.entry].centre;
  gn09::point target;
  if (cli.start_site) {
    const gn09::local_structure structure = gn09::materialize_local_structure(start_world,
                                                                               *cli.start_site);
    const double offset = double(structure.zones[1].box.v0 + 200) / 100.0;
    if (cli.site_roof) {
      // Снаружи над крышей: пролом, иначе первый застеклённый модуль, иначе модуль 3.
      uint32_t zone = structure.roof_breach;
      for (uint32_t i = 3; zone == 0 && i < structure.zones.size() && i < 16; ++i) {
        if ((structure.glazed_roofs >> i) & 1u) zone = i;
      }
      if (zone == 0) zone = 3;
      const gn09::plan_rect box = structure.zones[zone].box;
      const double u = double(box.u0 + box.u1) / 200.0, v = double(box.v0 + box.v1) / 200.0;
      const double roof = double(structure.level_cm) / 100.0 + 3.0 +
                          (structure.dock_axis == 0 ? 0.6 : 2.4);
      target = gn09::structure_to_world(structure, u, roof, v);
      start = gn09::structure_to_world(structure, u - 6.0, roof + 5.0, v);
      bool found = false;
      for (const double lift : {5.0, 3.5, 7.0, 2.5, 9.0}) {
        for (const double back : {6.0, 9.0, 3.0, 12.0, 0.5}) {
          for (const double side : {0.0, 4.0, -4.0}) {
            const gn09::point candidate = gn09::structure_to_world(structure, u - back,
                                                                   roof + lift, v + side);
            if (found || gn09::density(start_world, candidate) >= -0.5) continue;
            start = candidate;
            found = true;
          }
        }
      }
      std::cout << "GN09 roof view zone=" << structure.zones[zone].purpose
                << (found ? "" : " (no open camera point found)") << '\n';
    } else if (cli.start_exit) {
      const gn09::emergency_exit& door = structure.emergency_exits[*cli.start_exit];
      const double side = door.span.v0 == structure.zones[door.zone].box.v0 ? -1.0 : 1.0;
      const gn09::point mouth{0, 0, side * 12.0};
      const gn09::point entrance{double(door.span.u0 + door.span.u1) / 200.0,
                                 double(structure.level_cm) / 100.0,
                                 double(door.span.v0) / 100.0 + side * 1.2};
      start = gn09::structure_to_world(structure,
        mouth.x * 0.3 + entrance.x * 0.7,
        mouth.y * 0.3 + entrance.y * 0.7,
        mouth.z * 0.3 + entrance.z * 0.7);
      target = gn09::structure_to_world(structure, entrance.x,
                                        entrance.y, entrance.z - side * 1.2);
    } else if (structure.dock_axis == 0) {
      start = cli.site_exterior
        ? gn09::structure_to_world(structure, 4.0, 0.0, offset)
        : gn09::structure_to_world(structure, 19.0, 0.0, offset);
      target = gn09::structure_to_world(structure,
                                        cli.site_exterior ? 19.0 : 39.0, 0.0, offset);
    } else {
      start = cli.site_exterior
        ? gn09::docking_axis_point(structure, 1.0, 0.0)
        : gn09::structure_to_world(structure, 7.0,
                                    double(structure.level_cm) / 100.0, 0.0);
      target = cli.site_exterior
        ? gn09::structure_to_world(structure, 0.5,
                                    double(structure.level_cm) / 100.0, 0.0)
        : gn09::structure_to_world(structure, 20.0,
                                    double(structure.level_cm) / 100.0, 0.0);
    }
  } else if (cli.start_hall) {
    // Внутри зала на оси коридора существ, взгляд вдоль зала.
    const gn09::hall& h = start_world.halls[*cli.start_hall];
    const auto at = [&](const double u, const double y) {
      return gn09::point{h.centre.x + u * h.forward.x, h.centre.y + y, h.centre.z + u * h.forward.z};
    };
    start = at(-0.6 * h.core_half, h.core_y + 0.3 * h.core_radius);
    target = at(0.6 * h.core_half, h.core_y);
    if (cli.hall_throat) {
      // Из зала вверх (или вниз) по горловине: камера чуть в стороне от оси шахты.
      const double dir = h.throat_top.y > h.throat_end.y ? 1.0 : -1.0;
      start = {h.throat_end.x + 6.0, h.throat_end.y - dir * 12.0, h.throat_end.z};
      target = {h.throat_end.x, h.throat_end.y + dir * 120.0, h.throat_end.z + 4.0};
    }
    std::cout << "GN09 hall " << *cli.start_hall << ' ' << gn09::name(h.kind) << " half="
              << h.half_length << 'x' << h.half_width << 'x' << h.half_height << " m\n";
  } else if (cli.start_cave) {
    const gn09::site local = gn09::materialize_site(start_world, *cli.start_cave);
    start = local.rooms[0].centre;
    target = local.rooms[1].centre;
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
  viewer.overlay_summary = "Caves, built outposts, 17 x 6 m capsule route; GN03 chunk viewer";
  viewer.overlay_controls = "WASD/QE fly | Shift fast | M view | G chunk grid | N new seed | U overlay | Esc quit";
  viewer.seed = cli.seed;
  viewer.chunk_cells = gn09::chunk::cells;
  viewer.cell_size = 1.0;
  viewer.window.horizontal_radius = cli.window_horizontal;
  viewer.window.vertical_radius = cli.window_vertical;
  // Окно больше стартового 7^3 нужно залам; арена растёт вместе с объёмом окна.
  const size_t window_chunks = size_t(2 * cli.window_horizontal + 1) *
                               size_t(2 * cli.window_horizontal + 1) *
                               size_t(2 * cli.window_vertical + 1);
  viewer.arena_vertices = std::max<size_t>(2u << 20, window_chunks * 6000);
  viewer.workers = 4;
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
  return devils_engine::gn03::run_viewer(viewer, make_builder(cli.radius));
}
