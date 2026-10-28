#include "adventure.h"
#include "local_structure.h"
#include "surface.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct options {
  uint64_t seed = 0;
  uint32_t seeds = 64;
  double radius = 1.5;
  bool verify = false;
  std::string svg;
  std::string site_svg;
  std::string structure_svg;
  uint32_t site_id = 0;
  int sector_x = 0, sector_z = 0;
};

bool number(const std::string_view text, uint64_t& out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc() && end == text.data() + text.size();
}

bool sector_number(const std::string_view text, int& out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc() && end == text.data() + text.size() &&
         out >= -100000 && out <= 100000;
}

bool real_number(const std::string_view text, double& out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc() && end == text.data() + text.size() &&
         std::isfinite(out) && out >= 0.5 && out <= 3.0;
}

bool parse(const int argc, char** argv, options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--verify") { out.verify = true; continue; }
    if (arg == "--help") {
      std::cout << "GN09_local_adventure [--seed=N] [--radius=R] [--verify] [--seeds=N] [--svg=PATH] "
                   "[--sector=X,Z] [--site=N --site-svg=PATH --structure-svg=PATH]\n";
      return false;
    }
    const auto value = [&](const std::string_view prefix) {
      return arg.starts_with(prefix) ? arg.substr(prefix.size()) : std::string_view{};
    };
    uint64_t parsed = 0;
    if (arg.starts_with("--sector=")) {
      const std::string_view pair = arg.substr(9);
      const size_t comma = pair.find(',');
      if (comma != std::string_view::npos &&
          sector_number(pair.substr(0, comma), out.sector_x) &&
          sector_number(pair.substr(comma + 1), out.sector_z)) continue;
    }
    if (arg.starts_with("--seed=") && number(value("--seed="), parsed)) { out.seed = parsed; continue; }
    if (arg.starts_with("--radius=") && real_number(value("--radius="), out.radius)) continue;
    if (arg.starts_with("--seeds=") && number(value("--seeds="), parsed) && parsed > 0 &&
        parsed <= 10000) { out.seeds = uint32_t(parsed); continue; }
    if (arg.starts_with("--svg=") && !value("--svg=").empty()) {
      out.svg = value("--svg=");
      continue;
    }
    if (arg.starts_with("--site-svg=") && !value("--site-svg=").empty()) {
      out.site_svg = value("--site-svg=");
      continue;
    }
    if (arg.starts_with("--structure-svg=") && !value("--structure-svg=").empty()) {
      out.structure_svg = value("--structure-svg=");
      continue;
    }
    if (arg.starts_with("--site=") && number(value("--site="), parsed) && parsed <= UINT32_MAX) {
      out.site_id = uint32_t(parsed);
      continue;
    }
    std::cerr << "unknown or invalid option: " << arg << '\n';
    return false;
  }
  return true;
}

