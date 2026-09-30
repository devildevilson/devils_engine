#include "motif_viewer.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

#include "devils_engine/input/core.h"
#include "devils_engine/input/events.h"
#include "devils_engine/painter/assets_base.h"
#include "devils_engine/painter/auxiliary.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/makers.h"
#include "devils_engine/painter/system_info.h"
#include "devils_engine/painter/vulkan_header.h"
#include "devils_engine/playground/frame_pacer.h"
#include "devils_engine/playground/free_camera.h"
#include "devils_engine/playground/visage_overlay.h"
#include "devils_engine/utils/core.h"

#include "motif_demo.h"
#include "site_graph.h"
#include "viewer_detail_page.h"

// ПРОСМОТР ПЛАНА GN06. Локальная копия графа PF09 служит тонким слоем показа: те же буферы
// треугольников/линий, подсветка выбранного слота и текстовый overlay. Истина остаётся в
// `motif_layout`; вершины строятся из него заново при смене seed или режима показа. В окне нет
// генераторной логики и нет ресурсных радиусов: N/B вызывают ту же сборку с новым входным seed.

namespace gn06 {
namespace {

namespace input = devils_engine::input;
namespace painter = devils_engine::painter;
namespace playground = devils_engine::playground;
namespace utils = devils_engine::utils;

uint32_t pending_width = 1280;
uint32_t pending_height = 720;
bool resize_pending = false;
double scroll_delta = 0.0;
bool click_pending = false;
int32_t escape_key = -1;

constexpr uint32_t no_slot = UINT32_MAX;

void input_error(const int error, const char* message) noexcept {
  utils::warn("GN06 viewer input error {}: {}", error, message);
}

void key_callback(GLFWwindow* window, const int key, const int scancode, const int action, const int) noexcept {
  input::events::update_key(scancode, action);
  if (key == escape_key && action == 1) input::set_should_close(window, true);
}

void framebuffer_callback(GLFWwindow*, const int width, const int height) noexcept {
  if (width <= 0 || height <= 0) return;
  pending_width = uint32_t(width);
  pending_height = uint32_t(height);
  resize_pending = true;
}

void scroll_callback(GLFWwindow*, const double, const double amount) noexcept { scroll_delta += amount; }

void mouse_callback(GLFWwindow*, const int button, const int action, const int) noexcept {
  if (button == 0 && action == 1) click_pending = true;
}

void bind_key(const std::string_view name, const std::string_view canonical) {
  const auto [key, scancode] = input::key_from_canonical(canonical);
  if (key < 0 || scancode < 0) utils::error{}("GN06 viewer cannot resolve key '{}'", canonical);
  input::events::set_key(name, scancode, key);
}

std::vector<const char*> instance_extensions() {
  uint32_t count = 0;
  const char** required = input::get_required_instance_extensions(&count);
  return {required, required + count};
}

struct alignas(16) camera_block {
  glm::mat4 view_projection;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 viewport_near;
};
static_assert(sizeof(camera_block) == 160);

struct zone_vertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  uint32_t tint = 0;
  uint32_t slot = no_slot;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;
  uint32_t pad2 = 0;
};
static_assert(sizeof(zone_vertex) == 32);

uint32_t rgba(const uint32_t rgb) noexcept {
  return ((rgb >> 16) & 0xffu) | (rgb & 0x00ff00u) | ((rgb & 0xffu) << 16) | 0xff000000u;
}

struct geometry {
  std::vector<zone_vertex> fill;
  std::vector<zone_vertex> lines;
};

void add_vertex(std::vector<zone_vertex>& into, const float x, const float y, const float height,
                const uint32_t tint, const uint32_t slot) {
  into.push_back({x, height, y, tint, slot, 0, 0, 0});
}

void quad(geometry& out, const float x0, const float y0, const float x1, const float y1,
          const float height, const uint32_t tint, const uint32_t slot) {
  add_vertex(out.fill, x0, y0, height, tint, slot);
  add_vertex(out.fill, x1, y0, height, tint, slot);
  add_vertex(out.fill, x1, y1, height, tint, slot);
  add_vertex(out.fill, x0, y0, height, tint, slot);
  add_vertex(out.fill, x1, y1, height, tint, slot);
  add_vertex(out.fill, x0, y1, height, tint, slot);
}

struct view_point { float x, y; };

void polygon(geometry& out, const std::vector<view_point>& points, const float height,
             const uint32_t tint, const uint32_t slot) {
  if (points.size() < 3) return;
  for (size_t i = 1; i + 1 < points.size(); ++i) {
    add_vertex(out.fill, points[0].x, points[0].y, height, tint, slot);
    add_vertex(out.fill, points[i].x, points[i].y, height, tint, slot);
    add_vertex(out.fill, points[i + 1].x, points[i + 1].y, height, tint, slot);
  }
}

std::vector<view_point> outline_of(const site_view_part& part) {
  std::vector<view_point> points;
  points.reserve(part.outline.size());
  for (const auto point : part.outline) {
    points.push_back({float(point.x) / site_precision, float(point.y) / site_precision});
  }
  return points;
}

std::vector<view_point> clip_edge(const std::vector<view_point>& source, const uint32_t edge,
                                  const float limit) {
  std::vector<view_point> result;
  if (source.empty()) return result;
  const auto coordinate = [edge](const view_point point) { return edge < 2 ? point.x : point.y; };
  const auto inside = [edge, limit, &coordinate](const view_point point) {
    return edge % 2 == 0 ? coordinate(point) >= limit : coordinate(point) <= limit;
  };
  auto previous = source.back();
  for (const auto current : source) {
    const bool was_inside = inside(previous), is_inside = inside(current);
    if (was_inside != is_inside) {
      const auto previous_axis = coordinate(previous), current_axis = coordinate(current);
      const auto t = (limit - previous_axis) / (current_axis - previous_axis);
      result.push_back({previous.x + t * (current.x - previous.x),
                        previous.y + t * (current.y - previous.y)});
    }
    if (is_inside) result.push_back(current);
    previous = current;
  }
  return result;
}

std::vector<view_point> clipped_cell(const std::vector<view_point>& outline,
                                     const int32_t x, const int32_t y) {
  auto points = clip_edge(outline, 0, float(x));
  points = clip_edge(points, 1, float(x + 1));
  points = clip_edge(points, 2, float(y));
  return clip_edge(points, 3, float(y + 1));
}

void line(geometry& out, const float x0, const float y0, const float x1, const float y1,
          const uint32_t tint) {
  add_vertex(out.lines, x0, y0, 0.30f, tint, no_slot);
  add_vertex(out.lines, x1, y1, 0.30f, tint, no_slot);
}

// Общая кромка частей или открытого стыка — не стена. Вычитаем только совпавший
// участок; техническая дверь отдельно сохраняет свой контур и ориентированное полотно.
void part_boundary(geometry& out, const site_view_scene& scene, const site_view_part& part,
                   const uint32_t tint, const bool show_parts) {
  for (size_t i = 0; i < part.outline.size(); ++i) {
    const auto a = part.outline[i], b = part.outline[(i + 1) % part.outline.size()];
    const auto dx = int64_t(b.x) - a.x, dy = int64_t(b.y) - a.y;
    if (dx == 0 && dy == 0) continue;
    std::vector<std::pair<double, double>> visible{{0.0, 1.0}};
    for (const auto& zone : scene.zones) {
      for (const auto& other : zone.parts) {
        if (other.ref == part.ref || (show_parts && other.ref.area == part.ref.area)) continue;
        for (size_t j = 0; j < other.outline.size(); ++j) {
          const auto c = other.outline[j], d = other.outline[(j + 1) % other.outline.size()];
          if (dx * (int64_t(c.y) - a.y) != dy * (int64_t(c.x) - a.x) ||
              dx * (int64_t(d.y) - a.y) != dy * (int64_t(d.x) - a.x) ||
              dx * (int64_t(d.x) - c.x) + dy * (int64_t(d.y) - c.y) >= 0) continue;
          const auto tc = dx != 0 ? double(int64_t(c.x) - a.x) / dx : double(int64_t(c.y) - a.y) / dy;
          const auto td = dx != 0 ? double(int64_t(d.x) - a.x) / dx : double(int64_t(d.y) - a.y) / dy;
          const auto lo = std::max(0.0, std::min(tc, td));
          const auto hi = std::min(1.0, std::max(tc, td));
          if (lo >= hi) continue;
          std::vector<std::pair<double, double>> remaining;
          for (const auto& [start, end] : visible) {
            if (hi <= start || lo >= end)
              remaining.emplace_back(start, end);
            else {
              if (start < lo) remaining.emplace_back(start, lo);
              if (hi < end) remaining.emplace_back(hi, end);
            }
          }
          visible = std::move(remaining);
        }
      }
    }
    for (const auto& [start, end] : visible) {
      line(out, float(a.x + start * dx) / site_precision, float(a.y + start * dy) / site_precision,
           float(a.x + end * dx) / site_precision, float(a.y + end * dy) / site_precision, tint);
    }
  }
}

geometry build_geometry(const motif_view_scene& scene, const bool show_parts) {
  geometry out;
  const auto& layout = scene.layout;
  quad(out, 0.0f, 0.0f, float(layout.width), float(layout.height), -0.05f,
       rgba(0x17191d), no_slot);

  for (uint32_t i = 0; i < layout.instances.size(); ++i) {
    const auto& r = layout.instances[i].rect;
    quad(out, float(r.x), float(r.y), float(r.x + r.w), float(r.y + r.h), 0.10f,
         rgba(scene.zones[i].colour), i);
  }
  for (const auto& passage : layout.passages) {
    quad(out, float(passage.x), float(passage.y), float(passage.x + 1), float(passage.y + 1),
         0.15f, rgba(0xe1d3a5), passage.b);
  }
  quad(out, 0.0f, float(layout.entry_y), 1.0f, float(layout.entry_y + 1),
       0.15f, rgba(0xe1d3a5), no_slot);
  for (const auto& exposed : layout.exposures) {
    quad(out, float(exposed.x), float(exposed.y), float(exposed.x + 1), float(exposed.y + 1),
         0.18f, rgba(0xb8c8d9), exposed.instance);
  }

  if (!show_parts) return out;
  for (const auto& item : layout.instances) {
    const auto& r = item.rect;
    const float x0 = float(r.x), y0 = float(r.y);
    const float x1 = float(r.x + r.w), y1 = float(r.y + r.h);
    const auto tint = rgba(0xd4c5a4);
    line(out, x0, y0, x1, y0, tint);
    line(out, x1, y0, x1, y1, tint);
    line(out, x1, y1, x0, y1, tint);
    line(out, x0, y1, x0, y0, tint);
  }
  return out;
}

geometry build_geometry(const motif_view_scene& scene, const bool show_parts, const bool) {
  return build_geometry(scene, show_parts);
}

geometry build_geometry(const site_view_scene& scene, const bool show_parts,
                        const bool show_route) {
  geometry out;
  quad(out, 0.0f, 0.0f, float(scene.layout.width), float(scene.layout.height), -0.05f,
       rgba(0x17191d), no_slot);
  for (uint32_t slot = 0; slot < scene.zones.size(); ++slot) {
    const auto& zone = scene.zones[slot];
    for (const auto& part : zone.parts) {
      const auto outline = outline_of(part);
      const auto accent = std::find_if(scene.endings.begin(), scene.endings.end(),
        [&](const site_path_ending& ending) { return ending.accent_part == part.ref; });
      const auto part_colour = accent == scene.endings.end() ? zone.colour : accent->colour;
      polygon(out, outline, 0.10f, rgba(part_colour), slot);
      for (int32_t y = part.bounds.y; y < part.bounds.y + part.bounds.h; ++y) {
        for (int32_t x = part.bounds.x; x < part.bounds.x + part.bounds.w; ++x) {
          const auto cell = size_t(y) * scene.layout.width + x;
          if (scene.part_owner[cell] != part.ref) continue;
          uint32_t colour = part_colour;
          if (scene.exposed[cell] != 0) colour = 0xb8c8d9u;
          if (colour != part_colour)
            polygon(out, clipped_cell(outline, x, y), 0.11f, rgba(colour), slot);
        }
      }
    }
  }
  for (const auto& zone : scene.zones) {
    const auto tint = rgba(zone.connector ? (zone.glyph == 'D' ? 0xffe7a3 : 0x83d8cd) : 0xd4c5a4);
    for (const auto& part : zone.parts) {
      const auto& r = part.bounds;
      const float x0 = float(r.x), y0 = float(r.y);
      const float x1 = float(r.x + r.w);
      part_boundary(out, scene, part, tint, show_parts);
      if (zone.connector && zone.glyph == 'D') {
        if (part.outline.size() == 4) {
          const auto& p = part.outline;
          line(out, float(p[1].x + p[2].x) / (2 * site_precision), float(p[1].y + p[2].y) / (2 * site_precision),
            float(p[0].x + p[3].x) / (2 * site_precision), float(p[0].y + p[3].y) / (2 * site_precision), tint);
        }
        const auto cx = x0 + r.w * 0.5f, cy = y0 + r.h * 0.5f;
        line(out, cx, cy - 0.82f, cx + 0.82f, cy, tint);
        line(out, cx + 0.82f, cy, cx, cy + 0.82f, tint);
        line(out, cx, cy + 0.82f, cx - 0.82f, cy, tint);
        line(out, cx - 0.82f, cy, cx, cy - 0.82f, tint);
      } else if (zone.connector) {
        line(out, x0 - 0.20f, y0 + 0.35f, x1 + 0.20f, y0 + 0.35f, tint);
        line(out, x0 - 0.20f, y0 + 0.65f, x1 + 0.20f, y0 + 0.65f, tint);
      }
    }
  }
  if (show_route)
    for (const auto& span : scene.route_spans)
      line(out, float(span.a.x) / site_precision, float(span.a.y) / site_precision,
           float(span.b.x) / site_precision, float(span.b.y) / site_precision, rgba(0x42f5d1u));
  return out;
}

uint32_t pick_zone(const motif_view_scene& scene, const double x, const double y) noexcept {
  for (uint32_t i = 0; i < scene.layout.instances.size(); ++i) {
    const auto& r = scene.layout.instances[i].rect;
    if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) return i;
  }
  for (const auto& passage : scene.layout.passages)
    if (x >= passage.x && y >= passage.y && x < passage.x + 1 && y < passage.y + 1)
      return passage.b;
  return no_slot;
}

