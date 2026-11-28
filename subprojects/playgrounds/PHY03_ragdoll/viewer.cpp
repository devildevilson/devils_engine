#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/cli.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/utils/error.h"

#include "replay.h"

using namespace devils_engine;

namespace devils_engine::phy03 {
namespace {

using JPH::Vec3;
constexpr float pi = std::numbers::pi_v<float>;
using triangle = std::array<Vec3, 3>;
using line = std::array<Vec3, 2>;
constexpr int round_sides = 12;
constexpr int round_bands = 4;
using round_rings = std::array<std::array<Vec3, round_sides>, 2 * (round_bands + 1)>;

void quad(std::vector<triangle>& mesh, const Vec3 a, const Vec3 b, const Vec3 c, const Vec3 d) {
  mesh.push_back({a, b, c});
  mesh.push_back({a, c, d});
}

std::array<Vec3, 8> box_points(const Vec3 size) {
  return {
    Vec3(-1, -1, -1) * size, Vec3(1, -1, -1) * size, Vec3(1, -1, 1) * size, Vec3(-1, -1, 1) * size,
    Vec3(-1, 1, -1) * size, Vec3(1, 1, -1) * size, Vec3(1, 1, 1) * size, Vec3(-1, 1, 1) * size};
}

std::vector<triangle> make_box(const Vec3 size) {
  const auto p = box_points(size);
  std::vector<triangle> result;
  result.reserve(12);
  quad(result, p[0], p[1], p[2], p[3]);
  quad(result, p[7], p[6], p[5], p[4]);
  quad(result, p[0], p[4], p[5], p[1]);
  quad(result, p[1], p[5], p[6], p[2]);
  quad(result, p[2], p[6], p[7], p[3]);
  quad(result, p[3], p[7], p[4], p[0]);
  return result;
}

round_rings round_points(const geometry& shape) {
  const auto radius = shape.size.GetX();
  const auto half_height = shape.size.GetY();
  round_rings rings;
  for (int row = 0; row < int(rings.size()); ++row) {
    const bool upper = row > round_bands;
    const float elevation = (upper ? row - (round_bands + 1) : row - round_bands) * (pi / (2 * round_bands));
    const float y = std::sin(elevation) * radius + (upper ? half_height : -half_height);
    const float ring_radius = std::cos(elevation) * radius;
    for (int i = 0; i < round_sides; ++i) {
      const float azimuth = 2 * pi * i / round_sides;
      rings[size_t(row)][size_t(i)] = Vec3(std::cos(azimuth) * ring_radius, y, std::sin(azimuth) * ring_radius);
    }
  }
  return rings;
}

std::vector<triangle> make_round(const geometry& shape) {
  const auto rings = round_points(shape);
  std::vector<triangle> result;
  result.reserve((rings.size() - 1) * round_sides * 2);
  for (size_t row = 1; row < rings.size(); ++row) {
    if (row == round_bands + 1 && shape.kind == primitive::sphere) continue;
    for (size_t i = 0; i < round_sides; ++i) {
      const auto next = (i + 1) % round_sides;
      const auto a = rings[row - 1][i], b = rings[row][i], c = rings[row][next], d = rings[row - 1][next];
      if (row + 1 != rings.size()) result.push_back({a, b, c});
      if (row != 1) result.push_back({a, c, d});
    }
  }
  return result;
}

std::vector<line> make_outline(const geometry& shape) {
  std::vector<line> result;
  if (shape.kind == primitive::box) {
    const auto p = box_points(shape.size);
    result.reserve(12);
    for (size_t i = 0; i < 4; ++i) {
      const auto next = (i + 1) % 4;
      result.push_back({p[i], p[next]});
      result.push_back({p[4 + i], p[4 + next]});
      result.push_back({p[i], p[4 + i]});
    }
    return result;
  }

  const auto rings = round_points(shape);
  result.reserve(4 * round_sides + 4 * (rings.size() - 1));
  for (const auto row : {round_bands / 2, round_bands, round_bands + 1, round_bands + 1 + round_bands / 2}) {
    if (row == round_bands + 1 && shape.kind == primitive::sphere) continue;
    for (size_t i = 0; i < round_sides; ++i)
      result.push_back({rings[row][i], rings[row][(i + 1) % round_sides]});
  }
  for (size_t row = 1; row < rings.size(); ++row) {
    if (row == round_bands + 1 && shape.kind == primitive::sphere) continue;
    for (size_t i = 0; i < round_sides; i += round_sides / 4)
      result.push_back({rings[row - 1][i], rings[row][i]});
  }
  return result;
}

struct vertex {
  glm::vec4 position;
  glm::vec4 color;
};
static_assert(sizeof(vertex) == 32);
static_assert(sizeof(glm::mat4) == 64);

glm::vec3 glm_vec(const Vec3 value) {
  return {value.GetX(), value.GetY(), value.GetZ()};
}

glm::vec3 tint_from_rgb(const uint32_t rgb) {
  return glm::vec3((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255) / 255.0f;
}

void append(std::vector<vertex>& target, const std::vector<triangle>& mesh, const transform& pose,
            const uint32_t rgb, const bool grid = false) {
  const auto tint = tint_from_rgb(rgb);
  const auto light = Vec3(-.43f, .82f, .36f).Normalized();
  const auto center = glm_vec(Vec3(pose.position));
  for (const auto& face : mesh) {
    const auto normal = pose.rotation * (face[1] - face[0]).Cross(face[2] - face[0]).NormalizedOr(Vec3::sAxisY());
    const float lit = .46f + .54f * std::max(0.0f, normal.Dot(light));
    const auto color = glm::vec4(tint * lit, grid && normal.GetY() > .9f ? 1 : 0);
    for (const auto point : face)
      target.push_back({glm::vec4(center + glm_vec(pose.rotation * point), 1), color});
  }
}

void append_outline(std::vector<vertex>& target, const std::vector<line>& mesh, const transform& pose,
                    const uint32_t rgb, const float opacity) {
  // The shared shader uses nonnegative alpha for the solid floor grid marker.
  const auto color = glm::vec4(tint_from_rgb(rgb), -opacity);
  const auto center = glm_vec(Vec3(pose.position));
  for (const auto& edge : mesh)
    for (const auto point : edge)
      target.push_back({glm::vec4(center + glm_vec(pose.rotation * point), 1), color});
}

struct room_mesh {
  std::array<std::vector<triangle>, fixture_count> fixtures;
  std::array<std::vector<triangle>, part_count> actor;
  std::array<std::vector<line>, fixture_count> fixture_outlines;
  std::array<std::vector<line>, part_count> ragdoll_outlines;
  std::vector<line> query_outline = make_outline(query_capsule_geometry());
  std::vector<line> inner_outline = make_outline(inner_capsule_geometry());
  std::vector<vertex> vertices;

  explicit room_mesh(const snapshot& initial) {
    size_t count = 0;
    size_t ragdoll_line_count = 0;
    for (size_t i = 0; i < fixtures.size(); ++i) {
      fixtures[i] = make_box(initial.room[i].shape.size);
      fixture_outlines[i] = make_outline(initial.room[i].shape);
      count += fixtures[i].size() * 3 + fixture_outlines[i].size() * 2;
    }
    for (size_t i = 0; i < actor.size(); ++i) {
      const auto& shape = ragdoll_geometry()[i];
      actor[i] = shape.kind == primitive::box ? make_box(shape.size) : make_round(shape);
      ragdoll_outlines[i] = make_outline(shape);
      count += actor[i].size() * 3 * 2;
      ragdoll_line_count += ragdoll_outlines[i].size();
    }
    const auto actor_line_count = std::max(ragdoll_line_count, query_outline.size() + inner_outline.size());
    vertices.reserve(count + actor_line_count * 2 * 2);
  }

  uint32_t update(const snapshot& previous, const snapshot& current, const float alpha, const bool overview,
                  const bool show_colliders) {
    vertices.clear();
    for (size_t i = 0; i < fixtures.size(); ++i) {
      const auto& item = current.room[i];
      append(vertices, fixtures[i], interpolate(previous.room[i].pose, item.pose, alpha),
             item.shape.color, i == size_t(fixture::floor));
    }
    const auto draw_actor = [&](const actor_snapshot& before, const actor_snapshot& after, const bool player) {
      for (size_t i = 0; i < actor.size(); ++i) {
        const bool head = i == size_t(part::head);
        // Only the player looks from their own head in first person.
        if (head && player && !overview) continue;
        const auto color = player || head ? ragdoll_geometry()[i].color : 0xb6614d;
        append(vertices, actor[i], interpolate(before.parts[i], after.parts[i], alpha), color);
      }
    };
    const auto& previous_player = previous.player.rescues == current.player.rescues ? previous.player : current.player;
    draw_actor(previous_player, current.player, true);
    draw_actor(previous.dummy, current.dummy, false);
    const auto solid_count = uint32_t(vertices.size());
    if (show_colliders) {
      for (size_t i = 0; i < fixture_outlines.size(); ++i)
        append_outline(vertices, fixture_outlines[i], interpolate(previous.room[i].pose, current.room[i].pose, alpha),
                       0xc1d4e3, .52f);
      draw_actor_colliders(previous_player, current.player, alpha);
      draw_actor_colliders(previous.dummy, current.dummy, alpha);
    }
    return solid_count;
  }

  void draw_actor_colliders(const actor_snapshot& before, const actor_snapshot& after, const float alpha) {
    if (after.ragdoll_bodies != 0) {
      for (size_t i = 0; i < ragdoll_outlines.size(); ++i)
        append_outline(vertices, ragdoll_outlines[i], interpolate(before.parts[i], after.parts[i], alpha),
                       0xff71cf, .95f);
    } else if (after.capsule_bodies != 0) {
      const auto& start = before.capsule_bodies != 0 ? before : after;
      append_outline(vertices, query_outline, interpolate(start.query_capsule, after.query_capsule, alpha),
                     query_capsule_geometry().color, .95f);
      append_outline(vertices, inner_outline, interpolate(start.inner_capsule, after.inner_capsule, alpha),
                     inner_capsule_geometry().color, .86f);
    }
  }
};

void write_buffer(painter::graphics_base& render, const std::string_view name, const void* data, const size_t bytes) {
  const auto slot = render.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("PHY03 viewer: missing buffer '{}'", name);
  const auto frame = render.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size)
    utils::error{}("PHY03 viewer buffer '{}': requested {} bytes, capacity {}, mapped {}",
                   name, bytes, frame.sub.size, frame.mapped != nullptr);
  std::memcpy(static_cast<char*>(frame.mapped) + frame.sub.offset, data, bytes);
}

void write_draw(painter::graphics_base& render, const std::string_view name, const uint32_t first, const uint32_t count) {
  const auto command = render.find_constant(name);
  if (command == painter::invalid_resource_slot) utils::error{}("PHY03 viewer: missing draw constant '{}'", name);
  render.write_constant_data(command, VkDrawIndirectCommand{count, 1, first, 0});
}

struct vulkan_handles {
  painter::created_instance instance;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  painter::created_device device;

