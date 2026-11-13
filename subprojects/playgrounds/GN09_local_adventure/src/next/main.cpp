#include "devils_engine/playground/cli.h"
#include "runtime.h"
#include "devils_engine/originator/script_column.h"
#include "devils_engine/utils/prng.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <bit>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <vector>
#include <stdexcept>
using namespace gn09_next;
using clock_type = std::chrono::steady_clock;
void require(bool condition, const char* text) {
  if (!condition) throw std::runtime_error(text);
}
void verify_profile_continuity(runtime& world) {
  // A deliberately asymmetric test section. This is a continuity property, not
  // a second implementation of the gallery formula.
  const std::map<std::string, double> row{
    {"a_x", 0}, {"a_y", 0}, {"a_z", 0}, {"b_x", 20}, {"b_y", 0}, {"b_z", 0}, {"right_x", 0}, {"right_y", 0}, {"right_z", 1}, {"up_x", 0}, {"up_y", 1}, {"up_z", 0}, {"length2", 400}, {"width_a", 10}, {"width_b", 10}, {"roof_a", 8}, {"roof_b", 8}, {"floor_a", 4}, {"floor_b", 4}, {"relief", 0}, {"phase", 0}};
  std::vector<std::string> names;
  std::vector<double> scalars;
  for (const auto& [name, value] : row) {
    names.push_back(name);
    scalars.push_back(value);
  }
  const std::vector<std::string> columns{"x", "y", "z", "terrain_broad", "terrain_detail"};
  const auto program = o::column_program::compile("gallery continuity", world.shape_source("passages"), columns, names);
  o::column_workspace workspace;
  auto value = [&](double x, double y, double z) {
    double zero = 0, result = 0;
    const std::array<const double*, 5> data{&x, &y, &z, &zero, &zero};
    program->run(1, data, scalars, &result, workspace);
    return result;
  };
  for (double x : {-2.0, 10.0, 22.0})
    for (double z : {0.0, 9.5}) {
      const double below = value(x, -1e-6, z), above = value(x, 1e-6, z), middle = value(x, 0, z);
      require(std::abs(below - above) < 1e-4 && std::abs(middle - (below + above) / 2) < 1e-4, "asymmetric gallery has a discontinuity at its middle plane");
    }
}
void verify_ocean_floor(runtime& world) {
  const std::vector<std::string> names{"centre_x", "centre_y", "centre_z", "half_x", "half_y", "half_z", "depth", "relief"};
  const std::vector<double> scalars{0, 0, 0, 235, 800, 210, 70, 0};
  const std::vector<std::string> columns{"x", "y", "z", "terrain_broad", "terrain_detail"};
  const auto program = o::column_program::compile("ocean floor", world.shape_source("ocean"), columns, names);
  o::column_workspace workspace;
  auto value = [&](double x, double y) {
    double zero = 0, result = 0;
    const std::array<const double*, 5> data{&x, &y, &zero, &zero, &zero};
    program->run(1, data, scalars, &result, workspace);
    return result;
  };
  double previous = -1e30;
  for (double x : {0.0, 80.0, 160.0, 220.0}) {
    require(value(x, -120) > 0 && value(x, 0) < 0, "ocean floor disappeared");
    double low = -120, high = 0;
    for (int i = 0; i < 40; ++i) {
      const double mid = (low + high) / 2;
      if (value(x, mid) > 0)
        low = mid;
      else
        high = mid;
    }
    const double floor = (low + high) / 2;
    require(floor >= previous - 1e-6, "ocean floor does not rise towards the cave coast");
    previous = floor;
  }
}
void verify_biomes(runtime& world, uint64_t seed) {
  size_t probes = 0;
  for (int z = 1; z >= -1; --z)
    for (int x = 1; x >= -1; --x) {
      const auto window = world.chain().get("biome_window", seed, key(x, z));
      const auto p = window->field("biome_pixels", "position"), region = window->field("biome_pixels", "region"), blend = window->field("biome_pixels", "blend");
      const size_t side = size_t(std::sqrt(p.count()));
      const auto neighbour = world.chain().get("biome_window", seed, key(x + 1, z));
      for (size_t row = 0; row < side; ++row) {
        const size_t a = row * side + side - 1, b = row * side;
        for (uint32_t axis = 0; axis < 2; ++axis)
          require(region.get_integer(a, axis) == neighbour->field("biome_pixels", "region").get_integer(b, axis), "biome window seam changed owner");
        require(std::abs(blend.get(a) - neighbour->field("biome_pixels", "blend").get(b)) < 1e-12, "biome window seam changed blend");
      }
      for (size_t iz = 0; iz < side; iz += 8)
        for (size_t ix = 0; ix < side; ix += 8) {
          const size_t at = iz * side + ix;
          const auto sample = world.chain().get("biome_query", seed, key(int64_t(std::llround(p.get(at, 0))), int64_t(std::llround(p.get(at, 2)))));
          for (uint32_t axis = 0; axis < 2; ++axis)
            require(region.get_integer(at, axis) == sample->field("biome_sample", "region").get_integer(0, axis), "biome query halo omitted the nearest site");
          require(std::abs(blend.get(at) - sample->field("biome_sample", "blend").get(0)) < 1e-12, "biome query halo omitted the second nearest site");
          o::field_vector point{p.get(at, 0), 0, p.get(at, 2)};
          const auto owner = world.chain().field("biome_domain", seed, key(region.get_integer(at, 0), region.get_integer(at, 1)), "cell");
          require(owner->value(point).value < 0, "DS biome clipping disagrees with the Lua biome map");
          if (blend.get(at) < 1e-9) {
            const auto other = sample->field("biome_sample", "other");
            const auto competitor = world.chain().field("biome_domain", seed, key(other.get_integer(0, 0), other.get_integer(0, 1)), "cell");
            require(competitor->value(point).value > 0, "neighbouring biome geometry crosses its declared transition band");
          }
          ++probes;
        }
    }
  require(world.chain().verify("biome_query", seed, key(-384, 384)).empty(), "biome query rebuild differs");
  std::cout << "Streaming biome queries: " << probes << " halo probes, shared borders and rebuild passed\n";
}
void verify_network(runtime& world, uint64_t seed) {
  using id = uint64_t;
  std::map<id, std::vector<id>> adjacency;
  std::map<id, bool> main_nodes;
  std::set<std::pair<id, id>> unique;
  std::map<std::pair<int, int>, std::pair<id, size_t>> declared;
  std::set<size_t> degrees;
  size_t routes = 0, vertical = 0;
  double bottom = 1e30, top = -1e30;
  for (int z = -4; z <= 4; ++z)
    for (int x = -4; x <= 4; ++x) {
      const auto graph = world.chain().get("network", seed, key(x, z));
      const auto keys = graph->field("network_nodes", "key"), is_main = graph->field("network_nodes", "main");
      for (size_t i = 0; i < keys.count(); ++i) {
        const id k = id(keys.get_integer(i));
        const bool main = is_main.get(i) != 0;
        const auto [at, inserted] = main_nodes.emplace(k, main);
        require(inserted || at->second == main, "network node role changed across windows");
      }
      const auto a = graph->field("network_edges", "a"), b = graph->field("network_edges", "b");
      for (size_t i = 0; i < a.count(); ++i) {
        const id ka = id(a.get_integer(i)), kb = id(b.get_integer(i));
        require(ka != kb, "network self edge");
        require(unique.emplace(std::min(ka, kb), std::max(ka, kb)).second, "duplicate network edge owner");
        adjacency[ka].push_back(kb);
        adjacency[kb].push_back(ka);
      }
      const auto ordinary = graph->field("network_routes", "ordinary");
      for (size_t i = 0; i < ordinary.count(); ++i)
        require(ordinary.get(i) >= world.constants().number("route_min_ordinary"), "network route has fewer than N ordinary nodes");
      routes += ordinary.count();
      const auto pa = graph->field("network_paths", "a"), pb = graph->field("network_paths", "b"), pos = graph->field("network_nodes", "centre");
      for (size_t i = 0; i < pos.count(); ++i) {
        bottom = std::min(bottom, pos.get(i, 1));
        top = std::max(top, pos.get(i, 1));
      }
      for (size_t i = 0; i < pa.count(); ++i)
        if (std::hypot(pa.get(i, 0) - pb.get(i, 0), pa.get(i, 2) - pb.get(i, 2)) < 1e-6 && std::abs(pa.get(i, 1) - pb.get(i, 1)) > 40) ++vertical;
      declared[{x, z}] = {id(graph->field("network_node", "key").get_integer(0)), size_t(graph->field("network_node", "degree").get(0))};
    }
  for (int z = -1; z <= 1; ++z)
    for (int x = -1; x <= 1; ++x) {
      const auto [start, degree] = declared.at({x, z});
      require(adjacency[start].size() == degree, "network degree differs from complete neighbouring owners");
      degrees.insert(degree);
      std::map<id, size_t> distance;
      std::vector<id> frontier{start};
      distance[start] = 0;
      for (size_t i = 0; i < frontier.size(); ++i) {
        const auto current = frontier[i];
        for (const id next : adjacency[current])
          if (!distance.contains(next)) {
            distance[next] = distance[current] + 1;
            frontier.push_back(next);
          }
      }
      for (const auto& [address, node] : declared) {
        if (std::abs(address.first) > 1 || std::abs(address.second) > 1) continue;
        require(distance.contains(node.first), "main network is disconnected");
      }
      for (const auto& [node, d] : distance)
        if (node != start && main_nodes.at(node))
          require(d > size_t(world.constants().number("route_min_ordinary")), "a loop shortened a main route below N ordinary nodes");
    }
  require(world.chain().verify("network", seed, key(0, 0)).empty(), "network rebuild differs");
  require(vertical > 0, "network has no vertical shafts");
  std::cout << "Vertical network: " << vertical << " shaft edges, height span " << top - bottom << " m\n";
  std::cout << "Streaming main network: " << routes << " routes, " << main_nodes.size() << " nodes, " << degrees.size() << " observed main degrees; connectivity/N/ownership passed\n";
}
void verify_landscape(runtime& world, uint64_t seed) {
  const auto terrain = world.chain().get("terrain", seed, key(0, 0));
  const auto a = terrain->field("passages", "a"), b = terrain->field("passages", "b");
  require(a.count() > 0, "new landscape has no passages");
  size_t probes = 0;
  for (size_t i = 0; i < a.count(); i += std::max<size_t>(1, a.count() / 8)) {
    o::field_vector p{};
    for (uint32_t axis = 0; axis < 3; ++axis)
      p[axis] = (a.get(i, axis) + b.get(i, axis)) / 2;
    const auto field = world.chain().field("landscape", seed, key(int64_t(std::floor(p[0] / chunk_cells)), int64_t(std::floor(p[1] / chunk_cells)), int64_t(std::floor(p[2] / chunk_cells))), "solid");
    require(field->value(p).value < 0, "new gallery centre is blocked");
    for (uint32_t axis = 0; axis < 3; ++axis)
      for (int sign : {-1, 1}) {
        auto q = p;
        q[axis] += sign * (world.constants().number("sub_beam") / 2 + 1.5);
        require(field->value(q).value < 0, "new gallery hull clearance is blocked");
        ++probes;
      }
  }
  const auto start = world.start(seed, 0, 0);
  const o::chunk_key at{int64_t(std::floor(start[0] / chunk_cells)), int64_t(std::floor(start[1] / chunk_cells)), int64_t(std::floor(start[2] / chunk_cells))};
  const auto chunk = world.chunk(seed, at);
  for (const char* buffer : {"samples", "surface"}) {
    const auto values = chunk->field(buffer, buffer == std::string_view("samples") ? "value" : "position");
    const uint32_t components = buffer == std::string_view("samples") ? 1 : 3;
    for (size_t i = 0; i < values.count(); ++i)
      for (uint32_t axis = 0; axis < components; ++axis)
        require(std::isfinite(values.get(i, axis)), "new landscape produced a non-finite sample or vertex");
  }
  require(world.chain().verify("landscape", seed, key(at.x, at.y, at.z)).empty(), "new landscape rebuild differs");
  std::cout << "New landscape: " << a.count() << " variable-section spans, " << probes << " hull probes, finite chunk and rebuild passed\n";
}
void verify_features(runtime& world, uint64_t seed) {
  size_t anchors = 0, details = 0, heaps = 0, refused = 0, curtains = 0, severed = 0;
  auto inspect = [&](int64_t x, int64_t z, int64_t kind) {
    const auto at = key(x, z, kind);
    const auto instance = world.chain().get("features", seed, at);
    const auto support = world.chain().field("features", seed, at, "support");
    const auto p = instance->field("roots", "position"), d = instance->field("roots", "direction"), radius = instance->field("roots", "radius");
    for (size_t i = 0; i < p.count(); ++i) {
      o::field_vector q{p.get(i, 0), p.get(i, 1), p.get(i, 2)}, direction{d.get(i, 0), d.get(i, 1), d.get(i, 2)};
      o::field_vector right = std::abs(direction[1]) > 0.9 ? o::field_vector{1, 0, 0} : o::field_vector{-direction[2], 0, direction[0]};
      const double length = std::hypot(right[0], right[2]);
      for (double& v : right)
        v /= length;
      const o::field_vector up{right[1] * direction[2] - right[2] * direction[1], right[2] * direction[0] - right[0] * direction[2], right[0] * direction[1] - right[1] * direction[0]};
      require(support->value(q).value > 0.25, "rock feature has no anchor in the underlying geology");
      for (const auto& axis : {right, up})
        for (int sign : {-1, 1}) {
          auto edge = q;
          for (size_t c = 0; c < 3; ++c)
            edge[c] += sign * radius.get(i) * 0.6 * axis[c];
          require(support->value(edge).value > 0.25, "rock feature anchor has insufficient buried support");
        }
    }
    const auto protected_water = world.chain().field("features", seed, at, "protected");
    const auto a = instance->field("details", "a"), b = instance->field("details", "b");
    for (size_t i = 0; i < a.count(); ++i) {
      const double length = std::hypot(b.get(i, 0) - a.get(i, 0), b.get(i, 1) - a.get(i, 1), b.get(i, 2) - a.get(i, 2));
      const int steps = std::max(1, int(std::ceil(length / 2)));
      for (int j = 0; j <= steps; ++j) {
        o::field_vector q{};
        for (uint32_t axis = 0; axis < 3; ++axis)
          q[axis] = a.get(i, axis) + (b.get(i, axis) - a.get(i, axis)) * double(j) / steps;
        require(protected_water->value(q).value > 0, "protected boat water severs a rock feature");
      }
    }
    anchors += p.count();
    details += instance->field("details", "radius").count();
    heaps += instance->field("heaps", "centre").count();
    refused += size_t(instance->field("features_meta", "rejected").get(0));
    curtains += size_t(instance->field("features_meta", "curtains").get(0));
    severed += size_t(instance->field("features_meta", "severed").get(0));
  };
  inspect(0, 0, 0);
  bool biome = false;
  for (int r = 0; r <= 4 && !biome; ++r)
    for (int z = -r; z <= r && !biome; ++z)
      for (int x = -r; x <= r && !biome; ++x) {
        if (std::max(std::abs(x), std::abs(z)) != r) continue;
        const auto cell = world.chain().get("biome", seed, key(x, z));
        if (cell->field("biome", "kind").get(0) >= 2) {
          inspect(x, z, 1);
          biome = true;
        }
      }
  require(anchors > 0 && details > 0, "anchored environment sample is empty");
  require(world.chain().verify("features", seed, key(0, 0, 0)).empty(), "anchored features rebuild differs");
  std::cout << "Anchored environment: " << anchors << " buried roots, " << details << " rock spans, " << curtains << " ridge links, " << heaps << " rubble forms, " << refused << " unsupported and " << severed << " severed proposals refused\n";
}
bool verbose_outposts = false;
void verify_places(runtime& world, uint64_t seed) {
  size_t stations = 0;
  std::array<size_t, 3> poses{};
  double occupancy = 0, trunk = 0;
  std::array<size_t, 5> refusals{};
  double standoff = 0;
  std::array<size_t, 2> transitions{};
  std::array<size_t, 6> labels_seen{};
  std::array<size_t, 3> beacon_kinds{};
  std::array<std::array<size_t, 2>, 3> by_pose{};
  double extent_max = 0, reach_max = 0;
  size_t windows_total = 0, hatches_total = 0;
  size_t rooms_total = 0, triangles_max = 0, walls_max = 0, tower_valid = 0, core_min = SIZE_MAX, core_max = 0, rooms_max = 0;
  for (int z = -1; z <= 1; ++z)
    for (int x = -1; x <= 1; ++x) {
      const auto station = world.chain().get("outpost", seed, key(x, z));
      require(station->field("station_meta", "built").get(0) != 0, "main outpost was not built");
      const double inside = station->field("outpost_meta", "occupancy").get(0);
      require(inside >= 17.0 / 27 - 1e-9, "outpost body is mostly in open water");
      occupancy += inside;
      trunk += station->field("station_meta", "trunk_length").get(0);
      ++poses.at(size_t(station->field("station_meta", "pose").get(0)));
      ++stations;
      refusals[0] += size_t(station->field("outpost_meta", "proposals").get(0));
      standoff += station->field("outpost_meta", "standoff").get(0);
      if (verbose_outposts) {
        const auto o = station->field("station_meta", "origin");
        std::cout << "  outpost " << x << ',' << z << ": pose " << station->field("station_meta", "pose").get(0) << ", yaw "
                  << std::atan2(station->field("station_meta", "yaw_s").get(0), station->field("station_meta", "yaw_c").get(0)) << ", origin "
                  << o.get(0, 0) << ' ' << o.get(0, 1) << ' ' << o.get(0, 2) << ", lock " << station->field("station_meta", "trunk_length").get(0)
                  << " m, standoff " << station->field("outpost_meta", "standoff").get(0) << " m, occupancy " << station->field("outpost_meta", "occupancy").get(0)
                  << ", pairs " << station->field("outpost_meta", "proposals").get(0) << ", " << (station->field("station_meta", "transition").get(0) != 0 ? "tower" : "trunk") << " (towers "
                  << station->field("outpost_meta", "tower_valid").get(0) << '/' << station->field("outpost_meta", "tower_candidates").get(0) << "; buried " << station->field("outpost_meta", "tower_buried").get(0) << ", floating "
                  << station->field("outpost_meta", "tower_floating").get(0) << ", protected " << station->field("outpost_meta", "tower_protected").get(0) << "), core " << station->field("outpost_meta", "core_rooms").get(0) << " (refused: water "
                  << station->field("outpost_meta", "core_water").get(0) << ", protected " << station->field("outpost_meta", "core_protected").get(0) << ", overlap " << station->field("outpost_meta", "core_overlap").get(0) << "), windows " << station->field("station_meta", "windows").get(0) << ", hatches "
                  << station->field("station_meta", "hatches").get(0) << ", beacon " << station->field("station_meta", "beacon_kind").get(0) << ", rooms " << station->field("station_meta", "rooms").get(0) << '\n';
      }
      refusals[1] += size_t(station->field("outpost_meta", "refused_approach").get(0));
      refusals[2] += size_t(station->field("outpost_meta", "refused_dock").get(0));
      refusals[3] += size_t(station->field("outpost_meta", "refused_water").get(0));
      refusals[4] += size_t(station->field("outpost_meta", "refused_protected").get(0));
      const auto dock = station->field("station_meta", "dock");
      const double yaw_c = station->field("station_meta", "yaw_c").get(0), yaw_s = station->field("station_meta", "yaw_s").get(0);
      require(std::abs(yaw_c * yaw_c + yaw_s * yaw_s - 1) < 1e-9, "outpost frame yaw is not a rotation");
      const std::array<double, 3> boat_half{world.constants().number("sub_length") / 2, world.constants().number("sub_beam") / 2, world.constants().number("sub_beam") / 2};
      // Boat frame: local x = (c,0,s), y = up, z = (-s,0,c), centred on the dock.
      auto to_boat = [&](const o::field_vector& p) {
        const double dx = p[0] - dock.get(0, 0), dy = p[1] - dock.get(0, 1), dz = p[2] - dock.get(0, 2);
        return std::array<double, 3>{dx * yaw_c + dz * yaw_s, dy, dz * yaw_c - dx * yaw_s};
      };
      const auto natural = world.chain().field("outpost", seed, key(x, z), "natural");
      for (int sx : {-1, 0, 1})
        for (int sy : {-1, 0, 1})
          for (int sz : {-1, 0, 1}) {
            const double lx = sx * boat_half[0], ly = sy * boat_half[1], lz = sz * boat_half[2];
            const o::field_vector p{dock.get(0, 0) + lx * yaw_c - lz * yaw_s, dock.get(0, 1) + ly, dock.get(0, 2) + lx * yaw_s + lz * yaw_c};
            require(natural->value(p).value < 0, "new wet dock is blocked");
          }
      const auto ma = station->field("station_mesh", "a"), mb = station->field("station_mesh", "b"), mc = station->field("station_mesh", "c");
      for (size_t i = 0; i < ma.count(); ++i) {
        std::array<std::array<double, 3>, 3> local{};
        for (size_t v = 0; v < 3; ++v) {
          const auto& f = v == 0 ? ma : v == 1 ? mb : mc;
          local[v] = to_boat(o::field_vector{f.get(i, 0), f.get(i, 1), f.get(i, 2)});
        }
        bool overlap = true;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const double low = std::min({local[0][axis], local[1][axis], local[2][axis]}), high = std::max({local[0][axis], local[1][axis], local[2][axis]});
          overlap &= high > -boat_half[axis] && low < boat_half[axis];
        }
        require(!overlap, "outpost mesh intrudes into the boat at its wet dock");
      }
      // Rooms are reached from the lock only through passages that exist in the wall records.
      const auto parents = station->field("station_rooms", "parent"), labels = station->field("station_rooms", "label");
      const auto wall_room = station->field("station_walls", "room"), wall_other = station->field("station_walls", "other"), wall_opening = station->field("station_walls", "opening");
      const size_t room_count = parents.count();
      require(room_count > 0 && labels.get(0) == 0 && parents.get(0) < 0, "new outpost does not start with the lock");
      std::vector<std::vector<size_t>> around(room_count);
      for (size_t w = 0; w < wall_room.count(); ++w)
        if (wall_opening.get(w) == 1) {
          require(wall_other.get(w) >= 0 && wall_other.get(w) < double(room_count), "outpost passage leads outside the room list");
          const size_t a = size_t(wall_room.get(w)), b = size_t(wall_other.get(w));
          require(parents.get(a) == double(b) || parents.get(b) == double(a), "outpost passage joins rooms that are not linked");
          around[a].push_back(b);
          around[b].push_back(a);
        }
      std::vector<bool> reached(room_count, false);
      std::vector<size_t> queue{0};
      reached[0] = true;
      for (size_t head = 0; head < queue.size(); ++head)
        for (size_t next : around[queue[head]])
          if (!reached[next]) {
            reached[next] = true;
            queue.push_back(next);
          }
      require(queue.size() == room_count, "outpost room is not reachable from the lock through passages");
      // The lock mouth and the beacon point stand in open water, visible from the approaching boat.
      const auto hatch = station->field("station_meta", "hatch"), beacon = station->field("station_meta", "beacon");
      require(natural->value(o::field_vector{hatch.get(0, 0), hatch.get(0, 1), hatch.get(0, 2)}).value < 0, "outpost lock mouth is buried in rock");
      for (uint32_t a = 0; a < 3; ++a)
        require(std::isfinite(beacon.get(0, a)), "outpost beacon is not finite");
      require(natural->value(o::field_vector{beacon.get(0, 0), beacon.get(0, 1), beacon.get(0, 2)}).value < 0, "outpost beacon is not in open water");
      ++beacon_kinds.at(size_t(station->field("station_meta", "beacon_kind").get(0)));
      windows_total += size_t(station->field("station_meta", "windows").get(0));
      hatches_total += size_t(station->field("station_meta", "hatches").get(0));
      const bool tower = station->field("station_meta", "transition").get(0) != 0;
      ++transitions[tower ? 1 : 0];
      ++by_pose.at(size_t(station->field("station_meta", "pose").get(0)))[tower ? 1 : 0];
      // Influence region: extent against station_reach, distance from the main node against the terrain halo.
      const auto low = station->field("station_meta", "low"), high = station->field("station_meta", "high");
      const auto node = world.chain().get("network", seed, key(x, z))->field("network_node", "centre");
      for (uint32_t a = 0; a < 3; ++a) {
        extent_max = std::max(extent_max, high.get(0, a) - low.get(0, a));
        if (a != 1) reach_max = std::max({reach_max, std::abs(low.get(0, a) - node.get(0, a)), std::abs(high.get(0, a) - node.get(0, a))});
      }
      for (size_t i = 0; i < room_count; ++i)
        ++labels_seen.at(size_t(labels.get(i)));
      rooms_total += room_count;
      triangles_max = std::max(triangles_max, size_t(station->field("station_meta", "triangles").get(0)));
      walls_max = std::max(walls_max, wall_room.count());
      tower_valid += size_t(station->field("outpost_meta", "tower_valid").get(0) > 0);
      const size_t core = size_t(station->field("outpost_meta", "core_rooms").get(0));
      core_min = std::min(core_min, core);
      core_max = std::max(core_max, core);
      rooms_max = std::max(rooms_max, room_count);
    }
  std::cout << "New places: " << stations << " embedded outposts, occupancy " << occupancy / stations << ", trunk " << trunk / stations << " m, poses side/below/above " << poses[0] << '/' << poses[1] << '/' << poses[2]
            << "; dock standoff " << standoff / stations << " m; valid pairs " << refusals[0] << "; refused docks: approach " << refusals[1] << ", blocked " << refusals[2] << "; refused bodies: open water " << refusals[3] << ", protected route " << refusals[4] << '\n';
  std::cout << "Outpost transitions: trunk " << transitions[0] << ", tower " << transitions[1] << " (tower possible at " << tower_valid << "); rooms " << rooms_total << ": lock " << labels_seen[0] << ", dock " << labels_seen[1] << ", guard " << labels_seen[2]
            << ", technical " << labels_seen[3] << ", core " << labels_seen[4] << ", passage " << labels_seen[5] << "; core per outpost " << core_min << ".." << core_max
            << ", max rooms " << rooms_max << "; trunk/tower by pose side " << by_pose[0][0] << '/' << by_pose[0][1] << ", below " << by_pose[1][0] << '/' << by_pose[1][1] << ", above " << by_pose[2][0] << '/' << by_pose[2][1]
            << "; extent " << extent_max << " of " << 2 * world.constants().number("station_reach") << " m, reach from node " << reach_max << " of " << world.constants().number("outpost_search_reach") + 60 << " m; windows " << windows_total << ", emergency hatches " << hatches_total << "; beacon on roof/wall/lock mouth " << beacon_kinds[0] << '/' << beacon_kinds[1] << '/' << beacon_kinds[2] << ", max walls " << walls_max << ", max triangles " << triangles_max << '\n';
}
// Chunks overlapping the nine outposts of a seed: own build cost and surface size. Compare runs with
// outpost_crease_refinement = 1 and 0 to price the refinement of rock creases along the shell.
void outpost_chunks(runtime& world, uint64_t seed) {
  std::set<std::array<int64_t, 3>> keys;
  size_t details = 0;
  for (int z = -1; z <= 1; ++z)
    for (int x = -1; x <= 1; ++x) {
      const auto station = world.chain().get("outpost", seed, key(x, z));
      details += size_t(station->field("station_meta", "details").get(0));
      const auto low = station->field("station_meta", "low"), high = station->field("station_meta", "high");
      std::array<int64_t, 3> a{}, b{};
      for (uint32_t i = 0; i < 3; ++i) {
        a[i] = int64_t(std::floor(low.get(0, i) / chunk_cells));
        b[i] = int64_t(std::floor(high.get(0, i) / chunk_cells));
      }
      for (int64_t cz = a[2]; cz <= b[2]; ++cz)
        for (int64_t cy = a[1]; cy <= b[1]; ++cy)
          for (int64_t cx = a[0]; cx <= b[0]; ++cx)
            keys.insert({cx, cy, cz});
    }
  size_t vertices = 0, worst = 0, structure = 0;
  const auto start = std::chrono::steady_clock::now();
  for (const auto& k : keys) {
    const auto chunk = world.chain().get(world.generator_name(), seed, key(k[0], k[1], k[2]));
    const size_t v = size_t(chunk->field("surface_counts", "vertices").get(0));
    vertices += v;
    worst = std::max(worst, v);
    structure += size_t(chunk->field("surface_counts", "structure").get(0));
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "Outpost chunks, seed " << seed << ": " << keys.size() << " chunks, " << details << " crease boxes; rock surface " << vertices << " vertices (mean " << vertices / std::max<size_t>(1, keys.size()) << ", densest " << worst
            << "), structure " << structure << " vertices; wall " << seconds << " s\n";
  for (const auto& [name, st] : world.chain().all_stats())
    if (st.builds && (name == world.generator_name() || name == "outpost"))
      std::cout << "  " << name << ": " << st.builds << " builds, self " << double(st.self_ns) / 1e6 / st.builds << " ms\n";
}
bool segment_hits_triangle(const o::field_vector& p, const o::field_vector& q, const o::field_vector& a, const o::field_vector& b, const o::field_vector& c) {
  auto sub = [](const auto& x, const auto& y) {
    return o::field_vector{x[0] - y[0], x[1] - y[1], x[2] - y[2]};
  };
  auto cross = [](const auto& x, const auto& y) {
    return o::field_vector{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
  };
  auto dot = [](const auto& x, const auto& y) {
    return x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
  };
  auto d = sub(q, p), e1 = sub(b, a), e2 = sub(c, a), h = cross(d, e2);
  double det = dot(e1, h);
  if (std::abs(det) < 1e-12) return false;
  auto delta = sub(p, a);
  double u = dot(delta, h) / det;
  if (u < 0 || u > 1) return false;
  auto k = cross(delta, e1);
  double v = dot(d, k) / det;
  if (v < 0 || u + v > 1) return false;
  double t = dot(e2, k) / det;
  return t > 1e-6 && t < 1 - 1e-6;
}
void verify_cave_systems(runtime& world, uint64_t seed) {
  size_t systems = 0, loops = 0, slits = 0, halls = 0, spans = 0;
  for (int layer : {-1, 0, 1}) {
    auto cave = world.chain().get("small_cave", seed, key(0, 0, layer));
    const auto p = cave->field("cave_nodes", "centre"), parent = cave->field("cave_nodes", "parent"), degree = cave->field("cave_nodes", "degree");
    require(p.count() >= 3, "cave composition has no connected spaces");
    double low = 1e30, high = -1e30;
    std::vector<size_t> degrees(p.count());
    for (size_t i = 0; i < p.count(); ++i) {
      require(i == 0 ? parent.get(i) == -1 : parent.get(i) >= 0 && parent.get(i) < double(i), "cave tree is disconnected");
      low = std::min(low, p.get(i, 1));
      high = std::max(high, p.get(i, 1));
    }
    const auto a = cave->field("cave_edges", "a"), b = cave->field("cave_edges", "b");
    std::set<std::pair<size_t, size_t>> unique;
    for (size_t i = 0; i < a.count(); ++i) {
      size_t x = size_t(a.get(i)), z = size_t(b.get(i));
      require(x < p.count() && z < p.count() && x != z, "invalid cave edge");
      require(unique.emplace(std::min(x, z), std::max(x, z)).second, "duplicate cave edge");
      ++degrees[x];
      ++degrees[z];
    }
    for (size_t i = 0; i < p.count(); ++i)
      require(degrees[i] == size_t(degree.get(i)), "cave degree differs from graph");
    require(std::isfinite(high - low), "invalid cave height span");
    const auto pa = cave->field("passages", "a"), pb = cave->field("passages", "b"), right = cave->field("passages", "right"), up = cave->field("passages", "up");
    for (size_t i = 0; i < pa.count(); ++i) {
      const double w = std::min(cave->field("passages", "width_a").get(i), cave->field("passages", "width_b").get(i));
      const double h = std::min({cave->field("passages", "roof_a").get(i), cave->field("passages", "roof_b").get(i), cave->field("passages", "floor_a").get(i), cave->field("passages", "floor_b").get(i)});
      require(w > world.constants().number("swimmer_half_width") + 0.05 && h > world.constants().number("swimmer_half_height") + 0.05, "cave cross section does not fit a prone swimmer");
      for (uint32_t axis = 0; axis < 3; ++axis)
        require(std::isfinite(pa.get(i, axis)) && std::isfinite(pb.get(i, axis)) && std::isfinite(right.get(i, axis)) && std::isfinite(up.get(i, axis)), "nonfinite cave geometry");
    }
    if (layer == 0) {
      size_t chosen = 0;
      for (size_t i = 0; i < pa.count(); ++i)
        if (cave->field("passages", "roof_a").get(i) < 0.65) {
          chosen = i;
          break;
        }
      o::field_vector centre{}, tangent{};
      double length = 0;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        centre[axis] = (pa.get(chosen, axis) + pb.get(chosen, axis)) / 2;
        tangent[axis] = pb.get(chosen, axis) - pa.get(chosen, axis);
        length += tangent[axis] * tangent[axis];
      }
      for (double& v : tangent)
        v /= std::sqrt(length);
      const auto at = key(int64_t(std::floor(centre[0] / chunk_cells)), int64_t(std::floor(centre[1] / chunk_cells)), int64_t(std::floor(centre[2] / chunk_cells)));
      const auto chunk = world.chain().get("cave_landscape", seed, at);
      const auto field = world.chain().field("cave_landscape", seed, at, "solid");
      for (double along : {-0.6, 0.0, 0.6})
        for (int j = 0; j < 8; ++j) {
          auto p = centre;
          double angle = j * 3.141592653589793 / 4;
          for (uint32_t axis = 0; axis < 3; ++axis)
            p[axis] += along * tangent[axis] + std::cos(angle) * world.constants().number("swimmer_half_width") * right.get(chosen, axis) + std::sin(angle) * world.constants().number("swimmer_half_height") * up.get(chosen, axis);
          require(field->value(p).value < 0, "cave field blocks the head-first swimmer cross section");
        }
      auto from = centre, to = centre;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        from[axis] -= tangent[axis] * 0.6;
        to[axis] += tangent[axis] * 0.6;
      }
      const auto mesh = chunk->field("surface", "position");

      using packed_point = std::array<int64_t, 3>;
      std::map<std::pair<packed_point, packed_point>, size_t> surface_edges;
      auto packed = [&](size_t index) {
        packed_point p{};
        for (uint32_t axis = 0; axis < 3; ++axis)
          p[axis] = int64_t(std::llround(mesh.get(index, axis) * 1e6));
        return p;
      };
      for (size_t i = 0; i < mesh.count(); i += 3)
        for (size_t j = 0; j < 3; ++j) {
          auto a = packed(i + j), b = packed(i + (j + 1) % 3);
          if (a == b) continue;
          if (b < a) std::swap(a, b);
          ++surface_edges[{a, b}];
        }
      for (const auto& [edge, count] : surface_edges)
        if (count != 2) {
          bool border = false;
          for (uint32_t axis = 0; axis < 3; ++axis) {
            int64_t low = int64_t(at.words[axis]) * chunk_cells * 1000000, high = low + chunk_cells * 1000000;
            border |= (edge.first[axis] == low && edge.second[axis] == low) || (edge.first[axis] == high && edge.second[axis] == high);
          }
          require(border, "adaptive cave surface has an interior open edge");
        }

      for (size_t i = 0; i < mesh.count(); i += 3) {
        o::field_vector a{}, b{}, c{};
        for (uint32_t axis = 0; axis < 3; ++axis) {
          a[axis] = mesh.get(i, axis);
          b[axis] = mesh.get(i + 1, axis);
          c[axis] = mesh.get(i + 2, axis);
        }
        require(!segment_hits_triangle(from, to, a, b, c), "fine mesh seals the narrow cave passage");
      }
      require(chunk->field("props", "position").count() == 0, "cave content was populated during geometry review");
    }
    loops += size_t(cave->field("cave_meta", "loops").get(0));
    slits += size_t(cave->field("cave_meta", "slits").get(0));
    halls += size_t(cave->field("cave_meta", "halls").get(0));
    spans += pa.count();
    ++systems;
    require(cave->field("cave_chambers", "centre").count() > 0, "cave system has no chamber");
    require(world.chain().verify("small_cave", seed, key(0, 0, layer)).empty(), "cave system changes on rebuild");
  }

  std::cout << "Independent caves: " << systems << " systems, " << loops << " loops, " << slits << " tight edges, " << halls << " halls, " << spans << " curved spans; no content/time/boat exclusion rules\n";
}