site_part_ref pick_part(const motif_view_scene&, const double, const double) noexcept {
  return {};
}

site_part_ref pick_part(const site_view_scene& scene, const double x, const double y) noexcept {
  if (x < 0 || y < 0 || x >= scene.layout.width || y >= scene.layout.height) return {};
  for (const auto& zone : scene.zones)
    for (const auto& part : zone.parts) {
      if (x < part.bounds.x || y < part.bounds.y ||
          x >= part.bounds.x + part.bounds.w || y >= part.bounds.y + part.bounds.h) continue;
      bool inside = true;
      for (size_t i = 0; i < part.outline.size(); ++i) {
        const auto a = part.outline[i], b = part.outline[(i + 1) % part.outline.size()];
        if (double(b.x - a.x) * (y * site_precision - a.y) -
              double(b.y - a.y) * (x * site_precision - a.x) <
            0.0) {
          inside = false;
          break;
        }
      }
      if (inside) return part.ref;
    }
  return {};
}

uint32_t pick_zone(const site_view_scene& scene, const double x, const double y) noexcept {
  const auto ref = pick_part(scene, x, y);
  return ref.valid() ? ref.area : no_slot;
}

std::vector<std::string> zone_details(const motif_view_scene& scene, const uint32_t selected,
                                      const uint32_t hovered, const bool show_parts,
                                      const std::string_view status) {
  std::vector<std::string> lines;
  const auto& layout = scene.layout;
  lines.push_back(std::format("seed {}  rules v{}  {} {}x{}", scene.seed, scene.catalogue_version,
                              scene.scale, layout.width, layout.height));
  lines.push_back(std::format("{} zones  {} passages  {} surface contacts  {} view",
                              layout.instances.size(), layout.passages.size(), layout.exposures.size(),
                              show_parts ? "parts" : "areas"));
  if (!status.empty()) lines.push_back(std::string(status));
  const auto index = selected != no_slot ? selected : hovered;
  if (index == no_slot || index >= layout.instances.size()) {
    lines.emplace_back("zone: hover or click a coloured area");
    return lines;
  }
  const auto& item = layout.instances[index];
  const auto& info = scene.zones[index];
  const auto& r = item.rect;
  lines.push_back(std::format("zone #{}  {} / {}{}", index, info.location, info.motif,
                              selected == index ? " [selected]" : ""));
  lines.push_back(std::format("rule {}  chance {}%  origin ({},{})  extent {}x{}  area {}",
                              info.rule, info.chance_percent, r.x, r.y, r.w, r.h, r.w * r.h));
  lines.push_back(item.parent == devils_engine::originator::no_motif_parent ?
    "parent: external entry" : std::format("parent: zone #{}", item.parent));
  std::string joins = "passages:";
  uint32_t links = 0;
  for (const auto& passage : layout.passages) {
    if (passage.a != index && passage.b != index) continue;
    ++links;
    joins += std::format(" ({},{})", passage.x, passage.y);
  }
  lines.push_back(std::format("{} {}", joins, links == 0 ? "none" : ""));
  uint32_t contacts = 0;
  for (const auto& exposure : layout.exposures) contacts += uint32_t(exposure.instance == index);
  lines.push_back(std::format("surface contacts {}", contacts));
  return lines;
}