  ~vulkan_handles() {
    if (device.device) vk::Device(device.device).destroy();
    if (surface) vk::Instance(instance.instance).destroy(surface);
    painter::destroy_debug_messenger(instance.instance, instance.messenger);
    if (instance.instance) vk::Instance(instance.instance).destroy();
  }
};

struct wait_for_device {
  VkDevice device;
  ~wait_for_device() { VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(device); }
};

enum class control : size_t {
  forward, back, left, right, weak_brake, strong_brake, test_push, small_push, less_push, more_push,
  camera, overview, colliders, rescue, reset, exit, count
};

struct control_binding {
  std::string_view action;
  std::string_view canonical_key;
};

constexpr std::array<control_binding, size_t(control::count)> controls{{
  {"move_forward", "key_w"}, {"move_back", "key_s"}, {"move_left", "key_a"}, {"move_right", "key_d"},
  {"weak_brake", "key_1"}, {"strong_brake", "key_2"}, {"test_push", "key_g"}, {"small_push", "key_h"},
  {"less_push", "left_bracket"}, {"more_push", "right_bracket"}, {"camera_mode", "tab"},
  {"overview", "key_v"}, {"colliders", "key_c"}, {"rescue", "key_e"}, {"reset", "key_r"}, {"exit", "escape"}
}};

void key_callback(GLFWwindow*, const int, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
}

class keyboard {
public:
  explicit keyboard(GLFWwindow* window);
  intent read(GLFWwindow* window, uint32_t power);
  bool pressed(control button);

private:
  std::array<bool, controls.size()> latched{};
  double mouse_x = 0;
  double mouse_y = 0;
  bool cursor_ready = false;
};

keyboard::keyboard(GLFWwindow* window) {
  input::events::init();
  input::events::clear_bindings();
  for (const auto& [action, canonical_key] : controls) {
    const auto [key, scancode] = input::key_from_canonical(canonical_key);
    if (key < 0 || scancode < 0)
      utils::error{}("PHY03 input: could not bind action '{}' to key '{}'", action, canonical_key);
    input::events::set_key(action, scancode, key);
  }
  input::set_window_callback(window, &key_callback);
}

bool keyboard::pressed(const control button) {
  const auto index = size_t(button);
  const bool down = input::events::is_pressed(controls[index].action);
  const bool edge = down && !latched[index];
  latched[index] = down;
  return edge;
}

intent keyboard::read(GLFWwindow* window, const uint32_t power) {
  const auto axis = [](const control positive, const control negative) {
    return float(input::events::is_pressed(controls[size_t(positive)].action)) -
           float(input::events::is_pressed(controls[size_t(negative)].action));
  };
  const auto [x, y] = input::cursor_pos(window);
  const bool focused = input::window_focused(window);
  constexpr float sensitivity = .0022f;
  intent result{.forward = axis(control::forward, control::back), .right = axis(control::right, control::left)};
  if (cursor_ready && focused) {
    result.yaw_delta = -float(x - mouse_x) * sensitivity;
    result.pitch_delta = -float(y - mouse_y) * sensitivity;
  }
  mouse_x = x;
  mouse_y = y;
  cursor_ready = focused;
  const bool weak = pressed(control::weak_brake);
  const bool strong = pressed(control::strong_brake);
  if (weak || strong) result.brake = strong ? brake_test::strong : brake_test::weak;
  const bool test_push_pressed = pressed(control::test_push);
  const bool small_push_pressed = pressed(control::small_push);
  result.room_push = test_push_pressed ? power : small_push_pressed ? weak_push : 0;
  result.rescue = pressed(control::rescue);
  return result;
}

void input_error(const int code, const char* message) noexcept {
  std::cerr << "PHY03 input " << code << ": " << message << '\n';
}

struct viewer_options {
  uint32_t frames = 0;
  uint32_t fps = 60;
  float push_velocity = push_speed(strong_push);
  bool validation = false;
  bool show_colliders = false;
  std::string replay;
  camera_mode camera = camera_mode::level;
};

bool draw(const simulation& sim, const float alpha, const camera_mode mode, const bool show_colliders,
          const uint32_t width, const uint32_t height, room_mesh& mesh,
          painter::graphics_base& render, painter::graphics_ctx& context) {
  render.prepare_frame();
  if (!render.can_draw()) return false;

  const auto camera = sim.sample_camera(alpha, mode);
  const auto view = glm::lookAtRH(glm_vec(Vec3(camera.position)),
    glm_vec(Vec3(camera.position) + camera.rotation.RotateAxisX()), glm_vec(camera.rotation.RotateAxisY()));
  const auto projection = playground::infinite_reverse_z_projection(1.25f, float(width) / float(height), .045f);
  const auto vp = projection * view;
  const auto solid_count = mesh.update(sim.previous(), sim.current(), alpha, mode == camera_mode::overview, show_colliders);
  const auto line_count = uint32_t(mesh.vertices.size()) - solid_count;

  write_buffer(render, "camera_buffer", &vp, sizeof(vp));
  write_buffer(render, "scene_vertices", mesh.vertices.data(), mesh.vertices.size() * sizeof(vertex));
  write_draw(render, "room_draw", 0, solid_count);
  write_draw(render, "collider_draw", solid_count, line_count);
  render.update_event();
  context.prepare();
  context.draw();
  render.submit_frame();
  return true;
}

std::string_view camera_name(const camera_mode mode) {
  if (mode == camera_mode::overview) return "overview";
  return mode == camera_mode::head ? "head orientation" : "level horizon";
}

void run_viewer(const viewer_options& options) {
  input::init input_runtime(&input_error);
  const std::unique_ptr<GLFWwindow, decltype(&input::destroy)> window(
    input::create_window(1120, 720, "PHY03"), &input::destroy);
  if (!window) utils::error{}("PHY03 viewer: could not create a window");
  const bool replaying = !options.replay.empty();
  if (!replaying) {
    input::set_cursor_input_mode(window.get(), DEVILS_ENGINE_INPUT_CURSOR_DISABLED);
    input::set_raw_mouse_motion(window.get());
  }

  vulkan_handles gpu;
  gpu.instance = painter::create_instance({.app_name = "PHY03 Ragdoll", .validation = options.validation});
  const auto surface_result = input::create_window_surface(gpu.instance.instance, window.get(), nullptr, &gpu.surface);
  if (surface_result != VK_SUCCESS) utils::error{}("PHY03 Vulkan surface creation failed with VkResult {}", surface_result);
  painter::system_info system(gpu.instance.instance);
  system.check_devices_surface_capability(gpu.surface);
  const auto physical = system.choose_physical_device();
  gpu.device = painter::create_device(gpu.instance.instance, physical, true, "phy03");

  auto [width, height] = input::framebuffer_size(window.get());
  painter::graphics_base render(gpu.instance.instance, gpu.device.device, physical.handle, painter::presentation_engine_type::main);
  const wait_for_device wait{gpu.device.device};
  render.create_allocator();
  render.create_command_pool(gpu.device.queues.graphics);
  render.create_descriptor_pool();
  const std::string resource_root = PHY03_RESOURCE_ROOT;
  render.set_shader_source_filesystem(resource_root + "/shaders/");
  render.set_startup_graph("phy03_room");
  auto config = painter::build_render_config(resource_root + "/render_config/");
  if (render.commit_parsed_resources(config) != 0) utils::error{}("PHY03 could not commit render graph '{}'", resource_root);
  render.set_present_mode(painter::physical_device_present_mode::values::mailbox,
                          painter::physical_device_present_mode::values::immediate);
  render.set_surface(gpu.surface, width, height);
  render.resize_viewport(width, height);
  render.populate_constant_default_values();
  const auto graph = render.find_render_graph("phy03_room");
  if (graph == painter::invalid_resource_slot) utils::error{}("PHY03 viewer: missing graph 'phy03_room'");
  render.change_render_graph(graph);
  painter::graphics_ctx context;
  context.base = &render;

  auto sim = std::make_unique<simulation>();
  room_mesh mesh(sim->current());
  fixed_clock clock;
  keyboard keys(window.get());
  intent pending;
  auto camera = options.camera;
  bool show_colliders = options.show_colliders;
  auto first_person = camera == camera_mode::head ? camera_mode::head : camera_mode::level;
  const auto scenario = replaying ? replay_from_name(options.replay) : replay_case::strong;
  auto power = uint32_t(std::lround(options.push_velocity * 10.0f));
  uint64_t trace = 0;
  uint32_t frames = 0;
  playground::frame_pacer pacer(options.fps);
  auto last_time = std::chrono::steady_clock::now();
  while (!input::should_close(window.get())) {
    input::poll_events();
    const auto now = std::chrono::steady_clock::now();
    const auto dt = std::clamp(std::chrono::duration<double>(now - last_time).count(), 0.0, .10);
    last_time = now;
    input::events::update(size_t(dt * 1'000'000));

    if (keys.pressed(control::exit)) break;
    if (keys.pressed(control::camera)) {
      first_person = first_person == camera_mode::head ? camera_mode::level : camera_mode::head;
      if (camera != camera_mode::overview) camera = first_person;
    }
    if (keys.pressed(control::overview))
      camera = camera == camera_mode::overview ? first_person : camera_mode::overview;
    if (keys.pressed(control::colliders)) show_colliders = !show_colliders;
    if (!replaying) {
      if (keys.pressed(control::reset)) {
        sim = std::make_unique<simulation>();
        clock = {};
        pending = {};
      }
      const int change = int(keys.pressed(control::more_push)) - int(keys.pressed(control::less_push));
      power = uint32_t(std::clamp(int(power) + change, 0, int(max_push)));
      const auto next = keys.read(window.get(), power);
      pending.forward = next.forward;
      pending.right = next.right;
      pending.yaw_delta += next.yaw_delta;
      pending.pitch_delta += next.pitch_delta;
      if (next.brake != brake_test::none) pending.brake = next.brake;
      pending.room_push = std::max(pending.room_push, next.room_push);
      pending.rescue |= next.rescue;
    }

    clock.advance(replaying ? 1.0 / options.fps : dt, [&] {
      if (replaying && sim->current().tick >= replay_ticks) return;
      sim->step(replaying ? recorded_input(scenario, sim->current().tick + 1) : pending);
      if (replaying) trace = fingerprint(trace, sim->current());
      else pending = {.forward = pending.forward, .right = pending.right};
    });

    const auto [next_width, next_height] = input::framebuffer_size(window.get());
    if (next_width != 0 && next_height != 0) {
      if (next_width != width || next_height != height || render.presentable_state_waiting_host_event()) {
        vk::Device(gpu.device.device).waitIdle();
        width = next_width;
        height = next_height;
        render.resize_viewport(width, height);
      }
      const auto& state = sim->current();
      const auto info = std::format("PHY03 | Player {}{} · Dummy {}{} | G {:.1f} · H {:.1f} m/s · [ ] | {} | C colliders {}",
        mode_name(state.player.mode), state.player.recovery_blocked ? " (E rescue)" : state.player.stepping ? " (step up)" : "",
        mode_name(state.dummy.mode), state.dummy.recovery_blocked ? " (blocked)" : "",
        push_speed(power), push_speed(weak_push), camera_name(camera), show_colliders ? "on" : "off");
      if (info != input::window_title(window.get())) input::set_window_title(window.get(), info);
      if (render.can_draw() && draw(*sim, clock.alpha(), camera, show_colliders, width, height, mesh, render, context)) {
        ++frames;
      }
    }
    if ((options.frames != 0 && frames >= options.frames) || (replaying && sim->current().tick >= replay_ticks)) break;
    pacer.wait();
  }
  if (replaying)
    std::cout << "PHY03 " << options.replay << " tick " << sim->current().tick << " fingerprint " << trace << '\n';
}
}
}

