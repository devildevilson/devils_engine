#ifndef DEVILS_ENGINE_PF05_WORLD_UI_H
#define DEVILS_ENGINE_PF05_WORLD_UI_H

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/visage/render_output.h"

namespace devils_engine::visage {
struct font_t;
}

namespace devils_engine::pf05 {

struct world_ui_field {
  std::string label;
  std::string value;
  glm::vec4 color{0.78f, 0.84f, 0.92f, 1.0f};
};

// Deliberately bounded presentation contract rather than an arbitrary immediate-mode callback.
// Gameplay supplies identity/anchor/health and at most a few short status rows/icons; the renderer owns layout.
struct world_ui_window {
  std::string id;
  std::string name;
  glm::vec3 anchor{};
  float health = 1.0f;
  std::vector<uint32_t> image_slots;
  std::vector<world_ui_field> fields;
  bool visible = true;
  bool selected = false;
};

struct world_ui_style {
  glm::vec2 size_px{196.0f, 92.0f};
  float font_px = 18.0f;
  float anchor_gap_px = 12.0f;
  // A constant-pixel window looks detached from its object. Scale instead follows perspective around
  // reference_distance, but clamps before becoming unreadably small or comically large.
  float reference_distance = 5.5f;
  float min_scale = 0.45f;
  float max_scale = 1.35f;
  float fade_start_distance = 13.0f;
  float fade_end_distance = 16.0f;
  uint32_t max_fields = 3;
  uint32_t max_images = 3;
};

struct world_ui_vertex {
  float position[2];
  float uv[2];
  uint32_t color;
  uint32_t window_id;
};
static_assert(sizeof(world_ui_vertex) == 24);

struct alignas(16) world_ui_transform {
  glm::vec4 anchor_fade_end;
  glm::vec4 pixel_offset_size;
  glm::vec4 distance_policy;
};
static_assert(sizeof(world_ui_transform) == 48);

class world_ui_builder {
public:
  world_ui_builder(const visage::font_t& font, world_ui_style style = {});
  ~world_ui_builder() noexcept;
  world_ui_builder(world_ui_builder&&) noexcept;
  world_ui_builder& operator=(world_ui_builder&&) noexcept;
  world_ui_builder(const world_ui_builder&) = delete;
  world_ui_builder& operator=(const world_ui_builder&) = delete;

  bool build(
    std::span<const world_ui_window> windows,
    const glm::mat4& view_projection,
    const glm::mat4& view,
    uint32_t viewport_width,
    uint32_t viewport_height);
  std::optional<size_t> hit_test(glm::vec2 cursor_px) const noexcept;
  std::span<const world_ui_vertex> vertices() const noexcept;
  std::span<const visage::gui_index_t> indices() const noexcept;
  std::span<const visage::gui_draw_command_t> commands() const noexcept;
  std::span<const world_ui_transform> transforms() const noexcept;

private:
  struct impl;
  std::unique_ptr<impl> state_;
};

} // namespace devils_engine::pf05

#endif
