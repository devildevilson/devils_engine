#include <exception>
#include "devils_engine/playground/cli.h"
#include <cstdio>
#include <map>
#include "adventure.h"
#include "graph_view.h"
#include "local_structure.h"
#include "station.h"
#include "surface.h"
#include "tunnel.h"
#include "cave.h"
#include "world_chain.h"

#include <algorithm>
#include <malloc.h>
#include <unistd.h>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <optional>
#include <set>
#include <iomanip>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <tuple>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct options {
  uint64_t seed = 0;
  uint32_t seeds = 64;
  double radius = 1.0;
  bool verify = false;
  bool bench = false;
  std::string svg;
  std::string site_svg;
  std::string structure_svg;
  std::string station_svg;
  bool station_stats = false;
  std::string graph_html;
  uint64_t graph_sectors = 6;
  uint32_t site_id = 0;
  int sector_x = 0, sector_z = 0;
  // Полёт: скорость камеры (м/с), длительность, рабочие потоки чанков.
  double flight = 0.0;
  double flight_seconds = 20.0;
  uint32_t workers = 4;
  // Сколько слоёв чанков впереди окна по ходу готовить заранее (только зависимости чанка).
  uint32_t lead = 0;
};







std::optional<int> parse(const int argc, char** argv, options& out) {
  CLI::App app{"GN09 local adventure"};
  app.option_defaults()->always_capture_default();
  app.add_flag("--verify", out.verify, "verify");
  app.add_flag("--bench", out.bench, "bench");
  app.add_flag("--station-stats", out.station_stats, "station-stats");
  app.add_option("--seed", out.seed, "World seed");
  app.add_option("--radius", out.radius, "Clearance radius")->check(devils_engine::playground::finite_number)->check(CLI::Range(0.5, 3.0));
  app.add_option("--seeds", out.seeds, "Verify seeds")->check(CLI::Range(1, 10000));
  app.add_option("--svg", out.svg, "Window SVG");
  app.add_option("--site-svg", out.site_svg, "Site SVG");
  app.add_option("--station-svg", out.station_svg, "Station SVG");
  app.add_option("--structure-svg", out.structure_svg, "Structure SVG");
  app.add_option("--graph-html", out.graph_html, "Graph HTML");
  app.add_option("--graph-sectors", out.graph_sectors, "Graph window")->check(CLI::Range(1, 16));
  app.add_option("--site", out.site_id, "Site index");
  app.add_option("--flight", out.flight, "Flight speed")->check(devils_engine::playground::finite_number)->check(CLI::NonNegativeNumber);
  app.add_option("--flight-seconds", out.flight_seconds, "Flight duration")->check(devils_engine::playground::finite_number)->check(CLI::PositiveNumber);
  app.add_option("--lead", out.lead, "Prefetch lead")->check(CLI::Range(0, 32));
  app.add_option("--workers", out.workers, "Workers")->check(CLI::Range(1, 64));
  std::string mods;
  app.add_option("--mods", mods, "Ordered mod folders");
  std::array<int, 2> sector{out.sector_x, out.sector_z};
  devils_engine::playground::add_tuple(app, "--sector", sector, ',', [](const auto& v) { return v[0] >= -100000 && v[0] <= 100000 && v[1] >= -100000 && v[1] <= 100000; });
  if (auto code = devils_engine::playground::parse_cli(app, argc, argv)) return code;
  out.sector_x = sector[0]; out.sector_z = sector[1];
  if (app.count("--mods")) gn09::set_world_mods(mods);
  return std::nullopt;
}

