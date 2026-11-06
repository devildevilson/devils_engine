#include "runtime.h"
#include "viewer.h"
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
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--natural")
        natural = true;
      else if (a == "--caves-only")
        caves_only = true;
      else if (a == "--clear")
        clear_view = true;
      else if (a == "--uncapped")
        options.uncapped = true;
      else if (a.starts_with("--seed="))
        options.seed = std::stoull(a.substr(7));
      else if (a.starts_with("--speed="))
        options.move_speed = std::stod(a.substr(8));
      else if (a.starts_with("--cave-layer="))
        cave_layer = std::stoi(a.substr(13));
      else if (a.starts_with("--frames="))
        options.frames = std::stoul(a.substr(9));
      else if (a.starts_with("--flight="))
        options.flight_speed = std::stod(a.substr(9));
      else if (a.starts_with("--turn-seconds="))
        options.flight_turn_seconds = std::stod(a.substr(15));
      else if (a.starts_with("--yaw="))
        options.start_yaw = std::stof(a.substr(6));
      else if (a == "--diagnostic") {
        diagnostic = true;
        options.mode = 1;
        options.fog_density = 0.003f;
        options.ambient = 3;
        options.lamp = 3;
      } else if (a.starts_with("--pitch="))
        options.start_pitch = std::stof(a.substr(8));
      else if (a.starts_with("--radius=")) {
        const auto radius = std::stoi(a.substr(9));
        if (radius < 2 || radius > 8) throw std::runtime_error("view radius must be between 2 and 8 chunks");
        options.window.horizontal_radius = radius;
        options.window.vertical_radius = radius;
        options.arena_vertices = size_t(2 * radius + 1) * size_t(2 * radius + 1) * size_t(2 * radius + 1) * 24000;
      } else if (a.starts_with("--workers="))
        options.workers = std::stoul(a.substr(10));
      else if (a.starts_with("--guard="))
        options.guard_radius = std::stoi(a.substr(8));
      else if (a.starts_with("--dump="))
        options.dump_path = a.substr(7);
      else if (a.starts_with("--view="))
        view = a.substr(7);
      else if (a.starts_with("--module="))
        module = a.substr(9);
      else if (a.starts_with("--position=")) {
        const auto first = a.find(','), second = a.find(',', first + 1);
        if (first == std::string::npos || second == std::string::npos) throw std::runtime_error("position requires x,y,z");
        position = {std::stod(a.substr(11, first - 11)), std::stod(a.substr(first + 1, second - first - 1)), std::stod(a.substr(second + 1))};
        positioned = true;
      } else if (a.starts_with("--sector=")) {
        auto c = a.find(',');
        sx = std::stoi(a.substr(9, c - 9));
        sz = std::stoi(a.substr(c + 1));
      } else
        throw std::runtime_error("unknown option: " + a);
    }
    if (options.workers == 0 || options.flight_speed < 0 || options.flight_turn_seconds < 0 || options.move_speed < 0 || !std::isfinite(options.move_speed)) throw std::runtime_error("invalid worker count or flight parameters");
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
      const bool cave = view == "cave" || view == "cave-hall" || view == "cave-tight" || view == "cave-shaft", station = view == "station" || view == "station-room" || view == "station-exterior";
      const bool collapse = view == "collapse";
      if (profile == profiles.end() && !cave && !station && !collapse) throw std::runtime_error("unknown debug view: " + view);
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
              if ((view == "cave-hall" || view == "cave-tight" || view == "cave-shaft" || caves_only) && (view != "cave-tight" || candidate->field("cave_meta", "slits").get(0) > 0)) {
                inspected_cave = candidate;
                sx += dx;
                sz += dz;
                found = true;
              } else if (view != "cave-tight") {
                const auto inspection = world->chain().get("cave_inspect", options.seed, key(sx + dx, sz + dz, cave_layer));
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
            if (profile != profiles.end() ? (int(node->field("network_node", "biome").get(0)) != profile - profiles.begin() || node->field("network_node", "blend").get(0) > 0.15) : site != (cave ? 1 : 2)) continue;
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
        if (!eye_found) throw std::runtime_error("no visible collapse viewpoint in this owner; choose another --sector");
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
          auto position = inspected_features->field("cave_portals", "position"), direction = inspected_features->field("cave_portals", "direction");
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
          for (uint32_t a = 0; a < 3; ++a)
            p[a] = local->field("station_rooms", "centre").get(0, a);
          p[0] -= 3;
          p[1] -= 1.3;
          p[2] -= 3;
          delta = {6, 0.5, 3};
        }
        if (view == "station-exterior") {
          const auto c = local->field("station_walls", "centre"), outside = local->field("station_walls", "outside"), axis = local->field("station_walls", "axis"), sign = local->field("station_walls", "sign");
          const auto natural_field = world->chain().field("outpost", options.seed, key(sx, sz), "natural");
          bool chosen = false;
          for (size_t i = 0; i < c.count() && !chosen; ++i)
            if (outside.get(i) != 0) {
              const uint32_t a = uint32_t(axis.get(i));
              for (double distance : {10.0, 6.0, 3.0}) {
                o::field_vector eye{};
                for (uint32_t j = 0; j < 3; ++j)
                  eye[j] = c.get(i, j);
                eye[a] += sign.get(i) * distance;
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
          if (!chosen) throw std::runtime_error("selected outpost has no exposed wall viewpoint; choose another --sector");
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
    options.start = {int64_t(std::floor(p[0] / chunk_cells)), int64_t(std::floor(p[1] / chunk_cells)), int64_t(std::floor(p[2] / chunk_cells))};
    options.start_offset_valid = true;
    options.start_offset = {p[0] - options.start.x * chunk_cells, p[1] - options.start.y * chunk_cells, p[2] - options.start.z * chunk_cells};
    devils_engine::gn03::world_memory memory;
    options.memory = &memory;
    auto seed = std::make_shared<std::atomic<uint64_t>>(options.seed);
    options.prefetch = [world, seed](const o::chunk_key& at) {
      world->chain().prefetch(world->generator_name(), seed->load(), key(at.x, at.y, at.z));
    };
    const int result = devils_engine::gn03::run_viewer(options, [world, seed](const std::vector<std::pair<std::string, double>>&, uint64_t value) {
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