void verify(runtime& world, uint64_t seed) {
  verify_profile_continuity(world);
  verify_ocean_floor(world);
  verify_biomes(world, seed);
  verify_network(world, seed);
  verify_landscape(world, seed);
  verify_features(world, seed);
  verify_places(world, seed);
  verify_cave_systems(world, seed);
  const auto start = world.start(seed, 0, 0);
  const int64_t cy = int64_t(std::floor(start[1] / chunk_cells)), cz = int64_t(std::floor(start[2] / chunk_cells));
  const int64_t boundary = int64_t(world.constants().number("main_spacing")) / chunk_cells;
  constexpr int padded = chunk_cells + 3;
  // The shared face of two chunks across an address-cell boundary: both chunk fields give one value.
  for (const int64_t x : {int64_t(-1), boundary - 1}) {
    const auto a = world.chain().field("landscape", seed, key(x, cy, cz), "solid"), b = world.chain().field("landscape", seed, key(x + 1, cy, cz), "solid");
    for (int z = 0; z < padded; ++z)
      for (int y = 0; y < padded; ++y) {
        const o::field_vector p{double((x + 1) * chunk_cells), double(cy * chunk_cells - 1 + y), double(cz * chunk_cells - 1 + z)};
        require(std::abs(a->value(p).value - b->value(p).value) < 1e-9, "active terrain seam changed across address cells");
      }
  }
  std::cout << "Verified active slice 2, seed " << seed << ": biome/network/terrain/places/chunk seams passed\n";
}
void map_svg(runtime& world, uint64_t seed, const std::string& path) {
  std::ofstream out(path);
  require(bool(out), "cannot write map");
  out << "<svg xmlns='http://www.w3.org/2000/svg' viewBox='-1200 -1200 2400 2400' width='1000' height='1000'>";
  out << "<rect x='-1200' y='-1200' width='2400' height='2400' fill='#17202a'/>";
  const char* colours[] = {"#344a62", "#536c45", "#65526c", "#244b78"};
  for (int z = -3; z <= 2; ++z)
    for (int x = -3; x <= 2; ++x) {
      const auto pixels = world.chain().get("biome_window", seed, key(x, z));
      const auto p = pixels->field("biome_pixels", "position"), kind = pixels->field("biome_pixels", "kind");
      const size_t side = size_t(std::sqrt(p.count()));
      const double pitch = p.get(1, 0) - p.get(0, 0);
      for (size_t iz = 0; iz + 1 < side; ++iz)
        for (size_t ix = 0; ix + 1 < side; ++ix) {
          const size_t i = iz * side + ix;
          out << "<rect x='" << p.get(i, 0) << "' y='" << p.get(i, 2) << "' width='" << pitch + 0.1 << "' height='" << pitch + 0.1 << "' fill='" << colours[int(kind.get(i))] << "'/>";
        }
    }
  for (int z = -3; z <= 2; ++z)
    for (int x = -3; x <= 2; ++x) {
      const auto graph = world.chain().get("network", seed, key(x, z));
      const auto a = graph->field("network_paths", "a"), b = graph->field("network_paths", "b");
      for (size_t i = 0; i < a.count(); ++i)
        out << "<path d='M " << a.get(i, 0) << ' ' << a.get(i, 2) << " L " << b.get(i, 0) << ' ' << b.get(i, 2) << "' stroke='#e6d6a8' stroke-width='3'/>";
      const auto ordinary = graph->field("network_nodes", "centre"), role = graph->field("network_nodes", "main");
      for (size_t i = 0; i < ordinary.count(); ++i)
        if (role.get(i) == 0)
          out << "<circle cx='" << ordinary.get(i, 0) << "' cy='" << ordinary.get(i, 2) << "' r='4' fill='#f6e7bd' stroke='#17202a' stroke-width='1'/>";
      const auto p = graph->field("network_node", "centre");
      out << "<circle cx='" << p.get(0, 0) << "' cy='" << p.get(0, 2) << "' r='10' fill='#f3a462' stroke='#17202a' stroke-width='3'/>";
    }
  out << "<text x='-1170' y='-1168' fill='white' font-family='sans-serif' font-size='25'>Orange: main outposts; small points: ordinary nodes; lines: graph routes</text>";
  out << "</svg>";
  std::cout << "Map: " << path << '\n';
}
void cave_plan_svg(runtime& world, uint64_t seed, int x, int z, int layer, const std::string& path) {
  auto cave = world.chain().get("small_cave", seed, key(x, z, layer));
  std::ofstream out(path);
  require(bool(out), "cannot write cave plan");
  out << "<svg xmlns='http://www.w3.org/2000/svg' width='1400' height='740' viewBox='0 0 1400 740'><rect width='1400' height='740' fill='#12242d'/>";
  auto p = cave->field("cave_nodes", "centre"), kind = cave->field("cave_nodes", "kind");
  auto centres = cave->field("cave_chambers", "centre"), half = cave->field("cave_chambers", "half");
  auto right = cave->field("cave_chambers", "right"), up = cave->field("cave_chambers", "up"), forward = cave->field("cave_chambers", "forward");
  const char* names[] = {"entry", "chamber", "gallery", "shaft", "alcove", "low hall"};
  for (int panel = 0; panel < 2; ++panel) {
    uint32_t axis = panel == 0 ? 2 : 1;
    double lowx = 1e30, lowy = 1e30, highx = -1e30, highy = -1e30;
    for (size_t i = 0; i < centres.count(); ++i) {
      double radius = std::max({half.get(i, 0), half.get(i, 1), half.get(i, 2)});
      lowx = std::min(lowx, centres.get(i, 0) - radius);
      highx = std::max(highx, centres.get(i, 0) + radius);
      lowy = std::min(lowy, centres.get(i, axis) - radius);
      highy = std::max(highy, centres.get(i, axis) + radius);
    }
    double scale = 590 / std::max({highx - lowx, highy - lowy, 1.0});
    auto px = [&](double v) {
      return panel * 700 + 55 + (v - lowx) * scale;
    };
    auto py = [&](double v) {
      return panel == 0 ? 95 + (v - lowy) * scale : 685 - (v - lowy) * scale;
    };
    out << "<text x='" << panel * 700 + 20 << "' y='32' fill='white' font-family='sans-serif' font-size='20'>seed " << seed << ", system " << x << ',' << z << ',' << layer << (panel == 0 ? " — XZ" : " — XY") << "</text>";
    for (size_t i = 0; i < centres.count(); ++i) {
      double a = 0, b = 0, c = 0;
      const std::array<decltype(right), 3> bases{right, up, forward};
      for (uint32_t k = 0; k < 3; ++k) {
        const auto& basis = bases[k];
        double h = half.get(i, k);
        double vx = basis.get(i, 0) * h, vy = basis.get(i, axis) * h;
        if (panel == 1) vy = -vy;
        a += vx * vx;
        b += vx * vy;
        c += vy * vy;
      }
      double discriminant = std::sqrt((a - c) * (a - c) + 4 * b * b), angle = 0.5 * std::atan2(2 * b, a - c) * 180 / 3.141592653589793;
      out << "<ellipse cx='" << px(centres.get(i, 0)) << "' cy='" << py(centres.get(i, axis)) << "' rx='" << std::sqrt(std::max(0.0, (a + c + discriminant) / 2)) * scale << "' ry='" << std::sqrt(std::max(0.0, (a + c - discriminant) / 2)) * scale << "' transform='rotate(" << angle << ' ' << px(centres.get(i, 0)) << ' ' << py(centres.get(i, axis)) << ")' fill='#568a92' fill-opacity='.25' stroke='#689ba2' stroke-width='1'/>";
    }
    auto a = cave->field("passages", "a"), b = cave->field("passages", "b");
    for (size_t i = 0; i < a.count(); ++i)
      out << "<path d='M " << px(a.get(i, 0)) << ' ' << py(a.get(i, axis)) << " L " << px(b.get(i, 0)) << ' ' << py(b.get(i, axis)) << "' stroke='#9bd6d2' stroke-opacity='.55' stroke-linecap='round' stroke-width='" << 2 * std::min({cave->field("passages", "width_a").get(i), cave->field("passages", "roof_a").get(i), cave->field("passages", "floor_a").get(i)}) * scale << "'/>";
    for (size_t i = 0; i < p.count(); ++i)
      out << "<circle cx='" << px(p.get(i, 0)) << "' cy='" << py(p.get(i, axis)) << "' r='4' fill='#f6bc65'/><text x='" << px(p.get(i, 0)) + 6 << "' y='" << py(p.get(i, axis)) - 5 << "' font-family='sans-serif' font-size='12' fill='white'>" << i << ' ' << names[std::min<size_t>(5, size_t(kind.get(i)))] << "</text>";
  }
  out << "<text x='20' y='725' fill='#c3d6d9' font-family='sans-serif' font-size='14'>Projected cavity envelopes and curved connections; no resources or creatures. Site rock preference: " << cave->field("cave_meta", "site_quality").get(0) << "</text></svg>";
}
// Plan (local XZ) and elevation (local ZY) of one outpost inside a 390 x 390 panel.
void outpost_panel(std::ostream& out, const o::result_ptr& local) {
  // Two projections of the outpost frame: plan (local XZ) above, elevation (local ZY) below.
  const auto p = local->field("station_rooms", "local_centre"), half = local->field("station_rooms", "half"), label = local->field("station_rooms", "label");
  double low[3]{1e30, 1e30, 1e30}, high[3]{-1e30, -1e30, -1e30};
  for (size_t i = 0; i < p.count(); ++i)
    for (uint32_t a = 0; a < 3; ++a) {
      low[a] = std::min(low[a], p.get(i, a) - half.get(i, a));
      high[a] = std::max(high[a], p.get(i, a) + half.get(i, a));
    }
  const double scale = std::min({360 / (high[0] - low[0]), 360 / (high[2] - low[2]), 140 / (high[2] - low[2]), 140 / (high[0] - low[0]), 140 / (high[1] - low[1])});
  static const char* colours[] = {"#c9a25a", "#4fa3d1", "#d1605a", "#8a8fd6", "#4c7a5a", "#9a9a9a"};
  // Panel 0: horizontal = x, vertical = z; panel 1: horizontal = z, vertical = y (up).
  auto project = [&](int panel, double x, double y, double z) {
    if (panel == 0) return std::array<double, 2>{20 + (x - low[0]) * scale, 45 + (z - low[2]) * scale};
    return std::array<double, 2>{20 + (z - low[2]) * scale, 205 + (high[1] - y) * scale};
  };
  for (int panel = 0; panel < 2; ++panel)
    for (size_t i = 0; i < p.count(); ++i) {
      const auto a = project(panel, p.get(i, 0) - half.get(i, 0), p.get(i, 1) + half.get(i, 1), p.get(i, 2) - half.get(i, 2));
      const auto b = project(panel, p.get(i, 0) + half.get(i, 0), p.get(i, 1) - half.get(i, 1), p.get(i, 2) + half.get(i, 2));
      out << "<rect x='" << std::min(a[0], b[0]) << "' y='" << std::min(a[1], b[1]) << "' width='" << std::abs(b[0] - a[0]) << "' height='" << std::abs(b[1] - a[1]) << "' fill='" << colours[std::min<size_t>(5, size_t(label.get(i)))]
          << "' fill-opacity='.3' stroke='" << colours[std::min<size_t>(5, size_t(label.get(i)))] << "'/>";
    }
  auto dot = [&](double x, double y, double z, const char* colour) {
    for (int panel = 0; panel < 2; ++panel) {
      const auto c = project(panel, x, y, z);
      out << "<circle cx='" << c[0] << "' cy='" << c[1] << "' r='2.5' fill='" << colour << "'/>";
    }
  };
  const auto hole = local->field("station_walls", "hole_centre"), opening = local->field("station_walls", "opening");
  for (size_t i = 0; i < hole.count(); ++i)
    if (opening.get(i) == 1) dot(hole.get(i, 0), hole.get(i, 1), hole.get(i, 2), "#81df9b");
  const auto openings = local->field("station_openings", "local_centre"), kind = local->field("station_openings", "kind");
  for (size_t i = 0; i < openings.count(); ++i)
    dot(openings.get(i, 0), openings.get(i, 1), openings.get(i, 2), kind.get(i) == 3 ? "#69bdf1" : "#efaa62");
  out << "<text x='18' y='372' fill='white' font-family='sans-serif' font-size='12'>Plan XZ, elevation ZY; dots: passage/emergency/window</text>";
  out << "<text x='18' y='388' fill='white' font-family='sans-serif' font-size='12'>" << (local->field("station_meta", "transition").get(0) != 0 ? "Tower" : "Trunk") << ", lock " << local->field("station_meta", "trunk_length").get(0) << " m; pose " << local->field("station_meta", "pose").get(0) << "; rooms " << local->field("station_meta", "rooms").get(0)
      << ", windows " << local->field("station_meta", "windows").get(0) << "</text>";
}
// All nine main outposts of a seed, 3 x 3 panels.
void outpost_gallery_svg(runtime& world, uint64_t seed, const std::string& path) {
  std::ofstream out(path);
  require(bool(out), "cannot write outpost gallery");
  out << "<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='1200' viewBox='0 0 1200 1200'><rect width='1200' height='1200' fill='#17202a'/>";
  for (int z = -1; z <= 1; ++z)
    for (int x = -1; x <= 1; ++x) {
      const auto local = world.chain().get("outpost", seed, key(x, z));
      out << "<g transform='translate(" << 400 * (x + 1) << ',' << 400 * (z + 1) << ")'><rect x='5' y='5' width='390' height='390' fill='none' stroke='#586574'/><text x='18' y='30' fill='white' font-family='sans-serif' font-size='18'>Outpost " << x << ',' << z << ", seed " << seed << "</text>";
      outpost_panel(out, local);
      out << "</g>";
    }
  out << "</svg>";
  std::cout << "Outpost gallery: " << path << '\n';
}
void plans_svg(runtime& world, uint64_t seed, const std::string& path) {
  std::ofstream out(path);
  require(bool(out), "cannot write local plans");
  out << "<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='800' viewBox='0 0 1200 800'><rect width='1200' height='800' fill='#17202a'/>";
  struct example {
    o::result_ptr instance;
    int x, z, kind;
  };
  std::vector<example> examples;
  for (int x = -1; x <= 1; ++x)
    examples.push_back({world.chain().get("outpost", seed, key(x, 0)), x, 0, 2});
  const auto start = world.start(seed, 0, 0);
  const auto scope = world.chain().get("small_cave_scope", seed, key(int64_t(std::floor(start[0] / chunk_cells)), int64_t(std::floor(start[1] / chunk_cells)), int64_t(std::floor(start[2] / chunk_cells))));
  const auto keys = scope->field("small_cave_keys", "key");
  for (size_t i = 0; i < keys.count() && examples.size() < 6; ++i) {
    auto cave = world.chain().get("small_cave", seed, key(keys.get_integer(i, 0), keys.get_integer(i, 1), keys.get_integer(i, 2)));
    if (cave->field("cave_meta", "nodes").get(0) != 0) examples.push_back({cave, int(keys.get_integer(i, 0)), int(keys.get_integer(i, 1)), 1});
  }
  std::array<size_t, 3> made{};
  for (const auto& example : examples) {
    const auto local = example.instance;
    const int kind = example.kind, x = example.x, z = example.z;
    const double left = 400 * made[kind]++, top = kind == 1 ? 0 : 400;
    out << "<g transform='translate(" << left << ',' << top << ")'><rect x='5' y='5' width='390' height='390' fill='none' stroke='#586574'/><text x='18' y='30' fill='white' font-family='sans-serif' font-size='18'>" << (kind == 1 ? "Cave" : "Outpost") << ' ' << x << ',' << z << "</text>";
    if (kind == 1) {
      const auto p = local->field("cave_nodes", "centre");
      double low[2]{1e30, 1e30}, high[2]{-1e30, -1e30};
      for (size_t i = 0; i < p.count(); ++i)
        for (uint32_t a = 0; a < 2; ++a) {
          low[a] = std::min(low[a], p.get(i, a) - 2);
          high[a] = std::max(high[a], p.get(i, a) + 2);
        }
      const double scale = 280 / std::max({high[0] - low[0], high[1] - low[1], 1.0});
      auto px = [&](size_t i) {
        return 50 + (p.get(i, 0) - low[0]) * scale;
      };
      auto py = [&](size_t i) {
        return 340 - (p.get(i, 1) - low[1]) * scale;
      };
      auto edges_a = local->field("cave_edges", "a"), edges_b = local->field("cave_edges", "b");
      for (size_t i = 0; i < edges_a.count(); ++i) {
        const size_t a = size_t(edges_a.get(i)), b = size_t(edges_b.get(i));
        out << "<path d='M " << px(a) << ' ' << py(a) << " L " << px(b) << ' ' << py(b) << "' stroke='#8bc6df' stroke-width='2'/>";
      }
      for (size_t i = 0; i < p.count(); ++i)
        out << "<circle cx='" << px(i) << "' cy='" << py(i) << "' r='" << 2 * scale << "' fill='#315b72' stroke='#8bc6df'/><text x='" << px(i) << "' y='" << py(i) << "' fill='white' font-size='12'>" << i << "</text>";
      out << "<text x='18' y='375' fill='white' font-size='14'>XY projection; independent system</text>";
    } else {
      outpost_panel(out, local);
    }
    out << "</g>";
  }
  out << "</svg>";
  std::cout << "Local plans: " << path << '\n';
}