void report(const gn09::adventure& world, const gn09::verification& result) {
  uint32_t bases = 0, empty = 0;
  uint32_t pockets = 0;
  for (const gn09::node& at : world.route.nodes) {
    bases += gn09::is_base(at.kind);
    empty += at.kind == gn09::theme::empty;
  }
  for (const gn09::edge link : world.route.edges) {
    pockets += gn09::materialize_edge_pocket(world.seed,
      world.route.nodes[link.a], world.route.nodes[link.b]).has_value();
  }
  std::cout << "GN09 seed=" << world.seed << " R=" << world.clearance_radius
            << " m graph=" << world.route.nodes.size() << " nodes/"
            << world.route.edges.size() << " edges, entry=" << world.route.entry
            << " destination=" << world.route.destination << ", bases=" << bases
            << " empty=" << empty << " decorative edge pockets=" << pockets << "\n";
  for (const gn09::node& at : world.route.nodes) {
    std::cout << "  node " << at.id << " rank=" << unsigned(at.rank) << ' ' << gn09::name(at.kind)
              << " at (" << at.centre.x << ',' << at.centre.y << ',' << at.centre.z << ")\n";
  }
  for (const gn09::edge link : world.route.edges) {
    const gn09::point a = world.route.nodes[link.a].centre;
    const gn09::point b = world.route.nodes[link.b].centre;
    std::cout << "  route " << link.a << " -- " << link.b << " biome=" <<
      gn09::name(gn09::biome_at(world.seed,
        {(a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2})) << '\n';
  }
  for (uint32_t i = 0; i < world.route.nodes.size(); ++i) {
    if (world.route.nodes[i].kind == gn09::theme::empty) continue;
    const gn09::site local = gn09::materialize_site(world, i);
    std::cout << "  site " << local.node_id << " " << gn09::name(local.kind)
              << '/' << gn09::name(local.recipe) << " at ("
              << local.centre.x << ',' << local.centre.y << ',' << local.centre.z << ") "
              << local.portals.size()
              << " ports, " << local.links.size()
              << (gn09::is_base(local.kind) ? " cave links, module programme:" : " room links, rooms:");
    for (const gn09::room& chamber : local.rooms) std::cout << ' ' << chamber.purpose;
    std::cout << '\n';
    if (!gn09::is_base(local.kind)) {
      const gn09::local_structure structure = gn09::materialize_local_structure(world, i);
      std::cout << "    local structure: " << structure.features.size() << " local features\n";
      if (local.has_cave) {
        static constexpr const char* lairs[4] = {"in a niche", "side pocket", "empty dead end", "?"};
        std::cout << "    cave: scale " << local.scale << ", throat core " << local.throat_core
                  << (local.blocks_sub ? " (the boat stays out)" : " (wider than the hull)") << ", lair "
                  << lairs[local.lair_kind % 4] << " at " << gn09::cave_room_depth(local, local.lair) << " m, nest "
                  << gn09::cave_depth(local) << " m deep, "
                  << std::hypot(local.rooms[local.nest].centre.x - local.rooms[local.mouth].centre.x,
                                local.rooms[local.nest].centre.y - local.rooms[local.mouth].centre.y,
                                local.rooms[local.nest].centre.z - local.rooms[local.mouth].centre.z)
                  << " m from the mouth, " << local.rooms.size() << " rooms, " << local.deposits << " deposits x " << local.collect << " s, swim budget " << local.budget
                  << " s, estimate " << local.estimate << " s, tour " << gn09::cave_tour_length(local) << " m\n";
      }
    } else if (const auto built = gn09::station_for(world, i)) {
      const gn09::built_station& station = *built;
      const gn09::station_site& site = station.plan.site;
      uint32_t inner = 0;
      for (const gn09::station_room& r : station.plan.rooms) inner += r.inner;
      std::cout << "    station: pose=" << gn09::name(site.pose) << " yaw="
                << site.yaw * 57.29577951308232 << " deg, floor y=" << site.origin.y
                << ", tilt=" << site.tilt_deg << " deg, residual=" << site.residual << " m, rooms="
                << station.plan.rooms.size() << " (inner " << inner << "), doors="
                << station.plan.doors.size() << ", triangles=" << station.mesh.size() << ":";
      for (const gn09::station_room& r : station.plan.rooms) std::cout << ' ' << r.purpose;
      std::cout << '\n';
    } else {
      std::cout << "    station: no site found\n";
    }
  }
  for (size_t i = 0; i < world.halls.size(); ++i) {
    const gn09::hall& h = world.halls[i];
    std::cout << "  hall " << i << ' ' << gn09::name(h.kind) << " region=" << h.region_x << ','
              << h.region_z << " at (" << h.centre.x << ',' << h.centre.y << ',' << h.centre.z
              << ") half=" << h.half_length << 'x' << h.half_width << 'x' << h.half_height
              << " m, pillars=" << h.pillars.size() << ", ledges=" << h.ledges.size()
              << ", resident " << gn09::name(h.resident) << " body r=" << h.body_radius
              << " m, niches=" << h.niches.size()
              << ", route entrances=" << gn09::hall_entrances(world.seed, h).size()
              << '\n';
    for (const gn09::station_anchor& anchor : gn09::hall_station_anchors(world.seed, h)) {
      const auto station = gn09::station_for(world, anchor);
      std::cout << "    hall outpost " << std::hex << anchor.key << std::dec << ' ' << gn09::name(anchor.kind)
                << '/' << gn09::name(anchor.recipe) << " at (" << anchor.centre.x << ',' << anchor.centre.y
                << ',' << anchor.centre.z << "): ";
      if (station) {
        std::cout << "pose=" << gn09::name(station->plan.site.pose) << ", rooms=" << station->plan.rooms.size()
                  << ", triangles=" << station->mesh.size() << '\n';
      } else {
        std::cout << "no site found\n";
      }
    }
  }
  {
    std::vector<uint64_t> seen;
    for (const gn09::station_anchor& anchor : gn09::station_anchors(world)) {
      const auto tunnel = gn09::tunnel_for(world, anchor);
      if (!tunnel || std::find(seen.begin(), seen.end(), tunnel->key) != seen.end()) continue;
      seen.push_back(tunnel->key);
      std::cout << "  tunnel " << std::hex << tunnel->first_key << " <-> " << tunnel->second_key << std::dec
                << ": span " << tunnel->span << " m, slope " << tunnel->slope_deg << " deg, triangles "
                << tunnel->mesh.size() << '\n';
    }
    if (seen.empty()) std::cout << "  no dry tunnels between stations in this window\n";
  }
  std::cout << "field: " << result.checked_samples << " checked 3D profile samples, "
            << result.reached_rooms
            << '/' << result.total_rooms << " reachable rooms, longest non-base dead end="
            << result.max_deadend_nodes << " nodes, wide route="
            << result.wide_route_percent << "%, submarine=" << gn09::starter_sub_length() << "x" << gn09::starter_sub_beam() << " m along edges and turns; "
            << (result.ok ? "verified" : result.cause) << '\n';
}

// Цена поля по видам мест. Отпечаток сэмплов и вершин сравнивается до и после
// оптимизации: ускорение обязано давать побитово тот же мир.
int bench(const options& opt) {
  using clock = std::chrono::steady_clock;
  const auto ms = [](const clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  const auto start = clock::now();
  const gn09::adventure world = gn09::generate(opt.seed, {-2, -2, 3, 3}, opt.radius);
  std::cout << "GN09 bench seed=" << opt.seed << " window 5x5 sectors: "
            << world.route.nodes.size() << " nodes/" << world.route.edges.size()
            << " edges, generate " << ms(clock::now() - start) << " ms\n";
  const auto pick = [&](const auto predicate) -> const gn09::node* {
    for (const gn09::node& at : world.route.nodes) {
      const double c = 2.5 * gn09::sector_size();
      if (std::abs(at.centre.x - 0.5 * gn09::sector_size()) < c &&
          std::abs(at.centre.z - 0.5 * gn09::sector_size()) < c && predicate(at)) return &at;
    }
    return nullptr;
  };
  std::cout << "  " << world.halls.size() << " halls reach the window\n";
  const std::array<std::pair<const char*, const gn09::node*>, 3> places{{
    {"base", pick([](const gn09::node& at) { return gn09::is_base(at.kind); })},
    {"natural site", pick([](const gn09::node& at) {
      return at.kind == gn09::theme::grotto || at.kind == gn09::theme::vent; })},
    {"empty node", pick([](const gn09::node& at) { return at.kind == gn09::theme::empty; })},
  }};
  uint64_t fingerprint = 1469598103934665603ull;
  const auto mix = [&](const uint64_t v) { fingerprint = (fingerprint ^ v) * 1099511628211ull; };
  for (const auto& [label, at] : places) {
    if (at == nullptr) { std::cout << "  " << label << ": none in window\n"; continue; }
    const int cx = int(std::floor(at->centre.x / gn09::chunk::cells));
    const int cy = int(std::floor(at->centre.y / gn09::chunk::cells));
    const int cz = int(std::floor(at->centre.z / gn09::chunk::cells));
    clock::duration prepare{}, field{}, surface{}, station{};
    size_t vertices = 0, kept_edges = 0, kept_nodes = 0, kept_sites = 0;
    uint32_t chunks = 0;
    for (int z = cz - 1; z <= cz + 1; ++z) for (int y = cy - 1; y <= cy + 1; ++y)
      for (int x = cx - 1; x <= cx + 1; ++x) {
        const auto t0 = clock::now();
        const gn09::field_region region = gn09::prepare_chunk_field(world, x, y, z);
        const auto t1 = clock::now();
        const gn09::chunk volume = gn09::generate_chunk(region, x, y, z);
        const auto t2 = clock::now();
        gn09::surface_mesh mesh = gn09::generate_surface(region, volume);
        const auto t3 = clock::now();
        gn09::append_outpost_surface(world, volume, mesh);
        const auto t4 = clock::now();
        prepare += t1 - t0;
        kept_edges += region.edges.size();
        kept_nodes += region.nodes.size();
        kept_sites += region.sites + region.stations;
        field += t2 - t1;
        surface += t3 - t2;
        station += t4 - t3;
        vertices += mesh.size();
        ++chunks;
        for (const double v : volume.samples) mix(std::bit_cast<uint64_t>(v));
        for (const gn09::surface_vertex& v : mesh) {
          mix(std::bit_cast<uint64_t>(v.position.x));
          mix(std::bit_cast<uint64_t>(v.position.y));
          mix(std::bit_cast<uint64_t>(v.position.z));
          mix(std::bit_cast<uint64_t>(v.normal.x));
          mix(v.shade);
        }
      }
    std::cout << "  " << label << " node " << at->id << ": " << chunks
              << " chunks, ms/chunk: prepare " << ms(prepare) / chunks
              << ", field " << ms(field) / chunks << ", surface " << ms(surface) / chunks
              << ", station " << ms(station) / chunks << "; " << vertices << " vertices; per chunk "
              << double(kept_edges) / chunks << " edges, " << double(kept_nodes) / chunks
              << " nodes, " << double(kept_sites) / chunks << " places of "
              << world.route.edges.size() << '/' << world.route.nodes.size() << '\n';
  }
  // Зал: столб чанков через середину по вертикали, от пола до потолка и дальше в скалу.
  if (!world.halls.empty()) {
    const gn09::hall& h = world.halls.front();
    const int cx = int(std::floor(h.centre.x / gn09::chunk::cells));
    const int cz = int(std::floor(h.centre.z / gn09::chunk::cells));
    const int y0 = int(std::floor((h.centre.y - h.half_height - 40.0) / gn09::chunk::cells));
    const int y1 = int(std::floor((h.centre.y + h.half_height + 40.0) / gn09::chunk::cells));
    clock::duration total{};
    uint32_t chunks = 0, rock = 0, air = 0;
    size_t vertices = 0;
    for (int y = y0; y <= y1; ++y) for (int z = cz - 1; z <= cz + 1; ++z)
      for (int x = cx - 1; x <= cx + 1; ++x) {
        const auto t0 = clock::now();
        const gn09::field_region region = gn09::prepare_chunk_field(world, x, y, z);
        const int sign = gn09::uniform_sign(region);
        rock += sign > 0;
        air += sign < 0;
        if (sign == 0) {
          const gn09::chunk volume = gn09::generate_chunk(region, x, y, z);
          vertices += gn09::generate_surface(region, volume).size();
        }
        total += clock::now() - t0;
        ++chunks;
      }
    std::cout << "  hall 0 " << gn09::name(h.kind) << " column: " << chunks << " chunks, "
              << air << " uniform water, " << rock << " uniform rock, "
              << ms(total) / chunks << " ms/chunk on average, " << vertices << " vertices\n";
  }
  std::cout << "  fingerprint " << std::hex << fingerprint << std::dec << '\n';

  // Те же чанки из цепочки (chunk_field: окно 5x5 вокруг сектора чанка, пещеры fan-in, поле `solid` из
  // словаря форм). Эталон — поле, собранное хостом на окне 7x7 вокруг сектора чанка: окно бенча у своего
  // края обрезает рёбра соседних секторов и эталоном там быть не может.
  std::map<std::pair<int, int>, gn09::adventure> wide_windows;
  const auto wide_window = [&](const int x, const int z) -> const gn09::adventure& {
    const int sx = int(std::floor(x * double(gn09::chunk::cells) / gn09::sector_size()));
    const int sz = int(std::floor(z * double(gn09::chunk::cells) / gn09::sector_size()));
    auto found = wide_windows.find({sx, sz});
    if (found == wide_windows.end()) {
      found = wide_windows.emplace(std::pair{sx, sz}, gn09::generate(opt.seed, {sx - 3, sz - 3, sx + 4, sz + 4}, opt.radius)).first;
    }
    return found->second;
  };
  uint64_t chain_print = 1469598103934665603ull;
  const auto chain_mix = [&](const uint64_t v) { chain_print = (chain_print ^ v) * 1099511628211ull; };
  clock::duration chain_time{}, rebuild_time{}, empty_time{}, surface_time{};
  uint32_t empty_chunks = 0, surface_chunks = 0, skipped_chunks = 0;
  uint32_t chain_chunks = 0;
  size_t differing = 0, rounding_only = 0, rebuilt_differ = 0, surface_differ = 0, props_seen = 0, props_differ = 0;
  double max_sample_error = 0.0;
  auto& chain = gn09::world_chain(opt.radius);
  for (const auto& [label, at] : places) {
    if (at == nullptr) continue;
    const int cx = int(std::floor(at->centre.x / gn09::chunk::cells));
    const int cy = int(std::floor(at->centre.y / gn09::chunk::cells));
    const int cz = int(std::floor(at->centre.z / gn09::chunk::cells));
    for (int z = cz - 1; z <= cz + 1; ++z) for (int y = cy - 1; y <= cy + 1; ++y)
      for (int x = cx - 1; x <= cx + 1; ++x) {
        const auto t0 = clock::now();
        const gn09::chunk_instance made = gn09::chunk_from_chain(opt.seed, opt.radius, x, y, z, true);
        const gn09::surface_mesh mesh = gn09::surface_from_chain(made);
        chain_time += clock::now() - t0;
        // Поверхность хоста по тому же полю: марширующие тетраэдры и обрезка построек на C++.
        gn09::surface_mesh host = gn09::generate_surface(made.region, made.volume);
        gn09::append_outpost_surface(*made.region.world, made.volume, host);
        surface_differ += host.size() != mesh.size();
        // Предметы хоста (прежний отбор вьювера: места окна в cave_reach от угла чанка) против выхода props.
        {
          std::vector<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>> wanted, got;
          const double origin[3]{double(x * gn09::chunk::cells), double(y * gn09::chunk::cells),
                                 double(z * gn09::chunk::cells)};
          const auto& kinds = gn09::cave_feature_kinds();
          for (const gn09::node& at : made.region.world->route.nodes) {
            if (at.kind != gn09::theme::grotto && at.kind != gn09::theme::vent) continue;
            if (std::abs(at.centre.x - origin[0]) > gn09::cave_reach() || std::abs(at.centre.y - origin[1]) > gn09::cave_reach() ||
                std::abs(at.centre.z - origin[2]) > gn09::cave_reach()) continue;
            const auto structure = gn09::materialize_local_structure(*made.region.world, at.id);
            for (uint64_t i = 0; i < structure.features.size(); ++i) {
              const auto& f = structure.features[i];
              const double l[3]{f.centre.x - origin[0], f.centre.y - origin[1], f.centre.z - origin[2]};
              if (l[0] < 0.0 || l[0] >= gn09::chunk::cells || l[1] < 0.0 || l[1] >= gn09::chunk::cells ||
                  l[2] < 0.0 || l[2] >= gn09::chunk::cells) continue;
              const auto code = uint64_t(std::find(kinds.begin(), kinds.end(), f.kind) - kinds.begin());
              wanted.emplace_back(std::bit_cast<uint64_t>(f.centre.x), std::bit_cast<uint64_t>(f.centre.z), code, at.key, i);
            }
          }
          const auto pos = made.instance->field("props", "position"), kind = made.instance->field("props", "kind"),
                     node = made.instance->field("props", "node"), index = made.instance->field("props", "index");
          for (size_t i = 0, n = size_t(made.instance->field("surface_counts", "props").get(0)); i < n; ++i) {
            got.emplace_back(std::bit_cast<uint64_t>(pos.get(i, 0)), std::bit_cast<uint64_t>(pos.get(i, 2)),
                             uint64_t(kind.get(i)), devils_engine::originator::read_key_word(node, i), uint64_t(index.get(i)));
          }
          props_seen += got.size();
          props_differ += wanted != got;
        }
        for (size_t i = 0; i < std::min(host.size(), mesh.size()); ++i) {
          const auto& a = host[i]; const auto& b = mesh[i];
          surface_differ += std::bit_cast<uint64_t>(a.position.x) != std::bit_cast<uint64_t>(b.position.x) ||
                            std::bit_cast<uint64_t>(a.position.y) != std::bit_cast<uint64_t>(b.position.y) ||
                            std::bit_cast<uint64_t>(a.position.z) != std::bit_cast<uint64_t>(b.position.z) ||
                            std::bit_cast<uint64_t>(a.normal.x) != std::bit_cast<uint64_t>(b.normal.x) ||
                            std::bit_cast<uint64_t>(a.normal.y) != std::bit_cast<uint64_t>(b.normal.y) ||
                            std::bit_cast<uint64_t>(a.normal.z) != std::bit_cast<uint64_t>(b.normal.z) || a.shade != b.shade;
        }
        // Пересборка мимо кэша при готовых связях: цена самого поля чанка и проверка повторяемости.
        const devils_engine::originator::instance_key key{{uint64_t(int64_t(x)), uint64_t(int64_t(y)), uint64_t(int64_t(z))}};
        const auto t1 = clock::now();
        rebuilt_differ += !chain.verify("chunk_field", opt.seed, key).empty();
        rebuild_time += clock::now() - t1;
        (mesh.empty() ? empty_time : surface_time) += clock::now() - t1;
        (mesh.empty() ? empty_chunks : surface_chunks) += 1;
        ++chain_chunks;
        const gn09::chunk reference =
          gn09::generate_chunk(gn09::prepare_chunk_field(wide_window(x, z), x, y, z), x, y, z);
        if (made.sampled) {
          for (size_t i = 0; i < reference.samples.size(); ++i) {
            const double host = reference.samples[i], script = made.volume.samples[i];
            if (std::bit_cast<uint64_t>(host) != std::bit_cast<uint64_t>(script)) {
              if (!std::isfinite(host) || !std::isfinite(script)) { ++differing; continue; }
              const double error = std::abs(host - script);
              max_sample_error = std::max(max_sample_error, error);
              if (error > 1e-10 * std::max({1.0, std::abs(host), std::abs(script)})) ++differing;
              else ++rounding_only;
            }
          }
        } else {
          // Пропущенный чанк обязан быть сплошной породой и у хоста.
          ++skipped_chunks;
          for (const double v : reference.samples) differing += v <= 0.0;
        }
        for (const double v : made.volume.samples) chain_mix(std::bit_cast<uint64_t>(v));
        for (const gn09::surface_vertex& v : mesh) {
          chain_mix(std::bit_cast<uint64_t>(v.position.x));
          chain_mix(std::bit_cast<uint64_t>(v.position.y));
          chain_mix(std::bit_cast<uint64_t>(v.position.z));
          chain_mix(std::bit_cast<uint64_t>(v.normal.x));
          chain_mix(v.shade);
        }
      }
  }
  const double n = std::max(chain_chunks, 1u);
  std::cout << "  chain chunks: " << chain_chunks << ", cold " << ms(chain_time) / n
            << " ms/chunk (links + field + surface), field rebuild " << ms(rebuild_time) / n << " ms/chunk ("
            << rebuilt_differ << " rebuilds differ), " << differing
            << " samples materially differ from the 7x7 window (" << rounding_only
            << " rounding-only, max absolute error " << max_sample_error << "), " << surface_differ
            << " surface vertices differ from the host mesh, " << props_seen << " props (" << props_differ
            << " chunks differ from the host list), fingerprint " << std::hex << chain_print << std::dec << '\n';
  std::cout << "  unsampled solid chunks " << skipped_chunks << '\n';
  std::cout << "  rebuild by kind: " << empty_chunks << " empty chunks " << ms(empty_time) / std::max(empty_chunks, 1u)
            << " ms, " << surface_chunks << " with surface " << ms(surface_time) / std::max(surface_chunks, 1u) << " ms\n";
  return 0;
}

// Полёт в реальном времени: камера идёт по прямой со скоростью opt.flight, окно 9x9x9 чанков вокруг
// неё (как у просмотрщика), рабочие потоки берут ближайший к камере недостроенный чанк и строят его
// целиком (поле, поверхность, постройки — chunk_from_chain + surface_from_chain). Мерится:
//   - старт: через сколько готовы ближние чанки (куб 5x5x5) неподвижной камеры;
//   - в полёте: задержка чанка от входа в окно до готовности и кадры (по 1/60 с), когда в ближнем
//     кубе есть дыра — то, что игрок увидел бы как недостроенный мир;
//   - цепочка: где ушло время по генераторам (своя сборка, ожидание чужой).
int flight_bench(const options& opt) {
  using clock = std::chrono::steady_clock;
  constexpr int window = 4, near = 2;
  const double side = gn09::chunk::cells;
  auto& chain = gn09::world_chain(opt.radius);
  const gn09::adventure start_world = gn09::generate(opt.seed, {opt.sector_x, opt.sector_z, opt.sector_x + 2,
                                                               opt.sector_z + 2}, opt.radius);
  const gn09::point start = start_world.route.nodes.front().centre;
  using key = std::array<int, 3>;
  struct chunk_state { clock::time_point wanted; clock::time_point done; bool ready = false, taken = false; };
  std::mutex lock;
  std::condition_variable wake;
  std::map<key, chunk_state> chunks;
  gn09::point camera = start;
  bool stop = false;
  std::atomic<uint64_t> work_ns{0};
  std::atomic<uint64_t> shapeless{0}, meshless{0};
  const auto chunk_of = [&](const gn09::point p) {
    return key{int(std::floor(p.x / side)), int(std::floor(p.y / side)), int(std::floor(p.z / side))};
  };
  const auto distance2 = [&](const key& k, const key& c) {
    const int dx = k[0] - c[0], dy = k[1] - c[1], dz = k[2] - c[2];
    return dx * dx + dy * dy + dz * dz;
  };
  // Окно вокруг камеры: новые чанки входят с отметкой времени, вышедшие забываются.
  const auto refresh = [&](const clock::time_point now) {
    const key c = chunk_of(camera);
    for (int z = -window; z <= window; ++z) for (int y = -window; y <= window; ++y) for (int x = -window; x <= window; ++x) {
      const key k{c[0] + x, c[1] + y, c[2] + z};
      if (!chunks.contains(k)) chunks.emplace(k, chunk_state{now, {}, false, false});
    }
    std::erase_if(chunks, [&](const auto& item) {
      const auto& k = item.first;
      return !item.second.taken && (std::abs(k[0] - c[0]) > window || std::abs(k[1] - c[1]) > window ||
                                    std::abs(k[2] - c[2]) > window);
    });
  };
  // Опережение: слои впереди окна по ходу (+x), зависимости их чанков строятся заранее.
  std::set<key> prefetch_wanted, prefetch_done;
  const auto refresh_prefetch = [&] {
    if (opt.lead == 0) return;
    const key c = chunk_of(camera);
    for (int d = 1; d <= int(opt.lead); ++d) for (int z = -window; z <= window; ++z) for (int y = -window; y <= window; ++y) {
      const key k{c[0] + window + d, c[1] + y, c[2] + z};
      if (!prefetch_done.contains(k)) prefetch_wanted.insert(k);
    }
  };
  std::atomic<uint64_t> prefetch_ns{0};
  std::vector<double> latencies;
  std::vector<std::thread> workers;
  for (uint32_t w = 0; w < opt.workers; ++w) {
    workers.emplace_back([&] {
      std::unique_lock guard(lock);
      while (true) {
        wake.wait(guard, [&] {
          if (stop || !prefetch_wanted.empty()) return true;
          for (const auto& [k, state] : chunks) if (!state.taken) return true;
          return false;
        });
        if (stop) return;
        const key c = chunk_of(camera);
        auto best = chunks.end();
        for (auto it = chunks.begin(); it != chunks.end(); ++it) {
          if (it->second.taken) continue;
          if (best == chunks.end() || distance2(it->first, c) < distance2(best->first, c)) best = it;
        }
        if (best == chunks.end()) {
          // Окно разобрано — опережение: ближайший к камере чанк впереди.
          auto pick = prefetch_wanted.begin();
          for (auto it = prefetch_wanted.begin(); it != prefetch_wanted.end(); ++it) {
            if (distance2(*it, c) < distance2(*pick, c)) pick = it;
          }
          const key k = *pick;
          prefetch_wanted.erase(pick);
          prefetch_done.insert(k);
          guard.unlock();
          const auto p0 = clock::now();
          chain.prefetch("chunk_field", opt.seed, {{uint64_t(int64_t(k[0])), uint64_t(int64_t(k[1])), uint64_t(int64_t(k[2]))}});
          prefetch_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - p0).count());
          guard.lock();
          continue;
        }
        best->second.taken = true;
        const key k = best->first;
        guard.unlock();
        const auto t0 = clock::now();
        const gn09::chunk_instance made = gn09::chunk_from_chain(opt.seed, opt.radius, k[0], k[1], k[2]);
        const gn09::surface_mesh mesh = gn09::surface_from_chain(made);
        const auto t1 = clock::now();
        work_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        shapeless += !made.sampled;
        meshless += mesh.empty();
        guard.lock();
        if (auto found = chunks.find(k); found != chunks.end()) {
          found->second.ready = true;
          found->second.done = t1;
          latencies.push_back(std::chrono::duration<double, std::milli>(t1 - found->second.wanted).count());
        }
        (void)mesh;
      }
    });
  }
  const auto near_missing = [&] {
    const key c = chunk_of(camera);
    int missing = 0;
    for (int z = -near; z <= near; ++z) for (int y = -near; y <= near; ++y) for (int x = -near; x <= near; ++x) {
      const auto found = chunks.find(key{c[0] + x, c[1] + y, c[2] + z});
      missing += found == chunks.end() || !found->second.ready;
    }
    return missing;
  };
  const auto began = clock::now();
  double near_ready_s = -1.0, first_ready_s = -1.0;
  {
    std::unique_lock guard(lock);
    refresh(began);
    refresh_prefetch();
    wake.notify_all();
  }
  // Старт: неподвижная камера, пока ближний куб не готов (не дольше 120 с).
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::unique_lock guard(lock);
    const double t = std::chrono::duration<double>(clock::now() - began).count();
    if (first_ready_s < 0.0 && !latencies.empty()) first_ready_s = t;
    if (near_missing() == 0) { near_ready_s = t; break; }
    if (t > 120.0) break;
  }
  const size_t start_chunks = latencies.size();
  std::vector<double> start_latencies = latencies;
  latencies.clear();
  // Полёт по +x со скоростью opt.flight, кадры по 1/60 с.
  const auto flight_began = clock::now();
  uint32_t frames = 0, holed = 0;
  // Память по ходу полёта (раз в 30 с): живая куча glibc и RSS — их разница — удержание кучей.
  const auto memory_now = [] {
    const struct mallinfo2 info = mallinfo2();
    size_t rss = 0;
    if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
      unsigned long pages = 0, resident = 0;
      if (std::fscanf(f, "%lu %lu", &pages, &resident) == 2) rss = size_t(resident) * size_t(sysconf(_SC_PAGESIZE));
      std::fclose(f);
    }
    return std::pair<size_t, size_t>{info.uordblks + info.hblkhd, rss};
  };
  std::vector<std::pair<size_t, size_t>> memory_trace;
  int worst_missing = 0;
  auto next = flight_began;
  while (true) {
    next += std::chrono::microseconds(16667);
    std::this_thread::sleep_until(next);
    const auto now = clock::now();
    const double t = std::chrono::duration<double>(now - flight_began).count();
    if (t > opt.flight_seconds) break;
    std::unique_lock guard(lock);
    camera = {start.x + opt.flight * t, start.y, start.z};
    refresh(now);
    refresh_prefetch();
    wake.notify_all();
    const int missing = near_missing();
    if (frames % 1800 == 0) memory_trace.push_back(memory_now());
    // Освобождённое сборками возвращается системе раз в 10 с, как в просмотрщике: иначе куча glibc держит
    // пик (сборки выделяют и отпускают буферы под объявленную ёмкость в разных потоках).
    if (frames % 600 == 0) malloc_trim(0);
    ++frames;
    holed += missing > 0;
    worst_missing = std::max(worst_missing, missing);
  }
  {
    std::lock_guard guard(lock);
    stop = true;
  }
  wake.notify_all();
  for (auto& worker : workers) worker.join();
  const auto percentile = [](std::vector<double> values, const double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(q * double(values.size())))];
  };
  std::cout << "GN09 flight seed=" << opt.seed << " speed " << opt.flight << " m/s, " << opt.workers
            << " workers, window 9x9x9, near 5x5x5, lead " << opt.lead << '\n';
  std::cout << "  start: first chunk " << first_ready_s << " s, near cube " << near_ready_s << " s (" << start_chunks
            << " chunks built), latency p50 " << percentile(start_latencies, 0.5) << " ms, max "
            << percentile(start_latencies, 1.0) << " ms\n";
  std::cout << "  flight " << opt.flight_seconds << " s: " << latencies.size() << " chunks ("
            << double(latencies.size()) / opt.flight_seconds << "/s), latency p50 " << percentile(latencies, 0.5)
            << " ms, p90 " << percentile(latencies, 0.9) << " ms, p99 " << percentile(latencies, 0.99) << " ms, max "
            << percentile(latencies, 1.0) << " ms\n";
  std::cout << "  frames with a hole in the near cube: " << holed << " of " << frames << ", worst " << worst_missing
            << " of 125 chunks missing\n";
  std::cout << "  chunks without a surface " << meshless.load() << ", of them solid rock with no shape near (not sampled) "
            << shapeless.load() << '\n';
  std::cout << "  worker time in chunks " << double(work_ns.load()) * 1e-9 << " s, in prefetch "
            << double(prefetch_ns.load()) * 1e-9 << " s (lead " << opt.lead << " layers)\n";
  auto stats = chain.all_stats();
  std::sort(stats.begin(), stats.end(), [](const auto& a, const auto& b) { return a.second.self_ns > b.second.self_ns; });
  std::cout << "  generator            builds   self s  self ms/b   wait s  waits\n";
  for (const auto& [name, st] : stats) {
    if (st.builds == 0 && st.waits == 0) continue;
    std::cout << "  " << std::left << std::setw(20) << name << std::right << std::setw(7) << st.builds << std::fixed
              << std::setprecision(2) << std::setw(9) << double(st.self_ns) * 1e-9 << std::setw(11)
              << (st.builds ? double(st.self_ns) * 1e-6 / double(st.builds) : 0.0) << std::setw(9)
              << double(st.wait_ns) * 1e-9 << std::setw(7) << st.waits << std::defaultfloat << '\n';
  }
  memory_trace.push_back(memory_now());
  std::cout << "  memory every 30 s, live heap / RSS MiB:";
  for (const auto& [live, rss] : memory_trace) std::cout << ' ' << live / (1 << 20) << '/' << rss / (1 << 20);
  std::cout << '\n';
  auto held = chain.cache_footprint();
  std::sort(held.begin(), held.end(), [](const auto& a, const auto& b) { return a.second.buffer_bytes > b.second.buffer_bytes; });
  size_t held_total = 0;
  for (const auto& [name, f] : held) held_total += f.buffer_bytes;
  std::cout << "  cache held: " << double(held_total) / (1 << 20) << " MiB of buffers\n";
  for (const auto& [name, f] : held) {
    if (f.buffer_bytes < (1 << 20)) continue;
    std::cout << "    " << std::left << std::setw(20) << name << std::right << std::setw(6) << f.instances
              << " instances " << std::fixed << std::setprecision(1) << std::setw(9)
              << double(f.buffer_bytes) / (1 << 20) << " MiB" << std::defaultfloat << '\n';
  }
  return 0;
}

} // namespace