void report(const gn09::adventure& world, const gn09::verification& result) {
  uint32_t bases = 0, empty = 0;
  for (const gn09::node& at : world.route.nodes) {
    bases += gn09::is_base(at.kind);
    empty += at.kind == gn09::theme::empty;
  }
  std::cout << "GN09 seed=" << world.seed << " R=" << world.clearance_radius
            << " m graph=" << world.route.nodes.size() << " nodes/"
            << world.route.edges.size() << " edges, entry=" << world.route.entry
            << " destination=" << world.route.destination << ", bases=" << bases
            << " empty=" << empty << "\n";
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
    std::cout << "  site " << local.node_id << " " << gn09::name(local.kind) << " at ("
              << local.centre.x << ',' << local.centre.y << ',' << local.centre.z << ") "
              << local.portals.size()
              << " ports, " << local.links.size()
              << (gn09::is_base(local.kind) ? " cave links, module programme:" : " room links, rooms:");
    for (const gn09::room& chamber : local.rooms) std::cout << ' ' << chamber.purpose;
    std::cout << '\n';
    const gn09::local_structure structure = gn09::materialize_local_structure(world, i);
    std::cout << "    local structure: " << structure.zones.size() << " built zones, "
              << structure.openings.size() << " openings, "
              << structure.ports.size() << " dock ports, "
              << structure.deposits.size() << " resource deposits";
    if (gn09::is_base(structure.kind)) {
      std::cout << ", forward=(" << structure.forward.x << ',' << structure.forward.z << ')';
      std::cout << ", hatch=" << (structure.dock_axis > 0 ? "below_ceiling" :
                                   structure.dock_axis < 0 ? "above_floor" : "side")
                << ", level=" << double(structure.level_cm) / 100.0 << 'm';
      const auto volume = [](const std::vector<gn09::built_solid>& solids) {
        uint64_t total = 0;
        for (const gn09::built_solid& solid : solids) {
          total += uint64_t(solid.footprint.u1 - solid.footprint.u0) *
                   uint64_t(solid.footprint.v1 - solid.footprint.v0) *
                   uint64_t(solid.y1 - solid.y0);
        }
        return total;
      };
      const auto raw = gn09::materialize_outpost_solids(structure);
      const auto fitted = gn09::fit_outpost_solids(world, structure);
      std::cout << ", 3D solids=" << fitted.size() << ", kept="
                << (volume(raw) ? 100 * volume(fitted) / volume(raw) : 0) << '%';
    }
    std::cout << '\n';
  }
  std::cout << "field: " << result.checked_samples << " checked 3D profile samples, "
            << result.reached_rooms
            << '/' << result.total_rooms << " reachable rooms, longest non-base dead end="
            << result.max_deadend_nodes << " nodes, wide route="
            << result.wide_route_percent << "%, submarine=17x6 m along edges and turns; "
            << (result.ok ? "verified" : result.cause) << '\n';
}

} // namespace