// One thread, a fixed 8x5x7 block of chunks around the start: comparable step costs between versions.
void bench(runtime& world, uint64_t seed) {
  const auto p = world.start(seed, 0, 0);
  const int64_t cx = int64_t(std::floor(p[0] / chunk_cells)), cy = int64_t(std::floor(p[1] / chunk_cells)), cz = int64_t(std::floor(p[2] / chunk_cells));
  world.reset_timings();
  world.chain().reset_stats();
  size_t vertices = 0, chunks = 0;
  // Fingerprint of every surface and structure vertex, bit for bit: an optimisation that claims the
  // same geometry must keep it.
  uint64_t print = 0;
  const auto began = clock_type::now();
  for (int64_t x = cx - 4; x < cx + 4; ++x)
    for (int64_t y = cy - 2; y <= cy + 2; ++y)
      for (int64_t z = cz - 3; z <= cz + 3; ++z) {
        const auto chunk = world.chunk(seed, {x, y, z});
        for (const char* buffer : {"surface", "structure_surface"})
          for (const char* field : {"position", "normal", "shade"}) {
            const auto values = chunk->field(buffer, field);
            const uint32_t components = field[0] == 's' ? 1 : 3;
            print = devils_engine::utils::mix(print, values.count());
            for (size_t i = 0; i < values.count(); ++i)
              for (uint32_t a = 0; a < components; ++a)
                print = devils_engine::utils::mix(print, std::bit_cast<uint64_t>(values.get(i, a)));
          }
        vertices += chunk->field("surface", "position").count();
        ++chunks;
      }
  const double seconds = std::chrono::duration<double>(clock_type::now() - began).count();
  std::cout << "Bench seed " << seed << ": " << chunks << " chunks, " << vertices << " surface vertices, fingerprint " << std::hex << print << std::dec << ", " << seconds << " s\n";
  for (const auto& [name, s] : world.chain().all_stats())
    if (s.builds && s.self_ns > 50'000'000) std::cout << "  " << name << ": " << s.builds << " builds, " << s.hits << " hits, " << s.evictions << " evictions, self " << double(s.self_ns) / 1e6 / s.builds << " ms, total " << double(s.self_ns) / 1e9 << " s\n";
  world.report_timings();
}
int flight(runtime& world, uint64_t seed, double speed, double seconds, size_t workers, int sx, int sz) {
  const auto p = world.start(seed, sx, sz);
  o::chunk_key current{};
  devils_engine::gn03::chunk_streamer stream(world.factory(seed), workers, [&](const o::chunk_key& at) {
    world.chain().prefetch(world.generator_name(), seed, key(at.x, at.y, at.z));
  });
  auto refresh = [&](double x) {
    current = {int64_t(std::floor(x / chunk_cells)), int64_t(std::floor(p[1] / chunk_cells)), int64_t(std::floor(p[2] / chunk_cells))};
    stream.set_window({current, 4, 4});
    std::vector<o::chunk_key> ahead;
    for (int z = -4; z <= 4; ++z)
      for (int y = -4; y <= 4; ++y)
        for (int dx = 5; dx <= 10; ++dx)
          ahead.push_back({current.x + dx, current.y + y, current.z + z});
    stream.set_prefetch(ahead);
  };
  auto drain = [&] {
    devils_engine::gn03::chunk_mesh mesh;
    while (stream.pop_ready(mesh)) {
    }
    stream.take_evicted();
  };
  auto missing = [&] {
    int n = 0;
    for (int z = -2; z <= 2; ++z)
      for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
          n += !stream.present({current.x + x, current.y + y, current.z + z});
    return n;
  };
  refresh(p[0]);
  const auto cold = clock_type::now();
  while (true) {
    drain();
    if (!missing()) break;
    require(std::chrono::duration<double>(clock_type::now() - cold).count() < 60, "cold start timeout");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::cout << "Cold near cube: " << std::chrono::duration<double>(clock_type::now() - cold).count() << " s\n";
  const auto begin = clock_type::now();
  size_t frames = 0, holes = 0;
  int worst = 0;
  auto next = begin;
  while (true) {
    next += std::chrono::microseconds(16667);
    std::this_thread::sleep_until(next);
    const double t = std::chrono::duration<double>(clock_type::now() - begin).count();
    if (t >= seconds) break;
    refresh(p[0] + speed * t);
    drain();
    const int n = missing();
    ++frames;
    holes += n > 0;
    worst = std::max(worst, n);
  }
  auto stats = stream.stats();
  std::cout << "Flight " << speed << " m/s, " << seconds << " s, " << workers << " workers: " << holes << '/' << frames << " near-hole frames, worst " << worst << " missing, " << stats.generated << " chunks, mean build " << stats.total_milliseconds / std::max<size_t>(1, stats.generated) << " ms\n";
  for (const auto& [name, s] : world.chain().all_stats())
    if (s.builds) std::cout << "  " << name << ": " << s.builds << " builds, self " << double(s.self_ns) / 1e6 / s.builds << " ms, wait " << double(s.wait_ns) / 1e9 << " s\n";
  world.report_timings();
  return holes ? 2 : 0;
}
int main(int argc, char** argv) {
  try {
    uint64_t seed = 0;
    size_t seeds = 1;
    bool check = false, natural = false, progress = false, caves_only = false, cave_check = false, outpost_check = false;
    double speed = 0, seconds = 20;
    size_t workers = 11;
    int sx = 0, sz = 0, cave_layer = 0;
    std::string map, plans, cave_plan, gallery, module = "gn09_next";
    bool outpost_chunk_check = false, benchmark = false;
    CLI::App app{"GN09 NEXT"};
    app.option_defaults()->always_capture_default();
    app.add_flag("--natural", natural);
    app.add_flag("--caves-only", caves_only);
    app.add_flag("--verify-caves", cave_check);
    app.add_flag("--verify-outposts", outpost_check);
    app.add_flag("--outpost-chunks", outpost_chunk_check);
    app.add_flag("--bench", benchmark, "Build a fixed block of chunks in one thread and report step costs");
    app.add_option("--outpost-gallery", gallery, "Plans of all nine outposts, SVG");
    app.add_flag("--progress", progress);
    app.add_flag("--verify", check);
    app.add_option("--seed", seed, "World seed");
    app.add_option("--seeds", seeds, "Verification seeds")->check(CLI::PositiveNumber);
    app.add_option("--flight", speed, "Flight speed")->check(devils_engine::playground::finite_number)->check(CLI::NonNegativeNumber);
    app.add_option("--seconds", seconds, "Flight duration")->check(devils_engine::playground::finite_number)->check(CLI::PositiveNumber);
    app.add_option("--workers", workers, "Workers")->check(CLI::PositiveNumber);
    app.add_option("--map", map, "Map SVG");
    app.add_option("--plans", plans, "Outpost plans SVG");
    app.add_option("--cave-plan", cave_plan, "Cave plan SVG");
    app.add_option("--cave-layer", cave_layer, "Cave layer");
    app.add_option("--module", module, "Generator module");
    std::array<int, 2> sector{sx, sz};
    devils_engine::playground::add_tuple(app, "--sector", sector, ',');
    if (auto code = devils_engine::playground::parse_cli(app, argc, argv)) return *code;
    sx = sector[0]; sz = sector[1];
    require(workers > 0 && seconds > 0 && seeds > 0 && std::isfinite(speed) && speed >= 0, "invalid worker count, seed count or flight parameters");
    runtime world(module, natural, caves_only);
    std::jthread monitor;
    if (progress) monitor = std::jthread([&](std::stop_token stop) {
                    while (!stop.stop_requested()) {
                      for (int i = 0; i < 50 && !stop.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                      if (!stop.stop_requested()) {
                        world.report();
                        std::cout << std::flush;
                      }
                    }
                  });
    if (!map.empty()) map_svg(world, seed, map);
    if (!plans.empty()) plans_svg(world, seed, plans);
    if (!gallery.empty()) outpost_gallery_svg(world, seed, gallery);
    if (outpost_chunk_check)
      for (size_t i = 0; i < seeds; ++i)
        outpost_chunks(world, seed + i);
    if (!cave_plan.empty()) cave_plan_svg(world, seed, sx, sz, cave_layer, cave_plan);
    if (cave_check)
      for (size_t i = 0; i < seeds; ++i)
        verify_cave_systems(world, seed + i);
    verbose_outposts = outpost_check && seeds == 1;
    if (outpost_check)
      for (size_t i = 0; i < seeds; ++i)
        verify_places(world, seed + i);
    if (check)
      for (size_t i = 0; i < seeds; ++i)
        verify(world, seed + i);
    if (benchmark)
      for (size_t i = 0; i < seeds; ++i)
        bench(world, seed + i);
    if (speed > 0) return flight(world, seed, speed, seconds, workers, sx, sz);
    if (!check && !cave_check && !outpost_check && !outpost_chunk_check && !benchmark && map.empty() && plans.empty() && gallery.empty() && cave_plan.empty()) verify(world, seed);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "GN09_next: " << e.what() << '\n';
    return 1;
  }
}