// Станции второго поколения на всех базах seed'ов: экземпляры генератора station, независимая проверка.
int station_stats(const options& opt) {
  uint32_t bases = 0, none = 0, refused = 0;
  std::array<uint32_t, 3> poses{};
  // Предпочтение рецепта (0 стена, 1 пол, 2 свод) -> что построилось (3 — ничего).
  std::array<std::array<uint32_t, 4>, 3> preferred{};
  uint64_t rooms = 0, inner = 0, voxels = 0, triangles = 0, glass = 0, glazed = 0;
  const auto start = std::chrono::steady_clock::now();
  for (uint32_t i = 0; i < opt.seeds; ++i) {
    const gn09::adventure world = gn09::generate(opt.seed + i, {}, opt.radius);
    for (const gn09::node& at : world.route.nodes) {
      if (!gn09::is_base(at.kind)) continue;
      ++bases;
      const int8_t axis = gn09::preferred_dock_axis(world.seed, at);
      const uint32_t wish = axis == 0 ? 0u : axis < 0 ? 1u : 2u;
      const auto station = gn09::station_for(world, at.id);
      ++preferred[wish][station ? uint32_t(station->plan.site.pose) : 3u];
      if (!station) { ++none; continue; }
      const gn09::adventure context = gn09::canonical_context(world.seed, at.centre, opt.radius);
      const std::string cause = gn09::verify_station_plan(context, station->anchor, station->plan);
      if (!cause.empty()) {
        ++refused;
        std::cerr << "seed " << world.seed << " base " << at.id << ": " << cause << '\n';
        continue;
      }
      ++poses[uint8_t(station->plan.site.pose)];
      rooms += station->plan.rooms.size();
      for (const gn09::station_room& r : station->plan.rooms) inner += r.inner;
      uint64_t panes = 0;
      for (size_t c = 0; c < station->voxels.solid.size(); ++c) {
        voxels += station->voxels.solid[c] != 0;
        panes += station->voxels.solid[c] != 0 && station->voxels.shade[c] == gn09::station_glass_shade;
      }
      glass += panes;
      glazed += panes != 0;
      triangles += station->mesh.size();
    }
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  const uint32_t built = poses[0] + poses[1] + poses[2];
  std::cout << "bases " << bases << ": built " << built << " (wall " << poses[0] << ", floor "
            << poses[1] << ", ceiling " << poses[2] << "), no site or programme did not fit " << none
            << ", verify refused " << refused << '\n'
            << "rooms per station " << (built ? double(rooms) / built : 0.0) << ", inner "
            << (built ? double(inner) / built : 0.0) << ", solid voxels " << (built ? voxels / built : 0)
            << ", triangles " << (built ? triangles / built : 0) << ", glazed stations " << glazed
            << " (" << glass << " glass cells), " << seconds << " s\n";
  static constexpr const char* pose_names[3] = {"wall", "floor", "ceiling"};
  for (uint32_t w = 0; w < 3; ++w) {
    std::cout << "preferred " << pose_names[w] << ": built wall " << preferred[w][0] << ", floor "
              << preferred[w][1] << ", ceiling " << preferred[w][2] << ", none " << preferred[w][3] << '\n';
  }
  return refused ? 1 : 0;
}

int main(const int argc, char** argv) try {
  options opt;
  if (auto code = parse(argc, argv, opt)) return *code;
  if (opt.flight > 0.0) return flight_bench(opt);
  if (opt.bench) return bench(opt);
  if (opt.station_stats) return station_stats(opt);
  if (opt.verify && (opt.sector_x != 0 || opt.sector_z != 0)) {
    std::cerr << "--sector is for inspecting a window; --verify uses its own windows\n";
    return 2;
  }
  if (opt.verify) {
    uint64_t total_rooms = 0;
    uint64_t total_wide_percent = 0;
    uint32_t halls = 0, lairs = 0, stacked = 0, pillars = 0, niches = 0, overhangs = 0;
    size_t fewest_entrances = SIZE_MAX, most_entrances = 0;
    uint32_t lords = 0, beasts = 0, lord_refuges = 0, beast_passages = 0, beast_reach = 0, shared = 0;
    double lowest_node = 1e30, highest_node = -1e30, steepest = 0.0;
    // Вертикальность по рёбрам и узлам: доля крутых рёбер (подъём больше пробега), рёбер с
    // перепадом больше 30 м, средний перепад и разброс узлов относительно рельефа слоя.
    uint64_t edge_count = 0, steep_edges = 0, tall_edges = 0, node_count = 0;
    double total_rise = 0.0, layer_spread = 0.0;
    uint32_t least_wide_percent = 100;
    std::array<uint64_t, 3> station_poses{};
    uint64_t without_station = 0, station_rooms = 0, station_inner = 0, station_chamfered = 0, station_service = 0;
    double station_trunk = 0.0, station_trunk_max = 0.0;
    uint64_t hall_outposts = 0, tunnels = 0, caves = 0, sites_without_cave = 0;
    uint64_t cave_pockets = 0, cave_shelters = 0, cave_plugs = 0, nest_caves = 0;
    std::array<uint64_t, 4> cave_soft{};
    uint64_t stations_checked = 0, stations_breached = 0, tunnels_checked = 0, tunnels_breached = 0;
    uint64_t cave_entrances = 0, cave_chambers = 0, cave_dead_ends = 0, cave_blocking = 0;
    uint64_t cave_vestibules = 0, cave_second_entrances = 0, cave_loops = 0, cave_pillars = 0;
    double scale_sum = 0.0, budget_sum = 0.0, estimate_sum = 0.0, collect_sum = 0.0;
    uint64_t deposit_sum = 0;
    double tour_sum = 0.0, tour_min = 1e30, tour_max = 0.0;
    std::array<uint64_t, 4> lair_kinds{};
    double cave_depth_sum = 0.0, cave_depth_min = 1e30, goal_depth_sum = 0.0, goal_depth_min = 1e30;
    uint64_t goals = 0, lucky = 0;
    std::optional<uint64_t> tunnel_seed; // первое окно с туннелем — для порчи
    for (uint32_t i = 0; i < opt.seeds; ++i) {
      const gn09::adventure world = gn09::generate(opt.seed + i, {}, opt.radius);
      const gn09::verification result = gn09::verify(world);
      if (!result.ok) {
        std::cerr << "seed " << world.seed << " refused: " << result.cause << '\n';
        return 1;
      }
      total_rooms += result.total_rooms;
      without_station += result.bases_without_station;
      tunnels += result.tunnels;
      caves += result.caves;
      sites_without_cave += result.sites_without_cave;
      for (size_t k = 0; k < cave_soft.size(); ++k) cave_soft[k] += result.cave_soft[k];
      stations_checked += result.stations;
      stations_breached += result.stations_breached;
      tunnels_checked += result.tunnels;
      tunnels_breached += result.tunnels_breached;
      for (const gn09::node& at : world.route.nodes) {
        if (at.kind != gn09::theme::grotto && at.kind != gn09::theme::vent) continue;
        const gn09::site local = gn09::materialize_site(world, at.id);
        if (!local.has_cave) continue;
        for (const gn09::room& r : local.rooms) {
          cave_pockets += r.purpose.starts_with("pocket:");
          cave_shelters += r.purpose.starts_with("shelter:");
        }
        cave_plugs += local.plugs.size();
        cave_entrances += local.entrances.size();
        cave_second_entrances += local.entrances.size() > 1 ? local.entrances.size() - 1 : 0;
        cave_loops += local.links.size() + 1 > local.rooms.size() ? local.links.size() + 1 - local.rooms.size() : 0;
        cave_pillars += local.pillars;
        for (const gn09::room& r : local.rooms) cave_vestibules += r.purpose == "vestibule";
        ++lair_kinds[local.lair_kind % 4];
        for (const gn09::room& r : local.rooms) {
          cave_chambers += r.radius > gn09::cave_core(world.clearance_radius) + 0.01 && r.purpose != "main_chamber";
          cave_dead_ends += r.purpose == "dead_end";
        }
        cave_blocking += local.blocks_sub;
        scale_sum += local.scale;
        budget_sum += local.budget;
        estimate_sum += local.estimate;
        collect_sum += local.collect * local.deposits;
        deposit_sum += local.deposits;
        const double tour = gn09::cave_tour_length(local);
        tour_sum += tour;
        tour_min = std::min(tour_min, tour);
        tour_max = std::max(tour_max, tour);
        cave_depth_sum += gn09::cave_room_depth(local, local.lair);
        cave_depth_min = std::min(cave_depth_min, gn09::cave_room_depth(local, local.lair));
        for (uint32_t r = 0; r < local.rooms.size(); ++r) {
          const bool goal = local.rooms[r].purpose.starts_with("pocket:") ||
                            (local.recipe == gn09::site_recipe::nest_cave && r == local.nest);
          if (!goal) continue;
          const double depth = gn09::cave_room_depth(local, r);
          goal_depth_sum += depth;
          goal_depth_min = std::min(goal_depth_min, depth);
          ++goals;
          // Повезло: по прямой от узла лодки цель ближе радиосвязи.
          lucky += std::hypot(local.rooms[r].centre.x - local.centre.x, local.rooms[r].centre.y - local.centre.y,
                              local.rooms[r].centre.z - local.centre.z) < local.radio_target;
        }
        nest_caves += local.recipe == gn09::site_recipe::nest_cave;
      }
      if (result.tunnels && !tunnel_seed) tunnel_seed = world.seed;
      for (const gn09::node& at : world.route.nodes) {
        const auto station = gn09::station_for(world, at.id);
        if (!station) continue;
        ++station_poses[uint8_t(station->plan.site.pose)];
        station_trunk += station->plan.site.rock_distance;
        station_trunk_max = std::max(station_trunk_max, station->plan.site.rock_distance);
        station_rooms += station->plan.rooms.size();
        for (const gn09::station_room& r : station->plan.rooms) {
          station_inner += r.inner;
          station_chamfered += r.chamfer[0] || r.chamfer[1] || r.chamfer[2] || r.chamfer[3];
          station_service += r.purpose == "service_bay";
        }
      }
      // Залы одного «своего» региона на seed: по нему ведётся статистика уровня.
      const std::vector<gn09::hall> own = gn09::region_halls(world.seed, 0, 0);
      // Аванпосты этих залов: окно 2x2 сектора почти никогда не накрывает уступ зала.
      for (const gn09::hall& h : own) {
        for (const gn09::station_anchor& anchor : gn09::hall_station_anchors(world.seed, h)) {
          const auto station = gn09::station_for(world, anchor);
          if (!station) continue;
          ++hall_outposts;
          if (const std::string cause = gn09::verify_built_station(world, *station); !cause.empty()) {
            std::cerr << "seed " << world.seed << " hall outpost " << anchor.key << " refused: " << cause << '\n';
            return 1;
          }
        }
      }
      halls += uint32_t(own.size());
      stacked += own.size() == 2;
      for (const gn09::hall& h : own) {
        lairs += h.kind == gn09::hall_kind::lair;
        pillars += uint32_t(h.pillars.size());
        niches += uint32_t(h.niches.size());
        for (const gn09::hall_niche& niche : h.niches) overhangs += niche.overhang;
        const gn09::creature_presence presence = gn09::creature_of(world.seed, h);
        lords += h.resident == gn09::creature_class::lord;
        beasts += h.resident == gn09::creature_class::beast;
        shared += presence.halls.size() > 1;
        if (h.resident == gn09::creature_class::lord) lord_refuges += uint32_t(presence.refuges.size());
        if (h.resident == gn09::creature_class::beast) {
          beast_passages += uint32_t(presence.passages.size());
          beast_reach += uint32_t(presence.nodes.size());
        }
        const size_t entrances = gn09::hall_entrances(world.seed, h).size();
        fewest_entrances = std::min(fewest_entrances, entrances);
        most_entrances = std::max(most_entrances, entrances);
      }
      for (const gn09::node& at : world.route.nodes) {
        const double above = at.centre.y - gn09::layer_height(world.seed, at.centre.x, at.centre.z);
        layer_spread += above * above;
        ++node_count;
        lowest_node = std::min(lowest_node, at.centre.y);
        highest_node = std::max(highest_node, at.centre.y);
      }
      for (const gn09::edge link : world.route.edges) {
        const gn09::point a = world.route.nodes[link.a].centre, b = world.route.nodes[link.b].centre;
        const double run = std::hypot(a.x - b.x, a.z - b.z);
        const double rise = std::abs(a.y - b.y);
        if (run > 1.0) steepest = std::max(steepest, rise / run);
        ++edge_count;
        steep_edges += rise > run;
        tall_edges += rise > 30.0;
        total_rise += rise;
      }
      total_wide_percent += result.wide_route_percent;
      least_wide_percent = std::min(least_wide_percent, result.wide_route_percent);
    }
    // Проверяем сами сторожа: место на конце ветки и ветка >5 должны быть отвергнуты
    // при сохранении корректных степеней и связности графа.
    const gn09::adventure reference = gn09::generate(opt.seed, {}, opt.radius);
    // Окна примеров 4x4 сектора: часть секторов занимают залы, и в 2x2 баз мало.
    // Пример — первый seed, у которого в окне есть все три позы стыковки.
    const gn09::adventure examples = [&] {
      for (uint64_t seed = 0;; ++seed) {
        gn09::adventure candidate = gn09::generate(seed, {0, 0, 4, 4}, opt.radius);
        std::array<bool, 3> faces{};
        for (const gn09::node& at : candidate.route.nodes) {
          if (const auto station = gn09::station_for(candidate, at.id)) faces[uint8_t(station->plan.site.pose)] = true;
        }
        if (seed == 64 || std::count(faces.begin(), faces.end(), true) >= 2) return candidate;
      }
    }();
    const gn09::adventure other_recipes = gn09::generate(1, {0, 0, 4, 4}, opt.radius);
    std::array<bool, 6> seen_recipes{};
    for (const gn09::adventure* sample : {&examples, &other_recipes}) {
      for (const gn09::node& at : sample->route.nodes) {
        if (at.kind == gn09::theme::empty) continue;
        const gn09::site local = gn09::materialize_site(*sample, at.id);
        seen_recipes[uint32_t(local.recipe)] = true;
      }
    }
    if (std::find(seen_recipes.begin(), seen_recipes.end(), false) != seen_recipes.end()) {
      std::cerr << "seed-0/1 examples do not cover all local recipes\n";
      return 1;
    }
    uint32_t example_pockets = 0;
    for (const gn09::edge link : examples.route.edges) {
      example_pockets += gn09::materialize_edge_pocket(examples.seed,
        examples.route.nodes[link.a], examples.route.nodes[link.b]).has_value();
    }
    if (example_pockets == 0 || example_pockets >= examples.route.edges.size()) {
      std::cerr << "decorative edge pockets are not occasional\n";
      return 1;
    }
    std::array<bool, 3> seen_faces{};
    for (const gn09::node& at : examples.route.nodes) {
      if (const auto station = gn09::station_for(examples, at.id)) seen_faces[uint8_t(station->plan.site.pose)] = true;
    }
    if (std::count(seen_faces.begin(), seen_faces.end(), true) < 2) {
      std::cerr << "no example window within 64 seeds contains stations in two poses\n";
      return 1;
    }
    std::array<bool, 3> seen_biomes{};
    for (int z = 0; z < 4; ++z) for (int x = 0; x < 4; ++x) {
      const auto biome = gn09::biome_at(0, {double(192 * x + 96), 0,
                                              double(192 * z + 96)});
      seen_biomes[uint32_t(biome)] = true;
    }
    if (std::find(seen_biomes.begin(), seen_biomes.end(), false) != seen_biomes.end()) {
      std::cerr << "regional cave biomes do not cover all three profiles\n";
      return 1;
    }
    // Порча станции: каждую должна отвергнуть независимая проверка плана.
    const auto station_at = std::find_if(examples.route.nodes.begin(), examples.route.nodes.end(),
      [&](const gn09::node& at) {
        const auto station = gn09::station_for(examples, at.id);
        if (!station) return false;
        bool outer = false, inner = false;
        for (const gn09::station_room& r : station->plan.rooms) {
          outer = outer || (!r.inner && r.purpose != "airlock" && r.purpose != "corridor" &&
                            r.purpose != "entry_hall" && r.purpose != "guard_post");
          inner = inner || r.inner;
        }
        return outer && inner && station->plan.rooms.size() >= 4;
      });
    if (station_at == examples.route.nodes.end()) {
      std::cerr << "examples have no station with outer and inner rooms\n";
      return 1;
    }
    const gn09::adventure station_context =
      gn09::canonical_context(examples.seed, station_at->centre, opt.radius);
    const uint32_t station_id = uint32_t(std::find_if(station_context.route.nodes.begin(),
      station_context.route.nodes.end(), [&](const gn09::node& n) { return n.key == station_at->key; })
      - station_context.route.nodes.begin());
    const auto good_station = gn09::station_for(examples, station_at->id);
    const gn09::station_plan& good_plan = good_station->plan;
    if (const std::string cause = gn09::verify_station_plan(station_context, station_id, good_plan);
        !cause.empty()) {
      std::cerr << "example station refused before corruption: " << cause << '\n';
      return 1;
    }
    const auto expect_refusal = [&](gn09::station_plan plan, const auto& corrupt,
                                    const std::string_view cause, const char* what) {
      corrupt(plan);
      const std::string got = gn09::verify_station_plan(station_context, station_id, plan);
      if (got != cause) {
        std::cerr << "self-check missed " << what << " (got '" << got << "')\n";
        return false;
      }
      return true;
    };
    if (!expect_refusal(good_plan, [](gn09::station_plan& p) { ++p.doors[0].u0; ++p.doors[0].u1;
          ++p.doors[0].v0; ++p.doors[0].v1; }, "station door does not join two rooms",
          "a station door off its shared wall") ||
        !expect_refusal(good_plan, [](gn09::station_plan& p) {
          p.rooms[2].u0 = p.rooms[1].u0; p.rooms[2].v0 = p.rooms[1].v0;
          p.rooms[2].u1 = p.rooms[1].u1; p.rooms[2].v1 = p.rooms[1].v1; },
          "station rooms share space without a wall", "two station rooms without a wall") ||
        !expect_refusal(good_plan, [](gn09::station_plan& p) {
          for (gn09::station_room& r : p.rooms) {
            if (!r.inner && r.purpose != "airlock" && r.purpose != "corridor" &&
                r.purpose != "entry_hall" && r.purpose != "guard_post") { r.inner = true; break; }
          } }, "inner station room is not enclosed in rock", "an inner room standing in water") ||
        !expect_refusal(good_plan, [](gn09::station_plan& p) {
          p.hatch_v0 = p.rooms[0].v1 + 4; p.hatch_v1 = p.hatch_v0 + 2; },
          "station hatch is off the airlock", "a hatch off the airlock") ||
        !expect_refusal(good_plan, [](gn09::station_plan& p) { p.doors.clear(); },
          "station room is unreachable from the airlock", "a station without doors") ||
        !expect_refusal(good_plan, [](gn09::station_plan& p) {
          gn09::station_room& r = p.rooms.back();
          r.chamfer[0] = uint8_t(std::min(r.u1 - r.u0, r.v1 - r.v0)); },
          "station chamfer is too deep", "a chamfer deeper than its room")) {
      return 1;
    }
    {
      // Причал, сдвинутый в шлюз: корпус лодки занимает место постройки.
      gn09::station_plan blocked = good_plan;
      const gn09::station_room& airlock = blocked.rooms[0];
      blocked.site.berth = gn09::station_to_world(blocked.site, (airlock.u0 + airlock.u1) * 0.25, 1.5,
                                                  (airlock.v0 + airlock.v1) * 0.25);
      const std::string got = gn09::verify_station_plan(station_context, station_id, blocked);
      if (got != "station column meets the route, a turn or the berth" &&
          got != "station berth is not naturally open") {
        std::cerr << "self-check missed a berth inside the airlock (got '" << got << "')\n";
        return 1;
      }
    }
    {
      // Туннель, у которого средний пролёт опущен на 30 м: такой выемки в поле нет,
      // и независимая проверка по итоговому полю обязана это увидеть. Первый туннель
      // ищется по зёрнам с начала.
      std::shared_ptr<const gn09::built_tunnel> sample;
      gn09::adventure tunnel_world;
      // Окно с туннелем из уже проверенных (туннели в кэше), иначе — дальше по seed'ам.
      for (uint64_t probe = tunnel_seed.value_or(opt.seed + opt.seeds);
           probe < opt.seed + opt.seeds + 64 && !sample; ++probe) {
        tunnel_world = gn09::generate(probe, {}, opt.radius);
        for (const gn09::station_anchor& anchor : gn09::station_anchors(tunnel_world)) {
          if ((sample = gn09::tunnel_for(tunnel_world, anchor))) break;
        }
      }
      if (!sample) {
        std::cerr << "self-check cannot find a dry tunnel\n";
        return 1;
      }
      // Настоящий туннель: ресурсная форма чанка должна совпасть с независимой формулой C++
      // у середины пролёта. Без этого четыре первых seed проверяют только пустой список туннелей.
      {
        const gn09::tunnel_piece& piece = sample->pieces[sample->pieces.size() / 2];
        const gn09::point centre = piece.centre;
        const int cx = int(std::floor(centre.x / gn09::chunk::cells));
        const int cy = int(std::floor(centre.y / gn09::chunk::cells));
        const int cz = int(std::floor(centre.z / gn09::chunk::cells));
        const gn09::chunk_instance made = gn09::chunk_from_chain(tunnel_world.seed, opt.radius, cx, cy, cz, true);
        const gn09::field_region host = gn09::prepare_chunk_field(*made.region.world, cx, cy, cz);
        const auto shifted = [&](const gn09::point axis, const double amount) {
          return gn09::point{centre.x + axis.x * amount, centre.y + axis.y * amount, centre.z + axis.z * amount};
        };
        for (const gn09::point p : {centre, shifted(piece.x, 0.5), shifted(piece.y, 0.5),
                                   shifted(piece.z, 0.5), shifted(piece.z, -0.5)}) {
          if (std::abs(gn09::density(made.region, p) - gn09::density(host, p)) > 1e-9 ||
              gn09::rock_at(made.region, p) != gn09::rock_at(host, p)) {
            std::cerr << "self-check chain tunnel field differs from the host at seed " << tunnel_world.seed << '\n';
            return 1;
          }
        }
        if (gn09::rock_at(host, centre) != gn09::rock_kind::station_cut) {
          std::cerr << "self-check tunnel middle is not cut by the tunnel\n";
          return 1;
        }
      }
      gn09::built_tunnel sunk = *sample;
      sunk.pieces[sunk.pieces.size() / 2].centre.y -= 30.0;
      // Опущенный пролёт может попасть и в воду чужой полости (пещера места крупнее у мода) — тогда
      // его ловит раньше проверка породы вокруг; обе причины значат, что порча замечена.
      if (const std::string got = gn09::verify_tunnel(tunnel_world, sunk);
          got != "tunnel interior is not carved" && got != "tunnel is not enclosed in rock") {
        std::cerr << "self-check missed a tunnel span outside its carve (got '" << got << "')\n";
        return 1;
      }
    }
    const auto natural = std::find_if(reference.route.nodes.begin(), reference.route.nodes.end(),
      [&](const gn09::node& at) {
        if (at.kind != gn09::theme::grotto && at.kind != gn09::theme::vent) return false;
        const gn09::site local = gn09::materialize_site(reference, at.id);
        return local.has_cave && !(gn09::verify_cave(reference, local).soft & gn09::cave_check::nest_exposed);
      });
    if (natural == reference.route.nodes.end()) {
      std::cerr << "self-check cannot find a natural site with a cave\n";
      return 1;
    }
    const gn09::site natural_site = gn09::materialize_site(reference, natural->id);
    {
      // Гнездо, перенесённое к устью: проверка обязана это заметить — как мягкое
      // нарушение, у пещеры, где его не было.
      gn09::site exposed = natural_site;
      exposed.rooms[exposed.nest].centre = exposed.rooms[exposed.mouth].centre;
      const gn09::cave_check got = gn09::verify_cave(reference, exposed);
      if (!(got.soft & gn09::cave_check::nest_exposed)) {
        std::cerr << "self-check missed a cave nest at the mouth (refusal '" << got.refusal << "')\n";
        return 1;
      }
    }
    gn09::local_structure misplaced_feature = gn09::materialize_local_structure(reference, natural->id);
    misplaced_feature.features[0].centre.x += 100.0;
    if (gn09::verify_local_structure(reference, natural_site, misplaced_feature) !=
        "local feature lies outside its chamber") {
      std::cerr << "self-check missed a feature outside its chamber\n";
      return 1;
    }
    gn09::adventure clipped_turn = reference;
    clipped_turn.route.nodes[clipped_turn.route.entry].centre.y = gn09::world_y_max() - 1.0;
    if (gn09::verify(clipped_turn).cause != "node exceeds finite Y submarine turning clearance") {
      std::cerr << "self-check missed a clipped submarine turning space\n";
      return 1;
    }
    if (reference.halls.empty()) {
      std::cerr << "self-check window has no hall\n";
      return 1;
    }
    const gn09::hall& sample_hall = *std::max_element(reference.halls.begin(), reference.halls.end(),
      [](const gn09::hall& a, const gn09::hall& b) { return a.niches.size() < b.niches.size(); });
    gn09::hall out_of_world = sample_hall;
    out_of_world.centre.y = gn09::world_y_max() - 10.0;
    if (gn09::verify_hall(reference.seed, out_of_world, opt.radius) !=
        "hall exceeds the finite world height") {
      std::cerr << "self-check missed a hall outside the world height\n";
      return 1;
    }
    // Зал, поднятый над слоем, перестаёт быть частью маршрута.
    gn09::hall detached = sample_hall;
    const double lift = 2.0 * detached.half_height + 120.0;
    detached.centre.y += detached.centre.y + lift + 200.0 < gn09::world_y_max() ? lift : -lift;
    if (gn09::verify_hall(reference.seed, detached, opt.radius) !=
        "hall has fewer than two route entrances") {
      std::cerr << "self-check missed a hall detached from the route\n";
      return 1;
    }
    // Ниша, повёрнутая прямо в стену по нормали, видна с коридора существ.
    if (sample_hall.niches.empty()) {
      std::cerr << "self-check hall has no niches\n";
      return 1;
    }
    gn09::hall exposed = sample_hall;
    // Ниша, продолжающая луч от ближайшей станции петли через вход: дно видно по оси.
    gn09::hall_niche& niche = exposed.niches.front();
    gn09::point station{};
    double best = 1e30;
    for (const gn09::point p : gn09::loop_stations(exposed)) {
      const double d = std::hypot(p.x - niche.mouth.x, p.y - niche.mouth.y, p.z - niche.mouth.z);
      if (d < best) { best = d; station = p; }
    }
    const gn09::point ray{niche.mouth.x - station.x, niche.mouth.y - station.y, niche.mouth.z - station.z};
    const double ray_length = std::sqrt(ray.x * ray.x + ray.y * ray.y + ray.z * ray.z);
    const auto along = [&](const double t) {
      return gn09::point{niche.mouth.x + ray.x / ray_length * t, niche.mouth.y + ray.y / ray_length * t,
                         niche.mouth.z + ray.z / ray_length * t};
    };
    niche.elbow = along(30.0);
    niche.end = along(55.0);
    if (gn09::verify_hall(reference.seed, exposed, opt.radius) !=
        "hall niche is visible from the creature loop") {
      std::cerr << "self-check missed a niche visible from the creature loop\n";
      return 1;
    }
    // Повелитель: честная запись проходит, а домен, закрывший обход, отвергается.
    bool found_lord = false;
    for (uint64_t seed = 0; seed < 64 && !found_lord; ++seed) {
      for (const gn09::hall& lord : gn09::region_halls(seed, 0, 0)) {
        if (found_lord || lord.resident != gn09::creature_class::lord) continue;
        const gn09::creature_presence honest = gn09::creature_of(seed, lord);
        if (honest.refuges.size() < 2) continue;
        found_lord = true;
        const std::string cause = gn09::verify_creature(seed, lord, honest);
        if (!cause.empty()) {
          std::cerr << "honest lord presence refused: " << cause << '\n';
          return 1;
        }
        gn09::creature_presence closed = honest;
        closed.nodes.insert(closed.nodes.end(), honest.refuges.begin(), honest.refuges.end());
        if (gn09::verify_creature(seed, lord, closed) != "creature domain leaves no detour around the hall") {
          std::cerr << "self-check missed a lord domain that closes the detour: " << gn09::verify_creature(seed, lord, closed) << "\n";
          return 1;
        }
      }
    }
    if (!found_lord) {
      std::cerr << "no lord hall with a detour among 64 seeds\n";
      return 1;
    }
    for (const double extreme : {0.5, 3.0}) {
      const gn09::verification extreme_result = gn09::verify(gn09::generate(opt.seed, {}, extreme));
      if (!extreme_result.ok) {
        std::cerr << "R=" << extreme << " refused: " << extreme_result.cause << '\n';
        return 1;
      }
    }
    gn09::adventure close_bases = reference;
    close_bases.route.edges.push_back({0, 1});
    ++close_bases.route.nodes[0].rank;
    ++close_bases.route.nodes[1].rank;
    if (gn09::verify(close_bases).cause != "bases have fewer than two intermediate nodes") {
      std::cerr << "self-check missed an adjacent pair of bases: " << gn09::verify(close_bases).cause << "\n";
      return 1;
    }
    const auto leaf = std::find_if(reference.route.nodes.begin(), reference.route.nodes.end(),
                                   [](const gn09::node& at) {
                                     return at.kind == gn09::theme::empty && at.rank == 1;
                                   });
    if (leaf == reference.route.nodes.end()) {
      std::cerr << "self-check cannot find an empty dead-end leaf\n";
      return 1;
    }
    const uint32_t leaf_id = leaf->id;
    gn09::adventure misplaced = reference;
    misplaced.route.nodes[leaf_id].kind = gn09::theme::grotto;
    if (gn09::verify(misplaced).cause != "non-base site is not between two bases") {
      std::cerr << "self-check missed a non-base site outside base-to-base routes\n";
      return 1;
    }
    gn09::adventure long_branch = reference;
    long_branch.route.nodes[leaf_id].rank = 2;
    uint32_t previous = leaf_id;
    for (uint32_t step = 0; step < 5; ++step) {
      const uint32_t id = uint32_t(long_branch.route.nodes.size());
      const gn09::point last = long_branch.route.nodes[previous].centre;
      long_branch.route.nodes.push_back({id, 0xfeed0000ull + id,
                                         {last.x + 8.0, last.y, last.z + (step % 2 ? -8.0 : 8.0)},
                                         gn09::theme::empty, uint8_t(step == 4 ? 1 : 2)});
      long_branch.route.edges.push_back({previous, id});
      previous = id;
    }
    if (gn09::verify(long_branch).cause != "non-base dead end exceeds five nodes") {
      std::cerr << "self-check missed a six-node non-base dead end\n";
      return 1;
    }
    const gn09::surface_seam_result surface = gn09::verify_surface_seam(reference);
    if (!surface.ok) {
      std::cerr << "surface seam refused: " << surface.cause << '\n';
      return 1;
    }
    const gn09::surface_seam_result station_surface = gn09::verify_outpost_surface_seam(reference);
    if (!station_surface.ok) {
      std::cerr << "outpost surface seam refused: " << station_surface.cause << '\n';
      return 1;
    }
    const gn09::adventure left_window = gn09::generate(opt.seed, {-2, -2, 3, 3}, opt.radius);
    const gn09::adventure right_window = gn09::generate(opt.seed, {-1, -2, 4, 3}, opt.radius);
    bool found_window_seam = false;
    for (const gn09::edge link : left_window.route.edges) {
      const gn09::point a = left_window.route.nodes[link.a].centre;
      const gn09::point b = left_window.route.nodes[link.b].centre;
      if ((a.x - gn09::sector_size()) * (b.x - gn09::sector_size()) >= 0.0) continue;
      const double t = (gn09::sector_size() - a.x) / (b.x - a.x);
      const double z = a.z + (b.z - a.z) * t;
      if (z < 0.0 || z >= gn09::sector_size()) continue;
      const double y = a.y + (b.y - a.y) * t;
      const int cy = int(std::floor(y / gn09::chunk::cells));
      const int cz = int(std::floor(z / gn09::chunk::cells));
      const gn09::chunk left = gn09::generate_chunk(left_window, 5, cy, cz);
      const gn09::chunk right = gn09::generate_chunk(right_window, 6, cy, cz);
      uint32_t open_face = 0;
      bool equal = true;
      for (int iz = 0; iz <= gn09::chunk::cells; ++iz) {
        for (int iy = 0; iy <= gn09::chunk::cells; ++iy) {
          const double l = left.at(gn09::chunk::cells, iy, iz);
          const double r = right.at(0, iy, iz);
          equal = equal && l == r;
          open_face += l < 0.0;
        }
      }
      if (!equal || open_face == 0) {
        std::cerr << "independent graph windows disagree on an occupied chunk face\n";
        return 1;
      }
      found_window_seam = true;
      break;
    }
    if (!found_window_seam) {
      std::cerr << "self-check cannot find a route crossing a graph-window seam\n";
      return 1;
    }
    const gn09::point observer{1200, 0, -900};
    const gn09::adventure broad = gn09::generate_for_view(opt.seed, observer, 64.0, opt.radius);
    const gn09::adventure shifted = gn09::generate_for_view(opt.seed,
                                                             {observer.x + 120, 0, observer.z + 120}, 64.0,
                                                             opt.radius);
    const gn09::verification broad_result = gn09::verify(broad);
    if (!broad_result.ok) {
      std::cerr << "large view graph refused: " << broad_result.cause << '\n';
      return 1;
    }
    const auto covers = [&](const gn09::adventure& world) {
      const auto& a = world.route.area;
      return a.min_x * gn09::sector_size() < observer.x - 64.0 - gn09::sector_size() &&
             a.max_x * gn09::sector_size() > observer.x + 64.0 + gn09::sector_size() &&
             a.min_z * gn09::sector_size() < observer.z - 64.0 - gn09::sector_size() &&
             a.max_z * gn09::sector_size() > observer.z + 64.0 + gn09::sector_size();
    };
    if (!covers(broad)) {
      std::cerr << "view graph does not extend past the visibility range\n";
      return 1;
    }
    const gn09::node* common = nullptr;
    double best_distance = 1e30;
    for (const gn09::node& candidate : broad.route.nodes) {
      if (!gn09::is_base(candidate.kind)) continue;
      const double dx = candidate.centre.x - observer.x, dz = candidate.centre.z - observer.z;
      const double distance = dx * dx + dz * dz;
      if (distance < best_distance) { common = &candidate; best_distance = distance; }
    }
    if (common == nullptr) return 1;
    const auto matching = std::find_if(shifted.route.nodes.begin(), shifted.route.nodes.end(),
                                        [&](const gn09::node& n) { return n.key == common->key; });
    if (matching == shifted.route.nodes.end() || matching->centre.x != common->centre.x ||
        matching->centre.y != common->centre.y || matching->centre.z != common->centre.z) {
      std::cerr << "overlapping graph windows disagree about a node\n";
      return 1;
    }
    const gn09::site original_site = gn09::materialize_site(broad, common->id);
    const gn09::site repeated_site = gn09::materialize_site(shifted, matching->id);
    if (original_site.rooms.size() != repeated_site.rooms.size() ||
        original_site.portals.size() != repeated_site.portals.size()) {
      std::cerr << "overlapping graph windows disagree about a local site\n";
      return 1;
    }
    for (size_t j = 0; j < original_site.rooms.size(); ++j) {
      const auto& a = original_site.rooms[j];
      const auto& b = repeated_site.rooms[j];
      if (a.centre.x != b.centre.x || a.centre.y != b.centre.y ||
          a.centre.z != b.centre.z || a.purpose != b.purpose) {
        std::cerr << "overlapping graph windows disagree about site rooms\n";
        return 1;
      }
    }
    const auto same_station = [](const std::shared_ptr<const gn09::built_station>& a,
                                 const std::shared_ptr<const gn09::built_station>& b) {
      if (!a || !b) return !a && !b;
      const gn09::station_plan& x = a->plan;
      const gn09::station_plan& y = b->plan;
      if (x.site.yaw != y.site.yaw || x.site.origin.x != y.site.origin.x ||
          x.site.origin.y != y.site.origin.y || x.site.origin.z != y.site.origin.z ||
          x.rooms.size() != y.rooms.size() || x.doors.size() != y.doors.size() ||
          a->mesh.size() != b->mesh.size()) return false;
      for (size_t j = 0; j < x.rooms.size(); ++j) {
        if (x.rooms[j].u0 != y.rooms[j].u0 || x.rooms[j].v0 != y.rooms[j].v0 ||
            x.rooms[j].u1 != y.rooms[j].u1 || x.rooms[j].v1 != y.rooms[j].v1 ||
            x.rooms[j].purpose != y.rooms[j].purpose) return false;
      }
      return true;
    };
    if (!same_station(gn09::station_for(broad, common->id), gn09::station_for(shifted, matching->id))) {
      std::cerr << "overlapping graph windows disagree about a station\n";
      return 1;
    }
    // Станция — экземпляр цепочки по ключу якоря, поэтому окна совпадают по построению. Проверяем
    // другое: пересборка мимо кэша даёт те же выходы побайтно.
    {
      const gn09::node& base = *station_at;
      const auto key = gn09::station_key(gn09::anchor_of(examples, base.id));
      if (const std::string differs = gn09::world_chain(opt.radius).verify("station", examples.seed, key); !differs.empty()) {
        std::cerr << "station differs when rebuilt past the chain cache: " << differs << '\n';
        return 1;
      }
    }
    const gn09::adventure small_window = gn09::generate(opt.seed, {}, opt.radius);
    const gn09::adventure large_window = gn09::generate(opt.seed, {-2, -2, 3, 3}, opt.radius);
    for (const gn09::node& base : small_window.route.nodes) {
      if (!gn09::is_base(base.kind)) continue;
      const auto other = std::find_if(large_window.route.nodes.begin(), large_window.route.nodes.end(),
        [&](const gn09::node& candidate) { return candidate.key == base.key; });
      if (other == large_window.route.nodes.end()) return 1;
      if (!same_station(gn09::station_for(small_window, base.id), gn09::station_for(large_window, other->id))) {
        std::cerr << "station changes when graph window grows around it\n";
        return 1;
      }
    }
    const int chunk_x = int(observer.x) / gn09::chunk::cells;
    const int chunk_z = int(std::floor(observer.z / gn09::chunk::cells));
    if (gn09::generate_chunk(broad, chunk_x, 0, chunk_z).samples !=
        gn09::generate_chunk(shifted, chunk_x, 0, chunk_z).samples) {
      std::cerr << "overlapping graph windows disagree about a visible chunk\n";
      return 1;
    }
    // Зал, видимый из двух разных окон, одинаков и даёт те же сэмплы чанка в нём.
    const gn09::hall* shared_hall = nullptr;
    for (const gn09::hall& a : broad.halls) {
      for (const gn09::hall& b : shifted.halls) {
        if (a.key == b.key && shared_hall == nullptr) {
          if (a.centre.x != b.centre.x || a.centre.y != b.centre.y || a.centre.z != b.centre.z ||
              a.niches.size() != b.niches.size() || a.pillars.size() != b.pillars.size()) {
            std::cerr << "overlapping graph windows disagree about a hall\n";
            return 1;
          }
          shared_hall = &a;
        }
      }
    }
    if (shared_hall == nullptr) {
      std::cerr << "self-check windows share no hall\n";
      return 1;
    }
    {
      const int hx = int(std::floor(shared_hall->centre.x / gn09::chunk::cells));
      const int hy = int(std::floor(shared_hall->centre.y / gn09::chunk::cells));
      const int hz = int(std::floor(shared_hall->centre.z / gn09::chunk::cells));
      if (gn09::generate_chunk(broad, hx, hy, hz).samples !=
          gn09::generate_chunk(shifted, hx, hy, hz).samples) {
        std::cerr << "overlapping graph windows disagree about a chunk inside a hall\n";
        return 1;
      }
    }
    std::cout << "GN09 halls: " << halls << " in region 0,0 over " << opt.seeds << " seeds ("
              << lairs << " lairs, " << halls - lairs << " pillared with " << pillars
              << " pillars, " << stacked << " regions with two halls; " << niches
              << " hidden niches, " << overhangs << " under an overhang; route entrances "
              << fewest_entrances << ".." << most_entrances << " per hall)\n"
              << "GN09 creatures: " << lords << " lords (" << lord_refuges
              << " detour refuges at their entrances), " << beasts << " beasts ("
              << beast_passages << " passages their bodies fit, " << beast_reach
              << " route nodes in their domains), " << shared << " domains spanning several halls\n"
              << "GN09 relief: graph nodes from y=" << lowest_node << " to y=" << highest_node
              << ", steepest edge rise/run " << steepest << "; edges steeper than 45 deg "
              << 100.0 * double(steep_edges) / double(edge_count) << "%, rising over 30 m "
              << 100.0 * double(tall_edges) / double(edge_count) << "%, mean rise "
              << total_rise / double(edge_count) << " m; nodes vs layer RMS "
              << std::sqrt(layer_spread / double(node_count)) << " m\n";
    const uint64_t stations = station_poses[0] + station_poses[1] + station_poses[2];
    std::cout << "GN09 stations: " << stations << " built (wall " << station_poses[0] << ", floor "
              << station_poses[1] << ", ceiling " << station_poses[2] << "), " << without_station
              << " bases without a site; " << double(station_rooms) / double(std::max<uint64_t>(stations, 1))
              << " rooms per station, " << double(station_inner) / double(std::max<uint64_t>(stations, 1))
              << " inner, " << station_chamfered << " rooms with 45-degree corners; airlock corridor mean "
              << station_trunk / double(std::max<uint64_t>(stations, 1)) << " m (max " << station_trunk_max << "), "
              << station_service << " service branches; " << hall_outposts
              << " hall outposts on ledges of region 0,0 halls; " << tunnels << " dry tunnels between stations of the windows\n";
    std::cout << "GN09 caves: " << caves << " diver caves at natural sites (" << nest_caves << " nest caves), "
              << sites_without_cave << " sites without room for one; " << cave_pockets << " resource pockets, "
              << cave_shelters << " shelters, " << cave_plugs << " future ice plugs; along the cave: first monsters mean "
              << cave_depth_sum / double(std::max<uint64_t>(caves, 1)) << " m (min " << cave_depth_min
              << "), goals mean " << goal_depth_sum / double(std::max<uint64_t>(goals, 1)) << " m (min " << goal_depth_min
              << "); " << lucky << " of " << goals << " goals within radio range of the boat in a straight line\n";
    const double cave_count = double(std::max<uint64_t>(caves, 1));
    std::cout << "GN09 cave layouts: " << cave_entrances << " entrances (" << cave_second_entrances
              << " second entrances), " << cave_blocking << " throats narrower than the hull; " << cave_vestibules
              << " vestibules, " << cave_loops << " loops, " << cave_pillars << " pillars; " << cave_chambers << " niches (incl. lairs/nests, deposit niches), "
              << cave_dead_ends << " empty dead ends, passage scale mean " << scale_sum / cave_count
              << "; monsters in a niche " << lair_kinds[0] << ", an empty dead end " << lair_kinds[2]
              << "; tour of all goals and back mean " << tour_sum / cave_count << " m (" << tour_min << ".." << tour_max
              << "); swim budget mean " << budget_sum / cave_count << " s, estimate mean " << estimate_sum / cave_count
              << " s, " << deposit_sum << " deposits, collecting mean "
              << collect_sum / double(std::max<uint64_t>(deposit_sum, 1)) << " s each\n";
    // Обещания пещер мягкие: нарушение допустимо, пока оно у меньшинства пещер.
    static constexpr const char* soft_names[4] = {"lair closer than 0.4 of the deepest", "time estimate off budget by half",
                                                  "nest near the mouth", "cave runs into route or hall water"};
    std::cout << "GN09 cave soft promises broken:";
    bool too_many = false;
    for (size_t k = 0; k < cave_soft.size(); ++k) {
      std::cout << ' ' << soft_names[k] << ' ' << cave_soft[k] << (k + 1 < cave_soft.size() ? "," : "");
      too_many = too_many || 4 * cave_soft[k] > caves;
    }
    std::cout << " of " << caves << " caves\n";
    // Станции пещер не ждут: внутреннюю комнату может вскрыть пещера места. Мягко, как у пещер.
    std::cout << "GN09 station rooms breached by a place cave: " << stations_breached << " of " << stations_checked
              << " station checks, tunnels " << tunnels_breached << " of " << tunnels_checked << '\n';
    if (4 * stations_breached > stations_checked && stations_checked != 0) {
      std::cerr << "a cave breaches a quarter of stations or more\n";
      return 1;
    }
    if (too_many) {
      std::cerr << "a cave promise is broken by a quarter of caves or more\n";
      return 1;
    }
    std::cout << "GN09 verified " << opt.seeds << " seeds, " << total_rooms
              << " reachable rooms; wide route min=" << least_wide_percent
              << "% mean=" << (total_wide_percent / opt.seeds)
              << "%; spherical R=" << opt.radius
              << " m, submarine=" << gn09::starter_sub_length() << "x" << gn09::starter_sub_beam() << " m along edges and turns, base spacing >=2, bends, "
                 "vertical routes, dead ends <=5, "
                 "3D profiles, overlapping windows and chunk/surface seams agree ("
              << surface.shared_vertices << " cave + " << station_surface.shared_vertices
              << " station polygons cut by chunks); eighteen corruptions refused\n";
    return 0;
  }

  // Визуализатору графа нужно окно шире двух секторов: связи видны на масштабе зала.
  const int span = opt.graph_html.empty() ? 2 : int(opt.graph_sectors);
  const gn09::adventure world = gn09::generate(opt.seed,
    {opt.sector_x, opt.sector_z, opt.sector_x + span, opt.sector_z + span}, opt.radius);
  const gn09::verification result = gn09::verify(world);
  report(world, result);
  if (!result.ok) return 1;
  if (!opt.svg.empty()) {
    if (!gn09::write_svg(world, opt.svg)) {
      std::cerr << "cannot write SVG: " << opt.svg << '\n';
      return 1;
    }
    std::cout << "SVG: " << opt.svg << '\n';
  }
  if (!opt.graph_html.empty()) {
    if (!gn09::write_graph_html(world, opt.graph_html)) {
      std::cerr << "cannot write graph HTML: " << opt.graph_html << '\n';
      return 1;
    }
    std::cout << "graph HTML: " << opt.graph_html << '\n';
  }
  if (!opt.site_svg.empty()) {
    if (!gn09::write_site_svg(world, opt.site_id, opt.site_svg)) {
      std::cerr << "cannot write site SVG: " << opt.site_svg << '\n';
      return 1;
    }
    std::cout << "site SVG: " << opt.site_svg << '\n';
  }
  if (!opt.station_svg.empty()) {
    if (!gn09::write_station_svg(world, opt.site_id, opt.station_svg)) {
      std::cerr << "cannot write station SVG: " << opt.station_svg << '\n';
      return 1;
    }
    std::cout << "station SVG: " << opt.station_svg << '\n';
  }
  if (!opt.structure_svg.empty()) {
    if (!gn09::write_structure_svg(world, opt.site_id, opt.structure_svg)) {
      std::cerr << "cannot write structure SVG: " << opt.structure_svg << '\n';
      return 1;
    }
    std::cout << "structure SVG: " << opt.structure_svg << '\n';
  }
  return 0;
} catch (const std::exception& error) {
  std::cerr << "GN09: " << error.what() << '\n';
  return 1;
}