int main(const int argc, const char* const* argv) {
  CLI::App app{"PHY03 - interact with the ragdoll test room"};
  phy03::viewer_options options;
  std::string camera = "level";
  app.add_option("--frames", options.frames, "Maximum number of rendered frames (0 runs interactively)");
  app.add_option("--fps", options.fps, "Target frames per second")->check(CLI::Range(15, 240))->capture_default_str();
  app.add_option("--push-speed", options.push_velocity, "Initial G push in m/s, rounded to 0.1")
    ->check(playground::finite_number)->check(CLI::Range(0.0f, phy03::push_speed(phy03::max_push)))->capture_default_str();
  app.add_flag("--validation", options.validation, "Enable Vulkan validation");
  app.add_flag("--colliders", options.show_colliders, "Start with physics collider outlines visible (C toggles)");
  app.add_option("--replay", options.replay, "Play a fixed-tick input recording")
    ->check(CLI::IsMember({"strong", "weak", "low", "push", "rescue", "overlap", "stack"}));
  app.add_option("--camera", camera, "Starting camera")->check(CLI::IsMember({"level", "head", "overview"}))->capture_default_str();
  if (const auto exit = playground::parse_cli(app, argc, argv)) return *exit;

  try {
    phy03::jolt_runtime runtime;
    options.camera = camera == "overview" ? phy03::camera_mode::overview :
      camera == "head" ? phy03::camera_mode::head : phy03::camera_mode::level;
    phy03::run_viewer(options);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
