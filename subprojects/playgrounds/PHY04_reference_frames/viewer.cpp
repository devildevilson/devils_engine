#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <format>
#include <iostream>

#include <glm/ext/matrix_transform.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/playground/cli.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/utils/error.h"

#include "frames.h"
#include "primitive_view.h"
#include "replay.h"
#include "server_zones.h"

using namespace devils_engine;

namespace devils_engine::phy04 {
namespace {

using namespace phy03::primitive_view;

void segment(std::vector<vertex>& vertices, const Vec3 from, const Vec3 to, const uint32_t color) {
  const auto tint = glm::vec4(tint_from_rgb(color), -.92f);
  vertices.push_back({glm::vec4(glm_vec(from), 1), tint});
  vertices.push_back({glm::vec4(glm_vec(to), 1), tint});
}

void axes(std::vector<vertex>& vertices, const transform& mark, const float size) {
  const auto p = Vec3(mark.position);
  segment(vertices, p, p + size * mark.rotation.RotateAxisX(), 0xff705d);
  segment(vertices, p, p + size * mark.rotation.RotateAxisY(), 0x69f078);
  segment(vertices, p, p + size * mark.rotation.RotateAxisZ(), 0x66b5ff);
}

struct scene_mesh {
  std::array<std::vector<triangle>, fixture_count> fixtures;
  std::array<std::vector<triangle>, part_count> parts;
  std::vector<triangle> chunk_column = make_box(Vec3(.13f, 1.25f, .13f));
  std::vector<vertex> vertices;

  explicit scene_mesh(const snapshot& initial) {
    for (size_t i = 0; i < fixtures.size(); ++i) fixtures[i] = make_box(initial.fixtures[i].shape.size);
    for (size_t i = 0; i < parts.size(); ++i) {
      const auto& shape = phy03::ragdoll_geometry()[i];
      parts[i] = shape.kind == phy03::primitive::box ? make_box(shape.size) : make_round(shape);
    }
    vertices.reserve(6200);
  }

  uint32_t update(const snapshot& old, const snapshot& now, const float alpha) {
    vertices.clear();
    for (size_t i = 0; i < fixtures.size(); ++i)
      append(vertices, fixtures[i], phy03::interpolate(old.fixtures[i].pose, now.fixtures[i].pose, alpha),
        now.fixtures[i].shape.color, i < 2);
    for (size_t i = 0; i < parts.size(); ++i)
      append(vertices, parts[i], phy03::interpolate(old.parts[i], now.parts[i], alpha), phy03::ragdoll_geometry()[i].color);
    const auto column = point_in_zone({now.pelvis_address.key, Vec3(chunk_size * .5f, 1.25f, chunk_size * .5f)}, first_chunk);
    append(vertices, chunk_column, {column}, 0xf377dd);
    const auto solid_count = uint32_t(vertices.size());

    constexpr auto grid_origin = first_chunk;
    for (const auto& key : {first_chunk, second_chunk}) {
      const auto origin = point_in_zone({key, chunk_marker}, grid_origin);
      axes(vertices, {origin}, 1.8f);
    }
    segment(vertices, Vec3(chunk_size, .035f, 0), Vec3(chunk_size, .035f, chunk_size), 0xffda72);
    const auto zone_origin = point_in_zone({now.origin, chunk_marker}, grid_origin);
    segment(vertices, Vec3(zone_origin), Vec3(zone_origin) + Vec3(0, 2.25f, 0), 0xffda72);

    const auto hull = phy03::interpolate(old.hull.pose, now.hull.pose, alpha);
    const auto deck_mark = hull.position + hull.rotation * cabin_marker;
    axes(vertices, {deck_mark, hull.rotation}, .65f);
    axes(vertices, phy03::interpolate(old.parts[0], now.parts[0], alpha), .42f);
    return solid_count;
  }
};

enum class action : size_t { pause, tick, camera, report, reset, exit, count };
constexpr std::array<std::pair<std::string_view, std::string_view>, size_t(action::count)> bindings{{
  {"pause", "space"}, {"tick", "key_n"}, {"camera", "key_v"}, {"report", "key_p"}, {"reset", "key_r"}, {"exit", "escape"}
}};

void key_callback(GLFWwindow*, const int, const int scancode, const int state, const int) noexcept {
  input::events::update_key(scancode, state);
}

void input_error(const int code, const char* message) noexcept {
  std::cerr << "PHY04 input " << code << ": " << message << '\n';
}

struct keyboard {
  std::array<bool, bindings.size()> latched{};

  explicit keyboard(GLFWwindow* window) {
    input::events::init();
    input::events::clear_bindings();
    for (const auto& [name, canonical] : bindings) {
      const auto [key, scancode] = input::key_from_canonical(canonical);
      if (key < 0 || scancode < 0) utils::error{}("PHY04 input: could not bind action '{}' to '{}'", name, canonical);
      input::events::set_key(name, scancode, key);
    }
    input::set_window_callback(window, &key_callback);
  }