std::vector<std::string> zone_details(const motif_view_scene& scene, const uint32_t selected,
                                      const uint32_t hovered, const bool show_parts,
                                      const bool, const std::string_view status,
                                      const site_part_ref, const site_part_ref) {
  return zone_details(scene, selected, hovered, show_parts, status);
}

std::vector<std::string> zone_details(const site_view_scene& scene, const uint32_t selected,
                                      const uint32_t hovered, const bool show_parts,
                                      const bool show_route, const std::string_view status,
                                      const site_part_ref selected_part,
                                      const site_part_ref hovered_part) {
  std::vector<std::string> lines;
  lines.push_back(std::format("seed {}  rules v{} motifs v{}  {} {}x{}", scene.seed,
    scene.catalogue_version, scene.motif_catalogue_version, scene.scale, scene.layout.width, scene.layout.height));
  const auto connectors = std::count_if(scene.zones.begin(), scene.zones.end(),
    [](const site_view_zone& zone) { return zone.connector; });
  lines.push_back(std::format("{} areas  {} transitions  {} perceptions  {} / {}",
    scene.zones.size(), connectors,
    scene.perceptions, show_parts ? "detail" : "zones",
    show_route ? "route" : "no route"));
  if (!scene.projection_quality.exact())
    lines.push_back(std::format("view loss: parts {}  false/missing links {}/{}  unreachable cells {}",
      scene.projection_quality.lost_parts, scene.projection_quality.false_links,
      scene.projection_quality.missing_links, scene.projection_quality.unreachable_cells));
  if (!status.empty()) lines.push_back(std::string(status));
  const auto id = selected != no_slot ? selected : hovered;
  if (id == no_slot || id >= scene.zones.size()) {
    lines.emplace_back("area: hover or click a coloured zone, including a door");
    return lines;
  }
  const auto ref = selected != no_slot ? selected_part : hovered_part;
  if (ref.valid() && ref.area == id)
    lines.push_back(std::format("picked part_ref ({},{})", ref.area, ref.part));
  lines.insert(lines.end(), scene.zones[id].details.begin(), scene.zones[id].details.end());
  return lines;
}