int main(const int argc, char** argv) {
  options opt;
  if (!parse(argc, argv, opt)) return 2;
  if (opt.verify && (opt.sector_x != 0 || opt.sector_z != 0)) {
    std::cerr << "--sector is for inspecting a window; --verify uses its own windows\n";
    return 2;
  }
  if (opt.verify) {
    uint64_t total_rooms = 0;
    uint64_t total_wide_percent = 0;
    uint32_t least_wide_percent = 100;
    for (uint32_t i = 0; i < opt.seeds; ++i) {
      const gn09::adventure world = gn09::generate(opt.seed + i, {}, opt.radius);
      const gn09::verification result = gn09::verify(world);
      if (!result.ok) {
        std::cerr << "seed " << world.seed << " refused: " << result.cause << '\n';
        return 1;
      }
      total_rooms += result.total_rooms;
      total_wide_percent += result.wide_route_percent;
      least_wide_percent = std::min(least_wide_percent, result.wide_route_percent);
    }
    // Проверяем сами сторожа: место на конце ветки и ветка >5 должны быть отвергнуты
    // при сохранении корректных степеней и связности графа.
    const gn09::adventure reference = gn09::generate(opt.seed, {}, opt.radius);
    const gn09::adventure examples = gn09::generate(0, {}, opt.radius);
    std::array<bool, 3> seen_faces{};
    for (const gn09::node& at : examples.route.nodes) {
      if (!gn09::is_base(at.kind)) continue;
      const gn09::local_structure structure = gn09::materialize_local_structure(examples, at.id);
      seen_faces[structure.dock_axis + 1] = true;
    }
    if (std::find(seen_faces.begin(), seen_faces.end(), false) != seen_faces.end()) {
      std::cerr << "seed-0 examples do not contain side, upper and lower docking faces\n";
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
    const gn09::site built_site = gn09::materialize_site(reference, reference.route.entry);
    gn09::local_structure broken_hatch = gn09::materialize_local_structure(reference, reference.route.entry);
    ++broken_hatch.openings[0].span.u0;
    ++broken_hatch.openings[0].span.u1;
    if (gn09::verify_local_structure(reference, built_site, broken_hatch) !=
        "outpost opening does not lie on a shared wall") {
      std::cerr << "self-check missed a displaced outpost hatch\n";
      return 1;
    }
    const auto vertical = std::find_if(examples.route.nodes.begin(), examples.route.nodes.end(),
      [&](const gn09::node& at) {
        return gn09::is_base(at.kind) &&
          gn09::materialize_local_structure(examples, at.id).dock_axis != 0;
      });
    gn09::local_structure broken_level = gn09::materialize_local_structure(examples,
                                                                            vertical->id);
    broken_level.level_cm += 100;
    if (gn09::verify_local_structure(examples,
          gn09::materialize_site(examples, vertical->id), broken_level) !=
        "outpost has an invalid docking face") {
      std::cerr << "self-check missed a displaced vertical docking hatch\n";
      return 1;
    }
    gn09::local_structure broken_port = gn09::materialize_local_structure(reference, reference.route.entry);
    broken_port.ports[0].edge_index = UINT32_MAX;
    if (gn09::verify_local_structure(reference, built_site, broken_port) !=
        "outpost dock port disagrees with a graph link") {
      std::cerr << "self-check missed a dock port detached from the graph\n";
      return 1;
    }
    const gn09::local_structure base_structure = gn09::materialize_local_structure(reference,
                                                                                   reference.route.entry);
    std::vector<gn09::built_solid> broken_solids = gn09::fit_outpost_solids(reference, base_structure);
    broken_solids.push_back({{-900, -100, -700, 100}, -100, 100, 103});
    if (!gn09::verify_outpost_solids(reference, base_structure, broken_solids)
          .starts_with("outpost solid intersects submarine turn")) {
      std::cerr << "self-check missed a solid across submarine turning space\n";
      return 1;
    }
    std::vector<gn09::built_solid> blocked_dock =
      gn09::fit_outpost_solids(reference, base_structure);
    if (base_structure.dock_axis == 0) {
      const int32_t berth_v = base_structure.zones[1].box.v0 + 200;
      const int32_t far_v = berth_v + (berth_v > 0 ? 900 : -1000);
      blocked_dock.push_back({{600, far_v, 700, far_v + 100}, -100, 100, 103});
    } else {
      const int32_t y = base_structure.dock_axis * 500;
      blocked_dock.push_back({{-50, 950, 50, 1050}, y - 50, y + 50, 103});
    }
    if (gn09::verify_outpost_solids(reference, base_structure, blocked_dock) !=
        "outpost solid intersects docking sweep") {
      std::cerr << "self-check missed a solid across the docking approach\n";
      return 1;
    }
    const auto natural = std::find_if(reference.route.nodes.begin(), reference.route.nodes.end(),
      [](const gn09::node& at) { return at.kind == gn09::theme::grotto || at.kind == gn09::theme::vent; });
    if (natural == reference.route.nodes.end()) {
      std::cerr << "self-check cannot find a natural local site\n";
      return 1;
    }
    const gn09::site natural_site = gn09::materialize_site(reference, natural->id);
    gn09::local_structure misplaced_deposit = gn09::materialize_local_structure(reference, natural->id);
    misplaced_deposit.deposits[0].centre.x += 100.0;
    if (gn09::verify_local_structure(reference, natural_site, misplaced_deposit) !=
        "resource deposit lies outside its chamber") {
      std::cerr << "self-check missed a resource outside its chamber\n";
      return 1;
    }
    gn09::adventure clipped_turn = reference;
    clipped_turn.route.nodes[clipped_turn.route.entry].centre.y = gn09::world_y_max - 1.0;
    if (gn09::verify(clipped_turn).cause != "node exceeds finite Y submarine turning clearance") {
      std::cerr << "self-check missed a clipped submarine turning space\n";
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
      std::cerr << "self-check missed an adjacent pair of bases\n";
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
      if ((a.x - gn09::sector_size) * (b.x - gn09::sector_size) >= 0.0) continue;
      const double t = (gn09::sector_size - a.x) / (b.x - a.x);
      const double z = a.z + (b.z - a.z) * t;
      if (z < 0.0 || z >= gn09::sector_size) continue;
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
      return a.min_x * gn09::sector_size < observer.x - 64.0 - gn09::sector_size &&
             a.max_x * gn09::sector_size > observer.x + 64.0 + gn09::sector_size &&
             a.min_z * gn09::sector_size < observer.z - 64.0 - gn09::sector_size &&
             a.max_z * gn09::sector_size > observer.z + 64.0 + gn09::sector_size;
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
    const gn09::local_structure original_plan = gn09::materialize_local_structure(broad, common->id);
    const gn09::local_structure repeated_plan = gn09::materialize_local_structure(shifted, matching->id);
    const auto same_rect = [](const gn09::plan_rect a, const gn09::plan_rect b) {
      return a.u0 == b.u0 && a.v0 == b.v0 && a.u1 == b.u1 && a.v1 == b.v1;
    };
    if (original_plan.forward.x != repeated_plan.forward.x ||
        original_plan.forward.z != repeated_plan.forward.z ||
        original_plan.zones.size() != repeated_plan.zones.size() ||
        original_plan.openings.size() != repeated_plan.openings.size() ||
        original_plan.ports.size() != repeated_plan.ports.size()) {
      std::cerr << "overlapping graph windows disagree about an outpost plan\n";
      return 1;
    }
    for (size_t j = 0; j < original_plan.zones.size(); ++j) {
      if (!same_rect(original_plan.zones[j].box, repeated_plan.zones[j].box) ||
          original_plan.zones[j].purpose != repeated_plan.zones[j].purpose) {
        std::cerr << "overlapping graph windows disagree about outpost zones\n";
        return 1;
      }
    }
    for (size_t j = 0; j < original_plan.openings.size(); ++j) {
      if (!same_rect(original_plan.openings[j].span, repeated_plan.openings[j].span)) {
        std::cerr << "overlapping graph windows disagree about outpost openings\n";
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
      const gn09::local_structure a = gn09::materialize_local_structure(small_window, base.id);
      const gn09::local_structure b = gn09::materialize_local_structure(large_window, other->id);
      if (a.forward.x != b.forward.x || a.forward.z != b.forward.z ||
          a.zones.size() != b.zones.size() ||
          !same_rect(a.zones[1].box, b.zones[1].box)) {
        std::cerr << "outpost turns when graph window grows around it\n";
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
    std::cout << "GN09 verified " << opt.seeds << " seeds, " << total_rooms
              << " reachable rooms; wide route min=" << least_wide_percent
              << "% mean=" << (total_wide_percent / opt.seeds)
              << "%; spherical R=" << opt.radius
              << " m, submarine=17x6 m along edges and turns, base spacing >=2, bends, "
                 "vertical routes, dead ends <=5, "
                 "3D profiles, overlapping windows and chunk/surface seams agree ("
              << surface.shared_vertices << " cave + " << station_surface.shared_vertices
              << " outpost shared surface vertices); ten corruptions refused\n";
    return 0;
  }

  const gn09::adventure world = gn09::generate(opt.seed,
    {opt.sector_x, opt.sector_z, opt.sector_x + 2, opt.sector_z + 2}, opt.radius);
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
  if (!opt.site_svg.empty()) {
    if (!gn09::write_site_svg(world, opt.site_id, opt.site_svg)) {
      std::cerr << "cannot write site SVG: " << opt.site_svg << '\n';
      return 1;
    }
    std::cout << "site SVG: " << opt.site_svg << '\n';
  }
  if (!opt.structure_svg.empty()) {
    if (!gn09::write_structure_svg(world, opt.site_id, opt.structure_svg)) {
      std::cerr << "cannot write structure SVG: " << opt.structure_svg << '\n';
      return 1;
    }
    std::cout << "structure SVG: " << opt.structure_svg << '\n';
  }
  return 0;
}