  bool pressed(const action button) {
    const auto index = size_t(button);
    const bool down = input::events::is_pressed(bindings[index].first);
    const bool edge = down && !latched[index];
    latched[index] = down;
    return edge;
  }
};

struct viewer_options {
  uint32_t frames = 0;
  uint32_t fps = 60;
  bool turning = false;
  bool zones = false;
  bool paused = false;
  bool validation = false;
};

glm::mat4 view_at(const snapshot& old, const snapshot& now, const float alpha, const bool follow) {
  const glm::vec3 up(0, 1, 0);
  if (!follow) return glm::lookAtRH(glm::vec3(17, 15, 30), glm::vec3(16, 0, 7), up);
  const auto hull = phy03::interpolate(old.hull.pose, now.hull.pose, alpha);
  const auto position = glm_vec(Vec3(hull.position));
  const auto camera = position + glm_vec(hull.rotation * Vec3(-4.5f, 5.1f, 8.0f));
  return glm::lookAtRH(camera, position + glm::vec3(0, .2f, 0), up);
}

struct cabin_scene {
  experiment sim;
  scene_mesh mesh;

  explicit cabin_scene(const viewer_options& options) : sim(options.turning), mesh(sim.current()) {}

  void step() {
    sim.step();
    const auto& current = sim.current();
    if (current.tick >= step_budget && current.stage != phase::finished)
      utils::error{}("PHY04 viewer tick {}: replay ended during phase '{}'", current.tick, phase_name(current.stage));
  }

  bool finished() const { return sim.current().stage == phase::finished; }

  void report() const {
    print_sample(sim.current());
    print_points(sim.current());
  }

  std::string title(const bool paused, const bool follow) const {
    const auto& state = sim.current();
    return std::format("PHY04 | {} #{}{} | origin {} | {} view | chunk X {} + {:.2f} | zone X {:.2f} | deck X {:.2f}",
      phase_name(state.stage), state.tick, paused ? " [paused]" : "", state.origin[0], follow ? "deck" : "world",
      state.pelvis_address.key[0], state.pelvis_address.offset.GetX(), state.pelvis_zone.pose.position.GetX(),
      state.pelvis_cabin.pose.position.GetX());
  }

  bool draw(renderer& render, const uint32_t width, const uint32_t height, const float alpha, const bool follow) {
    const auto& before = sim.previous();
    const auto& after = sim.current();
    const auto count = mesh.update(before, after, alpha);
    return render.draw(view_at(before, after, alpha, follow), width, height, mesh.vertices, count);
  }
};

struct zone_scene {
  server_experiment sim;
  std::vector<triangle> floor = make_box(Vec3(8, .20f, 8));
  std::vector<triangle> column = make_box(Vec3(.18f, 1.25f, .18f));
  std::vector<triangle> player = make_round({phy03::primitive::capsule, {player_radius, player_half_cylinder, player_radius}, 0});
  std::vector<triangle> cargo = make_round({phy03::primitive::sphere, {cargo_radius, 0, cargo_radius}, 0});
  std::vector<vertex> vertices;

  explicit zone_scene(const viewer_options&) { vertices.reserve(3500); }

  void step() { sim.step(); }
  bool finished() const { return sim.current().tick >= zone_step_budget; }
  void report() const { print_zones(sim.current()); }

  std::string title(const bool paused, const bool follow) const {
    const auto& state = sim.current();
    return std::format("PHY04 zones | {} #{}{} | {} view | players in {} and {} | merge {} | cargo hit {} | player hit {}",
      state.merged_tick != 0 ? "common" : "separate", state.tick, paused ? " [paused]" : "",
      follow ? "follow" : "world", state.actors[0].address.key[0], state.actors[2].address.key[0],
      state.merged_tick, state.cargo_contact_tick, state.player_contact_tick);
  }