void write_buffer(painter::graphics_base& base, const std::string_view name,
                  const void* data, const size_t bytes) {
  const auto slot = base.find_resource(name);
  if (slot == painter::invalid_resource_slot) utils::error{}("GN06 viewer buffer '{}' is absent", name);
  const auto frame = base.get_current_buffer_resource_frame(slot);
  if (frame.mapped == nullptr || bytes > frame.sub.size)
    utils::error{}("GN06 viewer buffer '{}' needs {} bytes, capacity {}", name, bytes, frame.sub.size);
  if (bytes != 0) std::memcpy(static_cast<uint8_t*>(frame.mapped) + frame.sub.offset, data, bytes);
}

void write_overlay(painter::graphics_base& base, const playground::visage_overlay& overlay) {
  write_buffer(base, "ui_vertices", overlay.vertices().data(), overlay.vertices().size());
  write_buffer(base, "ui_indices", overlay.indices().data(), overlay.indices().size());
  const auto commands = overlay.commands();
  const auto frame = base.get_current_buffer_resource_frame(base.find_resource("ui_commands"));
  const uint32_t count = uint32_t(commands.size());
  auto* destination = static_cast<uint8_t*>(frame.mapped) + frame.sub.offset;
  if (sizeof(count) + commands.size_bytes() > frame.sub.size)
    utils::error{}("GN06 viewer UI command buffer is too small");
  std::memcpy(destination, &count, sizeof(count));
  if (!commands.empty()) std::memcpy(destination + sizeof(count), commands.data(), commands.size_bytes());
}

