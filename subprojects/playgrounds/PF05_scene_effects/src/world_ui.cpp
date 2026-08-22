#include "world_ui.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <glm/common.hpp>

#include "devils_engine/visage/font.h"
#include "devils_engine/visage/header.h"

namespace devils_engine::pf05 {
namespace {

void* nk_allocate(nk_handle, void*, const size_t size) {
  return std::malloc(size);
}

void nk_release(nk_handle, void* memory) {
  std::free(memory);
}

nk_color color8(const int r, const int g, const int b, const int a = 255) {
  return nk_rgba(r, g, b, a);
}

} // namespace

struct world_ui_builder::impl {
  struct hit_region {
    glm::vec2 min;
    glm::vec2 max;
    float view_depth;
    size_t window_index;
  };

  world_ui_style style;
  nk_allocator allocator{};
  nk_context context{};
  nk_buffer draw_commands{};
  nk_user_font font{};
  std::vector<world_ui_vertex> vertices;
  std::vector<visage::gui_index_t> indices;
  std::vector<visage::gui_draw_command_t> commands;
  std::vector<world_ui_transform> transforms;
  std::vector<hit_region> hit_regions;

  impl(const visage::font_t& source, const world_ui_style requested_style) : style(requested_style) {
    style.reference_distance = std::max(style.reference_distance, 0.001f);
    style.min_scale = std::max(style.min_scale, 0.01f);
    style.max_scale = std::max(style.max_scale, style.min_scale);
    style.fade_start_distance = std::max(style.fade_start_distance, 0.0f);
    style.fade_end_distance = std::max(style.fade_end_distance, style.fade_start_distance + 0.001f);
    allocator.alloc = &nk_allocate;
    allocator.free = &nk_release;
    font = *source.nkfont;
    font.height = style.font_px;
    nk_init(&context, &allocator, &font);
    nk_buffer_init(&draw_commands, &allocator, 4096);

    nk_style_default(&context);
    // Nuklear emits the automatic window background before nk_begin returns, i.e. before we can attach the
    // new window userdata to its current command buffer. Keep that implicit shape transparent and draw the
    // visible panel explicitly after assigning userdata below.
    context.style.window.fixed_background = nk_style_item_color(color8(0, 0, 0, 0));
    context.style.window.border_color = color8(0, 0, 0, 0);
    context.style.window.border = 0.0f;
    context.style.window.rounding = 3.0f;
    context.style.window.padding = nk_vec2(7.0f, 6.0f);
    context.style.window.spacing = nk_vec2(4.0f, 3.0f);
    context.style.text.color = color8(220, 228, 239);
    context.style.progress.normal = nk_style_item_color(color8(32, 40, 52, 245));
    context.style.progress.hover = context.style.progress.normal;
    context.style.progress.active = context.style.progress.normal;
    context.style.progress.cursor_normal = nk_style_item_color(color8(61, 200, 112, 255));
    context.style.progress.cursor_hover = context.style.progress.cursor_normal;
    context.style.progress.cursor_active = context.style.progress.cursor_normal;
    context.style.progress.rounding = 2.0f;
    context.style.progress.cursor_rounding = 2.0f;
  }

  ~impl() {
    nk_buffer_free(&draw_commands);
    nk_free(&context);
  }