  bool draw(renderer& render, const uint32_t width, const uint32_t height, const float alpha, const bool follow) {
    const auto& now = sim.current();
    const auto& old = sim.previous();
    vertices.clear();
    for (const auto& cell : now.floors) {
      const auto offset = float(cell.chunk_x - first_chunk[0]) * chunk_size;
      constexpr std::array<uint32_t, 5> floor_colors{0, 0x46617b, 0x765545, 0x686574, 0x4f7569};
      append(vertices, floor, {RVec3(offset + 8, -.20f, 8)}, floor_colors[cell.owners], true);
    }
    std::array<transform, zone_actor_count> poses;
    constexpr std::array<uint32_t, zone_actor_count> colors{0x73d9fa, 0x427ab5, 0xffb276, 0xcf6545};
    for (size_t i = 0; i < poses.size(); ++i) {
      const auto& actor = now.actors[i];
      poses[i] = phy03::interpolate(old.actors[i].motion.pose, actor.motion.pose, alpha);
      append(vertices, actor.player ? player : cargo, poses[i], colors[i]);
      if (actor.player && (i == 0 || actor.address.key != now.actors[0].address.key)) {
        const auto center = point_in_zone({actor.address.key, Vec3(8, 1.25f, 8)}, first_chunk);
        const bool shared = now.actors[0].address.key == now.actors[2].address.key;
        append(vertices, column, {center}, shared ? 0xc3f587 : colors[i]);
      }
    }
    const auto solid_count = uint32_t(vertices.size());
    for (const auto& cell : now.floors) {
      const auto x = float(cell.chunk_x - first_chunk[0]) * chunk_size;
      segment(vertices, Vec3(x, .025f, 0), Vec3(x, .025f, chunk_size), 0xaeb2aa);
    }
    for (size_t i = 0; i < now.active_zones.size(); ++i) {
      if (!now.active_zones[i]) continue;
      const auto origin = point_in_zone({now.origins[i], chunk_marker}, first_chunk);
      axes(vertices, {origin}, 2.2f);
    }
    constexpr float middle = float(right_start_chunk_x - first_chunk[0]) * chunk_size * .5f + 8;
    glm::vec3 target(middle, 0, 8);
    glm::vec3 offset(0, 65, 88);
    if (follow) {
      const auto a = Vec3(poses[0].position);
      const auto b = Vec3(poses[2].position);
      target = glm_vec((a + b) * .5f);
      const float distance = std::abs(a.GetX() - b.GetX());
      offset = glm::vec3(0, 7 + distance * .32f, 11 + distance * .39f);
    }
    const auto view = glm::lookAtRH(target + offset, target, glm::vec3(0, 1, 0));
    return render.draw(view, width, height, vertices, solid_count);
  }
};

template <typename Scene>
void run_viewer(const viewer_options& options) {
  input::init input_runtime(&input_error);
  const std::unique_ptr<GLFWwindow, decltype(&input::destroy)> window(
    input::create_window(1200, 760, "PHY04"), &input::destroy);
  if (!window) utils::error{}("PHY04 viewer: could not create a window");
  keyboard keys(window.get());
  renderer render(window.get(), "PHY04 Reference Frames", options.validation);
  auto scene = std::make_unique<Scene>(options);
  phy03::fixed_clock clock;
  playground::frame_pacer pacer(options.fps);
  auto last = std::chrono::steady_clock::now();
  bool paused = options.paused;
  bool follow = false;
  uint32_t frames = 0;
  while (!input::should_close(window.get())) {
    input::poll_events();
    const auto now = std::chrono::steady_clock::now();
    const auto dt = std::clamp(std::chrono::duration<double>(now - last).count(), 0.0, .1);
    last = now;
    input::events::update(size_t(dt * 1'000'000));
    if (keys.pressed(action::exit)) break;
    if (keys.pressed(action::pause)) paused = !paused;
    if (keys.pressed(action::camera)) follow = !follow;
    if (keys.pressed(action::reset)) {
      scene = std::make_unique<Scene>(options);
      clock = {};
    }
    if (keys.pressed(action::tick)) {
      paused = true;
      scene->step();
    }
    if (!paused)
      clock.advance(options.frames == 0 ? dt : 1.0 / options.fps, [&] { scene->step(); });
    if (keys.pressed(action::report)) scene->report();
    const auto title = scene->title(paused, follow);
    if (title != input::window_title(window.get())) input::set_window_title(window.get(), title);
    const auto [width, height] = input::framebuffer_size(window.get());
    const auto alpha = paused || scene->finished() ? 1.0f : clock.alpha();
    if (scene->draw(render, width, height, alpha, follow)) ++frames;
    if (options.frames != 0 && frames >= options.frames) break;
    pacer.wait();
  }
  scene->report();
}

}
}

int main(const int argc, const char* const* argv) {
  CLI::App app{"PHY04 - watch a ragdoll cross chunks and a moving cabin"};
  phy04::viewer_options options;
  app.add_option("--frames", options.frames, "Maximum rendered frames (0 runs interactively)");
  app.add_option("--fps", options.fps, "Target frames per second")->check(CLI::Range(15, 240))->capture_default_str();
  auto* turn = app.add_flag("--turn", options.turning, "Rotate the cabin during exit");
  app.add_flag("--zones", options.zones, "Watch the authoritative server zones merge")->excludes(turn);
  app.add_flag("--paused", options.paused, "Start paused at tick zero");
  app.add_flag("--validation", options.validation, "Enable Vulkan validation");
  if (const auto result = playground::parse_cli(app, argc, argv)) return *result;

  try {
    const phy03::jolt_runtime runtime;
    if (options.zones) phy04::run_viewer<phy04::zone_scene>(options);
    else phy04::run_viewer<phy04::cabin_scene>(options);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