void bind_ui_textures(painter::graphics_base& base, const painter::assets_base& assets) {
  const auto slot = base.find_descriptor("ui_textures");
  if (slot == painter::invalid_resource_slot) utils::error{}("GN06 viewer has no UI textures descriptor");
  auto& descriptor = base.descriptors[slot];
  const vk::ImageView fallback(assets.default_texture_view());
  std::vector<vk::DescriptorImageInfo> images(descriptor.texture_count);
  for (uint32_t i = 0; i < descriptor.texture_count; ++i) {
    vk::ImageView view;
    if (i < assets.texture_slots.size()) view = assets.texture_slots[i].view;
    images[i] = vk::DescriptorImageInfo(vk::Sampler{}, view ? view : fallback,
                                        vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  std::vector<vk::WriteDescriptorSet> writes;
  for (const auto raw_set : descriptor.sets) {
    if (raw_set == VK_NULL_HANDLE) continue;
    vk::WriteDescriptorSet write;
    write.dstSet = raw_set;
    write.dstBinding = uint32_t(descriptor.layout.size());
    write.descriptorCount = descriptor.texture_count;
    write.descriptorType = vk::DescriptorType::eSampledImage;
    write.pImageInfo = images.data();
    writes.push_back(write);
  }
  vk::Device(base.device).updateDescriptorSets(writes, nullptr);
}

} // namespace

void verify_site_viewer_details(const site_view_scene& scene) {
  for (uint32_t id = 0; id < scene.zones.size(); ++id) {
    for (const auto& part : scene.zones[id].parts) {
      for (const auto selected : {false, true}) {
        const auto details = zone_details(scene, selected ? id : no_slot, id, false, true,
          "viewer verification status", part.ref, part.ref);
        const auto first = make_viewer_detail_page(details, 0, playground::visage_overlay::max_detail_lines);
        std::vector<std::string> recovered;
        for (size_t index = 0; index < first.count; ++index) {
          const auto page = make_viewer_detail_page(details, index, playground::visage_overlay::max_detail_lines);
          if (page.lines.size() > playground::visage_overlay::max_detail_lines || page.index != index) {
            throw std::runtime_error("site viewer verify: properties exceed the panel capacity");
          }
          if (first.count == 1) {
            recovered = page.lines;
          } else {
            if (index == 0) recovered.assign(page.lines.begin(), page.lines.begin() + 2);
            recovered.insert(recovered.end(), page.lines.begin() + 2, page.lines.end() - 1);
          }
        }
        if (recovered != details) {
          throw std::runtime_error("site viewer verify: pagination lost or reordered properties");
        }
      }
    }
  }
}

template<class SceneFactory>
int run_viewer(const uint64_t initial_seed, SceneFactory&& make_scene,
               const std::string_view mode, const uint32_t frame_limit) {
  auto scene = make_scene(initial_seed);
  pending_width = 1280;
  pending_height = 720;
  resize_pending = false;
  scroll_delta = 0.0;
  click_pending = false;

  input::init input_runtime(&input_error);
  input::events::init();
  painter::load_dispatcher1();

  vk::ApplicationInfo app_info{};
  app_info.pApplicationName = "GN06 zones";
  app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.pEngineName = "devils_engine";
  app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
  app_info.apiVersion = VK_API_VERSION_1_0;
  const auto extensions = instance_extensions();
  vk::InstanceCreateInfo instance_info{};
  instance_info.pApplicationInfo = &app_info;
  instance_info.enabledExtensionCount = uint32_t(extensions.size());
  instance_info.ppEnabledExtensionNames = extensions.data();
  const VkInstance instance = vk::createInstance(instance_info);
  painter::load_dispatcher2(instance);

  GLFWwindow* window = input::create_window(pending_width, pending_height,
    mode == "site" ? "GN06 — site areas" : "GN06 — motif zones");
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (input::create_window_surface(instance, window, nullptr, &surface) != uint32_t(vk::Result::eSuccess))
    utils::error{}("GN06 viewer could not create a Vulkan surface");
  painter::system_info system(instance);
  system.check_devices_surface_capability(surface);
  const auto physical = system.choose_physical_device();
  const auto queue_plan = painter::make_device_queue_plan(physical);
  painter::device_maker device_maker(instance);
  device_maker.beginDevice(physical.handle);
  for (uint32_t i = 0; i < queue_plan.request_count; ++i)
    device_maker.createQueue(queue_plan.requests[i].family, queue_plan.requests[i].count);
  device_maker.features(vk::PhysicalDevice(physical.handle).getFeatures());
  device_maker.setExtensions(painter::default_device_extensions);
  const VkDevice device = device_maker.create({}, "gn06.motif_viewer.device");
  painter::load_dispatcher3(device);
  const auto queues = painter::device_queues::get(device, queue_plan);

  {
    painter::graphics_base base(instance, device, physical.handle, painter::presentation_engine_type::main);
    base.create_allocator();
    base.create_command_pool(queues.graphics);
    base.create_descriptor_pool();
    const std::string resource_root = std::string(GN06_RESOURCE_ROOT) + "/viewer/";
    const std::string cache_path = utils::cache_folder() + "gn06_zones_viewer.pipeline_cache";
    std::error_code cache_error;
    std::filesystem::create_directories(std::filesystem::path(cache_path).parent_path(), cache_error);
    base.get_or_create_pipeline_cache(cache_path);
    base.set_shader_source_filesystem(resource_root + "shaders/");
    base.set_startup_graph("gn06_zones");
    auto render_config = painter::build_render_config(resource_root + "render_config/");
    if (base.commit_parsed_resources(render_config) != 0)
      utils::error{}("GN06 viewer could not load its display graph");
    base.set_surface(surface, pending_width, pending_height);
    base.resize_viewport(pending_width, pending_height);
    base.populate_constant_default_values();
    const auto graph = base.find_render_graph("gn06_zones");
    if (graph == painter::invalid_resource_slot) utils::error{}("GN06 viewer display graph is absent");
    base.change_render_graph(graph);

    painter::assets_base assets(device, physical.handle);
    assets.create_fence();
    assets.create_allocator(instance);
    assets.create_command_buffer(queues.transfer, queues.graphics);
    assets.set_graphics_base(&base);
    assets.create_default_texture();
    const std::string common = std::string(PLAYGROUND_COMMON_RESOURCE_ROOT) + "/";
    playground::visage_overlay overlay(common + "fonts/crimson.roman.ttf", common + "ui/lab_overlay.lua",
      playground::overlay_description{mode == "site" ? "GN06 site areas" : "GN06 motif zones",
        mode == "site" ? "functional + technical areas | convex geometry" :
                         "legacy rectangular blocks + passages",
        mode == "site" ?
          "WASD pan | wheel zoom | LMB select | N/B seed | Tab parts | R route | F fit | Esc quit" :
          "WASD pan | wheel zoom | LMB select | N/B seed | Tab parts | F fit | Esc quit"});
    const auto atlas = overlay.font_atlas();
    const auto font_texture = assets.register_texture_storage("playground.crimson_roman");
    assets.create_texture_storage(font_texture,
      painter::texture_create_info{{atlas.width, atlas.height, 1}, VK_FORMAT_R8G8B8A8_UNORM});
    assets.populate_texture_storage(font_texture, atlas.bytes);
    assets.mark_ready_texture_slot(font_texture);
    overlay.set_font_texture(font_texture);
    bind_ui_textures(base, assets);

    input::events::clear_bindings();
    bind_key("pan_up", "key_w");
    bind_key("pan_down", "key_s");
    bind_key("pan_left", "key_a");
    bind_key("pan_right", "key_d");
    bind_key("next_seed", "key_n");
    bind_key("previous_seed", "key_b");
    bind_key("toggle_parts", "tab");
    bind_key("toggle_route", "key_r");
    bind_key("fit_view", "key_f");
    bind_key("previous_properties", "page_up");
    bind_key("next_properties", "page_down");
    escape_key = input::glfw_key_from_canonical("escape");
    input::set_window_callback(window, &key_callback);
    input::set_framebuffer_size_callback(window, &framebuffer_callback);
    input::set_window_callback(window, &scroll_callback);
    input::set_window_callback(window, &mouse_callback);

    bool show_parts = false;
    bool show_route = mode == "site";
    bool next_latch = false, previous_latch = false, parts_latch = false;
    bool route_latch = false, fit_latch = false;
    bool previous_properties_latch = false, next_properties_latch = false;
    size_t properties_page = 0;
    uint32_t inspected = no_slot;
    site_part_ref inspected_part;
    uint32_t selected = no_slot;
    site_part_ref selected_part;
    uint64_t requested_seed = initial_seed;
    std::string status;
    geometry drawing = build_geometry(scene, show_parts, show_route);
    double centre_x = 0.0, centre_y = 0.0, span = 64.0;
    const auto fit_scene = [&](const auto& current) {
      int32_t left = 0, top = current.layout.entry_y;
      int32_t right = 1, bottom = current.layout.entry_y + 1;
      for (const auto& item : current.layout.instances) {
        left = std::min(left, item.rect.x);
        top = std::min(top, item.rect.y);
        right = std::max(right, item.rect.x + item.rect.w);
        bottom = std::max(bottom, item.rect.y + item.rect.h);
      }
      centre_x = double(left + right) * 0.5;
      centre_y = double(top + bottom) * 0.5;
      const auto aspect = double(std::max(pending_width, 1u)) / std::max(pending_height, 1u);
      span = std::max({12.0, double(right - left + 4) * 1.25,
                       double(bottom - top + 4) * aspect * 1.25});
    };
    fit_scene(scene);
    auto previous_time = std::chrono::steady_clock::now();
    const auto start_time = previous_time;
    playground::frame_pacer pacer(60u);
    painter::graphics_ctx context;
    context.base = &base;
    context.assets = &assets;
    uint32_t drawn_frames = 0;

    while (!input::should_close(window)) {
      input::poll_events();
      const auto now = std::chrono::steady_clock::now();
      const auto dt = std::clamp(std::chrono::duration<double>(now - previous_time).count(), 0.0, 0.1);
      previous_time = now;
      input::events::update(size_t(dt * 1.0e6));
      if (resize_pending) {
        vk::Device(device).waitIdle();
        base.resize_viewport(pending_width, pending_height);
        resize_pending = false;
      }
      const auto pressed_once = [](const std::string_view name, bool& latch) {
        const bool pressed = input::events::is_pressed(name);
        const bool fired = pressed && !latch;
        latch = pressed;
        return fired;
      };
      const bool next = pressed_once("next_seed", next_latch);
      const bool previous = pressed_once("previous_seed", previous_latch);
      if (next || previous) {
        requested_seed += next ? 1 : uint64_t(-1);
        try {
          auto replacement = make_scene(requested_seed);
          scene = std::move(replacement);
          selected = no_slot;
          selected_part = {};
          properties_page = 0;
          inspected = no_slot;
          inspected_part = {};
          status.clear();
          drawing = build_geometry(scene, show_parts, show_route);
          fit_scene(scene);
          utils::info("GN06 viewer: generated seed {} with {} zones", scene.seed, scene.zones.size());
        } catch (const std::exception& error) {
          status = std::format("seed {} refused: {}", requested_seed, error.what());
          utils::warn("GN06 viewer: {}", status);
        }
      }
      if (pressed_once("toggle_parts", parts_latch)) {
        show_parts = !show_parts;
        drawing = build_geometry(scene, show_parts, show_route);
      }
      if (pressed_once("toggle_route", route_latch)) {
        show_route = !show_route;
        drawing = build_geometry(scene, show_parts, show_route);
      }
      if (pressed_once("fit_view", fit_latch)) fit_scene(scene);
      if (scroll_delta != 0.0) {
        span = std::clamp(span * std::pow(0.85, scroll_delta), 12.0, double(scene.layout.width) * 4.0);
        scroll_delta = 0.0;
      }
      const double pan = span * 0.7 * dt;
      centre_x += (double(input::events::is_pressed("pan_right")) -
                   double(input::events::is_pressed("pan_left"))) * pan;
      centre_y += (double(input::events::is_pressed("pan_down")) -
                   double(input::events::is_pressed("pan_up"))) * pan;

      const auto [mouse_x, mouse_y] = input::cursor_pos(window);
      const double aspect = double(std::max(pending_width, 1u)) / std::max(pending_height, 1u);
      const double height = span / aspect;
      const double pointer_x = centre_x + (mouse_x / double(std::max(pending_width, 1u)) - 0.5) * span;
      const double pointer_y = centre_y + (mouse_y / double(std::max(pending_height, 1u)) - 0.5) * height;
      const auto hovered = pick_zone(scene, pointer_x, pointer_y);
      const auto hovered_part = pick_part(scene, pointer_x, pointer_y);
      if (click_pending) {
        selected = hovered;
        selected_part = hovered_part;
        click_pending = false;
      }
      const auto inspected_now = selected != no_slot ? selected : hovered;
      const auto part_now = selected != no_slot ? selected_part : hovered_part;
      if (inspected_now != inspected || part_now != inspected_part) {
        properties_page = 0;
        inspected = inspected_now;
        inspected_part = part_now;
      }
      const auto previous_properties = pressed_once("previous_properties", previous_properties_latch);
      const auto next_properties = pressed_once("next_properties", next_properties_latch);
      if (previous_properties && properties_page != 0) --properties_page;
      if (next_properties) ++properties_page;
      const auto details = zone_details(scene, selected, hovered, show_parts, show_route,
        status, selected_part, hovered_part);
      const auto page = make_viewer_detail_page(details, properties_page,
        playground::visage_overlay::max_detail_lines);
      properties_page = page.index;
      overlay.set_detail_lines(page.lines);

      if (!base.can_draw()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        continue;
      }
      base.prepare_frame();
      const float fov = glm::radians(50.0f);
      const float distance = float((span * 0.5) / (std::tan(fov * 0.5f) * aspect));
      const glm::vec3 target{float(centre_x), 0.0f, float(centre_y)};
      const glm::vec3 eye = target + glm::vec3(0.0f, distance, 0.0f);
      camera_block camera{};
      camera.view = glm::lookAtRH(eye, target, glm::vec3(0.0f, 0.0f, -1.0f));
      camera.view_projection = playground::infinite_reverse_z_projection(fov, float(aspect), 0.25f) * camera.view;
      camera.camera_position = glm::vec4(float(centre_x), float(centre_y),
        std::bit_cast<float>(selected), std::bit_cast<float>(hovered));
      camera.viewport_near = glm::vec4(float(pending_width), float(pending_height), 0.1f, float(font_texture));
      write_buffer(base, "camera_buffer", &camera, sizeof(camera));
      write_buffer(base, "fill_vertices", drawing.fill.data(), drawing.fill.size() * sizeof(zone_vertex));
      write_buffer(base, "line_vertices", drawing.lines.data(), drawing.lines.size() * sizeof(zone_vertex));
      const VkDrawIndirectCommand fill_command{uint32_t(drawing.fill.size()), 1, 0, 0};
      const VkDrawIndirectCommand line_command{uint32_t(drawing.lines.size()), 1, 0, 0};
      const VkDrawIndirectCommand label_command{0, 1, 0, 0};
      base.write_constant_data(base.find_constant("fill_draw"), fill_command);
      base.write_constant_data(base.find_constant("line_draw"), line_command);
      base.write_constant_data(base.find_constant("label_draw"), label_command);
      base.update_event();

      const uint64_t delta_us = uint64_t(std::max(dt, 1.0e-6) * 1.0e6);
      const uint64_t stamp_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start_time).count());
      overlay.update(delta_us, stamp_us);
      write_overlay(base, overlay);
      context.prepare();
      context.draw();
      base.submit_frame();
      pacer.wait();
      if (frame_limit != 0 && ++drawn_frames >= frame_limit) break;
    }
    vk::Device(device).waitIdle();
    base.dump_cache_on_disk(cache_path);
  }

  vk::Instance(instance).destroy(surface);
  input::destroy(window);
  vk::Device(device).destroy();
  vk::Instance(instance).destroy();
  return 0;
}

int run_motif_viewer(const uint64_t initial_seed, const std::string_view scale,
                     const size_t requested_side, const int32_t entry_y,
                     const bool surface_cut, const std::string_view catalogue_text,
                     const uint32_t frame_limit) {
  return run_viewer(initial_seed, [&](const uint64_t seed) {
    return make_motif_view_scene(seed, scale, requested_side, entry_y, surface_cut, catalogue_text);
  }, "motifs", frame_limit);
}

int run_site_viewer(const uint64_t initial_seed, const std::string_view scale,
                    const int32_t entry_y, const bool surface_cut,
                    const std::string_view source, const std::string_view motifs_source,
                    const uint32_t frame_limit, const site_bounds bounds) {
  return run_viewer(initial_seed, [&](const uint64_t seed) {
    return make_site_view_scene(seed, scale, entry_y, surface_cut, source, motifs_source, bounds);
  }, "site", frame_limit);
}

} // namespace gn06