  void clear_output() {
    vertices.clear();
    indices.clear();
    commands.clear();
    transforms.clear();
    hit_regions.clear();
  }
};

world_ui_builder::world_ui_builder(const visage::font_t& font, const world_ui_style style)
  : state_(std::make_unique<impl>(font, style)) {}

world_ui_builder::~world_ui_builder() noexcept = default;
world_ui_builder::world_ui_builder(world_ui_builder&&) noexcept = default;
world_ui_builder& world_ui_builder::operator=(world_ui_builder&&) noexcept = default;

bool world_ui_builder::build(
  const std::span<const world_ui_window> windows,
  const glm::mat4& view_projection,
  const glm::mat4& view,
  const uint32_t viewport_width,
  const uint32_t viewport_height) {
  auto& out = *state_;
  out.clear_output();
  nk_input_begin(&out.context);
  nk_input_end(&out.context);

  const size_t window_count = std::min<size_t>(windows.size(), 16);
  out.transforms.reserve(window_count);
  for (size_t i = 0; i < window_count; ++i) {
    const auto& window = windows[i];
    out.transforms.push_back(world_ui_transform{
      glm::vec4(window.anchor, window.visible ? out.style.fade_end_distance : 0.0f),
      glm::vec4(
        -out.style.size_px.x * 0.5f,
        -out.style.size_px.y - out.style.anchor_gap_px,
        out.style.size_px.x,
        out.style.size_px.y),
      glm::vec4(
        out.style.reference_distance,
        out.style.min_scale,
        out.style.max_scale,
        out.style.fade_start_distance)});
    if (!window.visible) continue;

    const glm::vec4 anchor_clip = view_projection * glm::vec4(window.anchor, 1.0f);
    const float view_depth = -(view * glm::vec4(window.anchor, 1.0f)).z;
    if (anchor_clip.w > 0.0f && view_depth > 0.0f && view_depth < out.style.fade_end_distance) {
      const float scale = std::clamp(
        out.style.reference_distance / view_depth,
        out.style.min_scale,
        out.style.max_scale);
      const glm::vec2 anchor_screen =
        (glm::vec2(anchor_clip) / anchor_clip.w * 0.5f + 0.5f) *
        glm::vec2(float(viewport_width), float(viewport_height));
      const glm::vec2 offset{
        -out.style.size_px.x * 0.5f,
        -out.style.size_px.y - out.style.anchor_gap_px};
      out.hit_regions.push_back(impl::hit_region{
        anchor_screen + offset * scale,
        anchor_screen + (offset + out.style.size_px) * scale,
        view_depth,
        i});
    }

    nk_handle userdata{};
    userdata.id = int(i);
    // nk_set_user_data also mutates ctx.current, which after nk_end still points at the previous window.
    // Assigning the context value directly lets the next nk_begin copy the id into the correct command buffer.
    out.context.userdata = userdata;
    const float virtual_x = float(i) * (out.style.size_px.x + 32.0f);
    const struct nk_rect bounds = nk_rect(virtual_x, 0.0f, out.style.size_px.x, out.style.size_px.y);
    const nk_flags flags = NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT;
    if (nk_begin(&out.context, window.id.c_str(), bounds, flags)) {
      // Now ctx.current is THIS window, so the public helper updates the right command buffer and forces
      // texture/userdata command boundaries for every visible primitive belonging to it.
      nk_set_user_data(&out.context, userdata);
      auto* canvas = nk_window_get_canvas(&out.context);
      nk_fill_rect(
        canvas,
        bounds,
        3.0f,
        window.selected ? color8(20, 32, 46, 234) : color8(14, 19, 27, 224));
      nk_stroke_rect(
        canvas,
        bounds,
        3.0f,
        window.selected ? 2.0f : 1.0f,
        window.selected ? color8(92, 190, 232, 255) : color8(72, 92, 118, 240));
      const uint32_t image_count = std::min<uint32_t>(uint32_t(window.image_slots.size()), out.style.max_images);
      nk_layout_row_begin(&out.context, NK_STATIC, 20.0f, int(image_count + 1u));
      nk_layout_row_push(&out.context, out.style.size_px.x - 22.0f - float(image_count) * 22.0f);
      nk_label_colored(&out.context, window.name.c_str(), NK_TEXT_LEFT, color8(244, 247, 252));
      for (uint32_t image = 0; image < image_count; ++image) {
        nk_layout_row_push(&out.context, 18.0f);
        const uint32_t packed = visage::tex_id::pack(
          visage::gui_draw_mode::image, window.image_slots[image]);
        nk_image(&out.context, nk_image_id(int(packed)));
      }
      nk_layout_row_end(&out.context);

      nk_layout_row_dynamic(&out.context, 10.0f, 1);
      const nk_color health_color = window.health < 0.30f
        ? color8(224, 70, 70)
        : (window.health < 0.60f ? color8(236, 168, 58) : color8(61, 200, 112));
      out.context.style.progress.cursor_normal = nk_style_item_color(health_color);
      out.context.style.progress.cursor_hover = out.context.style.progress.cursor_normal;
      out.context.style.progress.cursor_active = out.context.style.progress.cursor_normal;
      nk_size health = nk_size(std::clamp(window.health, 0.0f, 1.0f) * 1000.0f + 0.5f);
      nk_progress(&out.context, &health, 1000, NK_FIXED);

      const uint32_t field_count = std::min<uint32_t>(uint32_t(window.fields.size()), out.style.max_fields);
      for (uint32_t field_index = 0; field_index < field_count; ++field_index) {
        const auto& field = window.fields[field_index];
        nk_layout_row_begin(&out.context, NK_DYNAMIC, 17.0f, 2);
        nk_layout_row_push(&out.context, 0.42f);
        nk_label_colored(&out.context, field.label.c_str(), NK_TEXT_LEFT, color8(130, 145, 164));
        nk_layout_row_push(&out.context, 0.58f);
        const auto channel = [](const float value) {
          return int(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        nk_label_colored(
          &out.context,
          field.value.c_str(),
          NK_TEXT_RIGHT,
          color8(channel(field.color.r), channel(field.color.g), channel(field.color.b), channel(field.color.a)));
        nk_layout_row_end(&out.context);
      }
    }
    nk_end(&out.context);
  }

  static const nk_draw_vertex_layout_element layout[] = {
    {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(visage::gui_vertex_t, pos)},
    {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(visage::gui_vertex_t, uv)},
    {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(visage::gui_vertex_t, color)},
    {NK_VERTEX_LAYOUT_END}};
  nk_convert_config config{};
  config.vertex_layout = layout;
  config.vertex_size = sizeof(visage::gui_vertex_t);
  config.vertex_alignment = alignof(visage::gui_vertex_t);
  config.tex_null = nk_draw_null_texture{nk_handle{0}, nk_vec2(0, 0)};
  config.circle_segment_count = 16;
  config.curve_segment_count = 16;
  config.arc_segment_count = 16;
  config.global_alpha = 1.0f;
  config.shape_AA = NK_ANTI_ALIASING_OFF;
  config.line_AA = NK_ANTI_ALIASING_OFF;

  nk_buffer vertex_buffer{};
  nk_buffer index_buffer{};
  nk_buffer_init(&vertex_buffer, &out.allocator, 4096);
  nk_buffer_init(&index_buffer, &out.allocator, 2048);
  nk_buffer_clear(&out.draw_commands);
  const nk_flags converted = nk_convert(
    &out.context, &out.draw_commands, &vertex_buffer, &index_buffer, &config);
  if (converted != 0) {
    nk_buffer_free(&vertex_buffer);
    nk_buffer_free(&index_buffer);
    nk_clear(&out.context);
    out.clear_output();
    return false;
  }

  const size_t vertex_count = vertex_buffer.allocated / sizeof(visage::gui_vertex_t);
  const size_t index_count = index_buffer.allocated / sizeof(visage::gui_index_t);
  const auto* source_vertices = static_cast<const visage::gui_vertex_t*>(vertex_buffer.memory.ptr);
  const auto* source_indices = static_cast<const visage::gui_index_t*>(index_buffer.memory.ptr);
  out.vertices.resize(vertex_count);
  out.indices.assign(source_indices, source_indices + index_count);
  std::vector<uint32_t> owners(vertex_count, std::numeric_limits<uint32_t>::max());
  for (size_t i = 0; i < vertex_count; ++i) {
    out.vertices[i] = world_ui_vertex{
      {source_vertices[i].pos[0], source_vertices[i].pos[1]},
      {source_vertices[i].uv[0], source_vertices[i].uv[1]},
      source_vertices[i].color,
      0u};
  }

  uint32_t first_index = 0;
  const nk_draw_command* command = nullptr;
  nk_draw_foreach(command, &out.context, &out.draw_commands) {
    if (command->elem_count == 0) continue;
    const uint32_t window_id = uint32_t(std::max(command->userdata.id, 0));
    for (uint32_t j = 0; j < command->elem_count; ++j) {
      const uint32_t vertex_id = out.indices[first_index + j];
      if (vertex_id >= out.vertices.size() || window_id >= out.transforms.size()) continue;
      if (owners[vertex_id] == std::numeric_limits<uint32_t>::max()) {
        owners[vertex_id] = window_id;
        out.vertices[vertex_id].window_id = window_id;
        const float virtual_x = float(window_id) * (out.style.size_px.x + 32.0f);
        out.vertices[vertex_id].position[0] -= virtual_x;
      } else if (owners[vertex_id] != window_id) {
        // A converted vertex must never cross window ownership; duplicating it would be required otherwise.
        out.clear_output();
        nk_buffer_free(&vertex_buffer);
        nk_buffer_free(&index_buffer);
        nk_clear(&out.context);
        return false;
      }
    }

    visage::gui_draw_command_t gpu_command{};
    gpu_command.elem_count = command->elem_count;
    gpu_command.clip_x = 0.0f;
    gpu_command.clip_y = 0.0f;
    gpu_command.clip_w = float(viewport_width);
    gpu_command.clip_h = float(viewport_height);
    gpu_command.texture_id = uint32_t(command->texture.id);
    out.commands.push_back(gpu_command);
    first_index += command->elem_count;
  }

  nk_buffer_free(&vertex_buffer);
  nk_buffer_free(&index_buffer);
  nk_clear(&out.context);
  return true;
}

std::optional<size_t> world_ui_builder::hit_test(const glm::vec2 cursor_px) const noexcept {
  const auto& regions = state_->hit_regions;
  const impl::hit_region* nearest = nullptr;
  for (const auto& region : regions) {
    if (cursor_px.x < region.min.x || cursor_px.x > region.max.x ||
        cursor_px.y < region.min.y || cursor_px.y > region.max.y) {
      continue;
    }
    if (nearest == nullptr || region.view_depth < nearest->view_depth) nearest = &region;
  }
  return nearest == nullptr ? std::nullopt : std::optional<size_t>{nearest->window_index};
}

std::span<const world_ui_vertex> world_ui_builder::vertices() const noexcept { return state_->vertices; }
std::span<const visage::gui_index_t> world_ui_builder::indices() const noexcept { return state_->indices; }
std::span<const visage::gui_draw_command_t> world_ui_builder::commands() const noexcept { return state_->commands; }
std::span<const world_ui_transform> world_ui_builder::transforms() const noexcept { return state_->transforms; }

} // namespace devils_engine::pf05
