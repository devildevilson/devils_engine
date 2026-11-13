#include "devils_engine/playground/cli.h"
#include "runtime.h"
#include "devils_engine/playground/chunk_viewer.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
  try {
    using namespace gn09_next;
    devils_engine::gn03::viewer_options options;
    options.window_title = "GN09 next — biome-first generator";
    options.async_present = true;
    options.realtime_flight = true;
    options.overlay_title = "GN09 next";
    options.overlay_summary = "Originator: biomes -> graph -> environment -> caves/outposts -> chunks";
    options.chunk_cells = chunk_cells;
    options.cell_size = 1;
    options.workers = 6;
    options.window.horizontal_radius = 4;
    options.window.vertical_radius = 4;
    options.prefetch_layers = 6;
    options.guard_radius = 2;
    options.underwater = true;
    options.rock_palette = true;
    options.arena_vertices = 729 * 24000;
    options.seed = 0;
    int sx = 0, sz = 0;
    bool natural = false, caves_only = false, diagnostic = false, clear_view = false;
    int cave_layer = 0;
    std::array<double, 3> position{};
    bool positioned = false;
    std::string module = "gn09_next", view;
    CLI::App app{"GN09 viewer: streamed flight through the gn09_next world"};
    app.option_defaults()->always_capture_default();
    app.add_flag("--natural", natural, "Chunks without outposts and expedition caves");
    app.add_flag("--caves-only", caves_only, "Chunks of expedition caves only");
    app.add_flag("--clear", clear_view, "Service light and weak turbidity to inspect the shape");
    app.add_flag("--uncapped", options.uncapped, "Do not cap the frame rate");
    app.add_flag("--diagnostic", diagnostic, "Colour surfaces by normal");
    app.add_option("--seed", options.seed, "World seed");
    app.add_option("--speed", options.move_speed, "Camera speed, m/s")
        ->check(devils_engine::playground::finite_number)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--cave-layer", cave_layer, "Depth layer of the expedition cave");
    app.add_option("--frames", options.frames, "Frame limit");
    app.add_option("--flight", options.flight_speed, "Automatic flight speed, m/s")
        ->check(devils_engine::playground::finite_number)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--turn-seconds", options.flight_turn_seconds, "Seconds between turns of the automatic flight")
        ->check(devils_engine::playground::finite_number)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--yaw", options.start_yaw, "Camera yaw, radians")->check(devils_engine::playground::finite_number);
    app.add_option("--pitch", options.start_pitch, "Camera pitch, radians")
        ->check(devils_engine::playground::finite_number);
    int radius = options.window.horizontal_radius;
    app.add_option("--radius", radius, "View radius (chunks)")->check(CLI::Range(2, 8));
    app.add_option("--workers", options.workers, "Chunk workers")->check(CLI::PositiveNumber);
    app.add_option("--guard", options.guard_radius, "Chunks around the camera that must be ready before it moves on");
    app.add_option("--dump", options.dump_path, "Write the last frame to this PPM file");
    app.add_option("--view", view, "Place the camera at a debug view near --sector")
        ->check(CLI::IsMember({"gallery", "clutter", "hall", "ocean", "cave", "cave-hall", "cave-tight", "cave-shaft",
            "station", "station-room", "station-exterior", "station-dock", "collapse"}));
    app.add_option("--module", module, "Generator module under resources/");
    std::array<int, 2> sector{sx, sz};
    devils_engine::playground::add_tuple(app, "--sector", sector, ',');
    devils_engine::playground::add_tuple(app, "--position", position, ',');
    if (auto code = devils_engine::playground::parse_cli(app, argc, argv)) return *code;
    sx = sector[0];
    sz = sector[1];
    positioned = app.count("--position") != 0;
    if (app.count("--radius")) {
      options.window.horizontal_radius = radius;
      options.window.vertical_radius = radius;
      options.arena_vertices = size_t(2 * radius + 1) * size_t(2 * radius + 1) * size_t(2 * radius + 1) * 24000;
    }
    if (diagnostic) {
      options.mode = 1;
      options.fog_density = 0.003f;
      options.ambient = 3;
      options.lamp = 3;
    }
    if (caves_only && view.empty()) view = "cave-hall";
    if (caves_only || view.starts_with("cave")) {
      if (options.move_speed == 0) options.move_speed = 0.9;
      options.near_plane = 0.025f;
      if (!diagnostic) {
        options.lamp_range = 14;
        options.fog_density = 0.026f;
      }
    }
    if (clear_view) {
      options.ambient = 10;
      options.lamp = 6;
      options.lamp_range = 60;
      options.fog_density = 0.003f;
    }
    auto world = std::make_shared<runtime>(module, natural, caves_only);
    auto p = world->start(options.seed, sx, sz);
    if (!view.empty()) {
      const std::vector<std::string> profiles{"gallery", "clutter", "hall", "ocean"};
      const auto profile = std::find(profiles.begin(), profiles.end(), view);
      const bool cave = view == "cave" || view == "cave-hall" || view == "cave-tight" || view == "cave-shaft",
                 station = view == "station" || view == "station-room" || view == "station-exterior" ||
          view == "station-dock";
      const bool collapse = view == "collapse";
      if (profile == profiles.end() && !cave && !station && !collapse)
        throw std::runtime_error("unknown debug view: " + view);
      o::result_ptr inspected_cave, inspected_features;
      bool found = false;
      for (int radius = 0; radius <= 20 && !found; ++radius) {
        for (int dz = -radius; dz <= radius && !found; ++dz)
          for (int dx = -radius; dx <= radius && !found; ++dx) {
            if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;
            const auto node = world->chain().get("network", options.seed, key(sx + dx, sz + dz));
            if (collapse) {
              const auto candidate = world->chain().get("features", options.seed, key(sx + dx, sz + dz, 0));
              if (candidate->field("heaps", "centre").count()) {
                inspected_features = candidate;
                sx += dx;
                sz += dz;
                found = true;
              }
              continue;
            }
            if (cave) {
              const auto candidate = world->chain().get("small_cave", options.seed, key(sx + dx, sz + dz, cave_layer));
              if ((view == "cave-hall" || view == "cave-tight" || view == "cave-shaft" || caves_only) &&
                  (view != "cave-tight" || candidate->field("cave_meta", "slits").get(0) > 0)) {
                inspected_cave = candidate;
                sx += dx;
                sz += dz;
                found = true;
              } else if (view != "cave-tight") {
                const auto inspection =
                    world->chain().get("cave_inspect", options.seed, key(sx + dx, sz + dz, cave_layer));
                if (inspection->field("cave_portals", "position").count()) {
                  inspected_cave = candidate;
                  inspected_features = inspection;
                  sx += dx;
                  sz += dz;
                  found = true;
                }
              }
              continue;
            }
            const int site = 2;
            if (profile != profiles.end()
                    ? (int(node->field("network_node", "biome").get(0)) != profile - profiles.begin() ||
                          node->field("network_node", "blend").get(0) > 0.15)
                    : site != (cave ? 1 : 2))
              continue;
            sx += dx;
            sz += dz;
            found = true;
          }
      }
      if (!found) throw std::runtime_error("no matching debug view nearby");
      p = world->start(options.seed, sx, sz);
      if (collapse) {
        const auto centres = inspected_features->field("heaps", "centre");
        const auto support = world->chain().field("features", options.seed, key(sx, sz, 0), "support");
        bool eye_found = false;
        for (size_t i = 0; i < centres.count() && !eye_found; i += 7) {
          o::field_vector top{centres.get(i, 0), centres.get(i, 1) + 12, centres.get(i, 2)};
          if (support->value(top).value >= -0.5) continue;
          for (double angle : {0.0, 1.5, 3.0, 4.5}) {
            o::field_vector eye{top[0] + 40 * std::cos(angle), top[1] + 3, top[2] + 40 * std::sin(angle)};
            if (support->value(eye).value >= -0.5) continue;
            p = eye;
            options.start_yaw = float(std::atan2(top[2] - eye[2], top[0] - eye[0]));
            options.start_pitch = float(std::atan2(top[1] - eye[1], 40));
            eye_found = true;
            break;
          }
        }
        if (!eye_found)
          throw std::runtime_error("no visible collapse viewpoint in this owner; choose another --sector");
      }
      if (cave) {
        std::array<double, 3> target{};
        if (view == "cave-hall" || view == "cave-shaft") {
          auto centres = inspected_cave->field("cave_chambers", "centre");
          size_t chosen = 0;
          for (size_t i = 0; i < centres.count(); ++i)
            if (inspected_cave->field("cave_chambers", "kind").get(i) == (view == "cave-shaft" ? 3 : 1)) {
              chosen = i;
              break;
            }
          for (uint32_t a = 0; a < 3; ++a) {
            p[a] = centres.get(chosen, a);
            target[a] = p[a];
          }
          p[0] -= 5;
          target[0] += 5;
          if (view == "cave-shaft") {
            target[0] = p[0] + 1;
            target[1] = p[1] + 15;
          }
        } else if (view == "cave-tight" || caves_only) {
          auto a = inspected_cave->field("passages", "a"), b = inspected_cave->field("passages", "b");
          size_t chosen = 0;
          for (size_t i = 0; i < a.count(); ++i)
            if (inspected_cave->field("passages", "roof_a").get(i) < 0.65) {
              chosen = i;
              break;
            }
          for (uint32_t axis = 0; axis < 3; ++axis) {
            p[axis] = (a.get(chosen, axis) + b.get(chosen, axis)) / 2;
            target[axis] = b.get(chosen, axis);
          }
        } else {
          auto position = inspected_features->field("cave_portals", "position"),
               direction = inspected_features->field("cave_portals", "direction");
          for (uint32_t a = 0; a < 3; ++a) {
            p[a] = position.get(0, a) - direction.get(0, a) * 2;
            target[a] = position.get(0, a) + direction.get(0, a) * 5;
          }
        }
        options.start_yaw = float(std::atan2(target[2] - p[2], target[0] - p[0]));
        options.start_pitch = float(std::atan2(target[1] - p[1], std::hypot(target[0] - p[0], target[2] - p[2])));
      }
      if (station) {
        const auto local = world->chain().get("outpost", options.seed, key(sx, sz));
        auto position = local->field("station_meta", "dock"), target = local->field("station_meta", "entry");
        std::array<double, 3> delta{};
        for (uint32_t a = 0; a < 3; ++a) {
          p[a] = position.get(0, a);
          delta[a] = target.get(0, a) - p[a];
        }
        if (view == "station-room") {
          // The docking bay of a tower, otherwise the first core room; look towards the room it is entered from.
          const auto centre = local->field("station_rooms", "centre"), half = local->field("station_rooms", "half"),
                     label = local->field("station_rooms", "label"), parent = local->field("station_rooms", "parent");
          size_t room = centre.count();
          for (size_t i = 0; i < centre.count() && room == centre.count(); ++i)
            if (label.get(i) == 1) room = i;
          for (size_t i = 0; i < centre.count() && room == centre.count(); ++i)
            if (label.get(i) == 4) room = i;
          if (room == centre.count())
            throw std::runtime_error("selected outpost has neither a docking bay nor a core room");
          // Entered through the floor or roof: look at the farthest room entered from this one instead.
          size_t from = size_t(parent.get(room));
          auto across = [&](size_t i) {
            return std::hypot(centre.get(i, 0) - centre.get(room, 0), centre.get(i, 2) - centre.get(room, 2));
          };
          if (across(from) < 1)
            for (size_t i = 0; i < centre.count(); ++i)
              if (parent.get(i) == double(room) && across(i) > across(from)) from = i;
          std::array<double, 3> towards{
              centre.get(from, 0) - centre.get(room, 0), 0, centre.get(from, 2) - centre.get(room, 2)};
          const double length = std::max(1e-6, std::hypot(towards[0], towards[2]));
          const double back = std::min({half.get(room, 0), half.get(room, 2)}) * 0.7;
          for (uint32_t a = 0; a < 3; ++a)
            p[a] = centre.get(room, a) - towards[a] / length * back;
          p[1] = centre.get(room, 1) - half.get(room, 1) + 1.6;
          for (uint32_t a = 0; a < 3; ++a)
            delta[a] = centre.get(from, a) - p[a];
          delta[1] = 0;
        }
        if (view == "station-exterior") {
          const auto c = local->field("station_walls", "centre"), outside = local->field("station_walls", "outside"),
                     normal = local->field("station_walls", "normal");
          const auto natural_field = world->chain().field("outpost", options.seed, key(sx, sz), "natural");
          bool chosen = false;
          for (size_t i = 0; i < c.count() && !chosen; ++i)
            if (outside.get(i) != 0) {
              for (double distance : {10.0, 6.0, 3.0}) {
                o::field_vector eye{};
                for (uint32_t j = 0; j < 3; ++j)
                  eye[j] = c.get(i, j) + normal.get(i, j) * distance;
                if (natural_field->value(eye).value < -0.5) {
                  for (uint32_t j = 0; j < 3; ++j) {
                    p[j] = eye[j];
                    delta[j] = c.get(i, j) - eye[j];
                  }
                  chosen = true;
                  break;
                }
              }
            }
          if (!chosen)
            throw std::runtime_error("selected outpost has no exposed wall viewpoint; choose another --sector");
        }
        if (view == "station-dock") {
          // Behind the docked boat, looking across the dock at the lock mouth.
          const auto hatch = local->field("station_meta", "hatch");
          std::array<double, 3> forward{};
          double length = 0;
          for (uint32_t a = 0; a < 3; ++a) {
            forward[a] = hatch.get(0, a) - position.get(0, a);
            length += forward[a] * forward[a];
          }
          length = std::sqrt(length);
          for (uint32_t a = 0; a < 3; ++a)
            forward[a] /= length;
          // Prefer the view from behind the boat (side lock) or beside it (vertical lock); keep the eye in open water.
          const double c = local->field("station_meta", "yaw_c").get(0),
                       s = local->field("station_meta", "yaw_s").get(0);
          const bool vertical = std::abs(forward[1]) > 0.5;
          const std::array<double, 3> back{-forward[0], 0, -forward[2]}, beam{s, 0, -c}, along{c, 0, s};
          std::vector<std::array<double, 3>> directions;
          if (vertical)
            directions = {beam, {-beam[0], 0, -beam[2]}, along, {-along[0], 0, -along[2]}};
          else
            directions = {back, beam, {-beam[0], 0, -beam[2]}};
          const auto natural_field = world->chain().field("outpost", options.seed, key(sx, sz), "natural");
          bool chosen = false;
          for (double distance : {18.0, 14.0, 10.0})
            for (const auto& away : directions) {
              if (chosen) break;
              o::field_vector eye{}, middle{};
              for (uint32_t a = 0; a < 3; ++a) {
                eye[a] = position.get(0, a) + away[a] * distance + (a == 1 ? distance * 0.2 : 0);
                middle[a] = (eye[a] + position.get(0, a)) / 2;
              }
              if (natural_field->value(eye).value < -1 && natural_field->value(middle).value < -1) {
                for (uint32_t a = 0; a < 3; ++a) {
                  p[a] = eye[a];
                  delta[a] = hatch.get(0, a) - eye[a];
                }
                chosen = true;
              }
            }
          if (!chosen)
            throw std::runtime_error("selected outpost has no open-water dock viewpoint; choose another --sector");
          std::cout << "Dock view: eye " << p[0] << ' ' << p[1] << ' ' << p[2] << ", dock " << position.get(0, 0) << ' '
                    << position.get(0, 1) << ' ' << position.get(0, 2) << ", hatch " << hatch.get(0, 0) << ' '
                    << hatch.get(0, 1) << ' ' << hatch.get(0, 2);
          const auto beacon = local->field("station_meta", "beacon");
          static const char* beacon_kinds[] = {"roof", "wall", "lock mouth"};
          std::cout << ", beacon " << beacon.get(0, 0) << ' ' << beacon.get(0, 1) << ' ' << beacon.get(0, 2) << " ("
                    << beacon_kinds[std::min<size_t>(2, size_t(local->field("station_meta", "beacon_kind").get(0)))]
                    << ")\n";
        }
        if (view == "station") {
          for (uint32_t a = 0; a < 3; ++a)
            p[a] = target.get(0, a);
          p[0] -= 5;
          delta = {1, 0, 0};
        }
        options.start_yaw = float(std::atan2(delta[2], delta[0]));
        options.start_pitch = float(std::atan2(delta[1], std::hypot(delta[0], delta[2])));
      }
      std::cout << "Debug view " << view << ": sector " << sx << ',' << sz << '\n';
    }
    if (positioned) p = position;
    options.start = {int64_t(std::floor(p[0] / chunk_cells)), int64_t(std::floor(p[1] / chunk_cells)),
        int64_t(std::floor(p[2] / chunk_cells))};
    options.start_offset_valid = true;
    options.start_offset = {p[0] - options.start.x * chunk_cells, p[1] - options.start.y * chunk_cells,
        p[2] - options.start.z * chunk_cells};
    devils_engine::gn03::world_memory memory;
    options.memory = &memory;
    auto seed = std::make_shared<std::atomic<uint64_t>>(options.seed);
    options.prefetch = [world, seed](const o::chunk_key& at) {
      world->chain().prefetch(world->generator_name(), seed->load(), key(at.x, at.y, at.z));
    };
    const int result = devils_engine::gn03::run_viewer(
        options, [world, seed](const std::vector<std::pair<std::string, double>>&, uint64_t value) {
          seed->store(value);
          return world->factory(value);
        });
    world->report();
    return result;
  } catch (const std::exception& e) {
    std::cerr << "GN09_next_viewer: " << e.what() << '\n';
    return 1;
  }
}
