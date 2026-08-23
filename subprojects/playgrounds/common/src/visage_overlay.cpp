#include "devils_engine/playground/visage_overlay.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/fileio.h>
#include <devils_engine/visage/font.h>
#include <devils_engine/visage/font_atlas_packer.h>
#include <devils_engine/visage/system.h>

namespace devils_engine::playground {

struct visage_overlay::impl {
  std::unique_ptr<visage::font_t> font;
  std::vector<uint8_t> atlas;
  uint32_t atlas_width = 0;
  uint32_t atlas_height = 0;
  std::unique_ptr<visage::system> ui;
  double smoothed_frame_ms = 0.0;
  size_t detail_count = 0;
};

visage_overlay::visage_overlay(
  std::string font_path,
  std::string script_path,
  overlay_description description)
  : state_(std::make_unique<impl>()) {
  visage::font_atlas_packer packer;
  packer.setup_font(std::move(font_path));

  visage::font_atlas_packer::config font_config{};
  font_config.max_corner_angle = 3.0;
  font_config.minimum_scale = 32.0;
  font_config.pixel_range = 4.0;
  font_config.mitter_limit = 1.0;
  font_config.color_channels = 4;
  font_config.thread_count = 2;
  font_config.save_png = false;
  auto [fonts, image] = packer.load_fonts(font_config);
  if (fonts.empty() || image.bytes.empty() || image.width == 0 || image.height == 0) {
    utils::error{}("playground Visage overlay could not build its font atlas");
  }

  state_->font = std::move(fonts.front());
  state_->atlas = std::move(image.bytes);
  state_->atlas_width = image.width;
  state_->atlas_height = image.height;
  state_->ui = std::make_unique<visage::system>(state_->font.get());

  auto& env = state_->ui->script_env();
  env["playground_title"] = std::move(description.title);
  env["playground_scene"] = std::move(description.scene);
  env["playground_controls"] = std::move(description.controls);
  env["playground_details"] = state_->ui->script_state().create_table();
  env["playground_detail_count"] = 0;

  const auto source = file_io::read(script_path);
  auto result = state_->ui->script_state().safe_script(
    source,
    env,
    sol::script_pass_on_error,
    "@" + script_path);
  if (!result.valid()) {
    const sol::error err = result;
    utils::error{}("playground Visage overlay could not load '{}': {}", script_path, err.what());
  }
  const sol::object entry = result.return_count() > 0
                              ? result.get<sol::object>()
                              : sol::make_object(state_->ui->script_state(), sol::nil);
  state_->ui->set_entry_point(entry);
}

visage_overlay::~visage_overlay() noexcept = default;
visage_overlay::visage_overlay(visage_overlay&&) noexcept = default;
visage_overlay& visage_overlay::operator=(visage_overlay&&) noexcept = default;

rgba_image_view visage_overlay::font_atlas() const noexcept {
  return {state_->atlas, state_->atlas_width, state_->atlas_height};
}

const visage::font_t& visage_overlay::font_metrics() const noexcept {
  return *state_->font;
}

void visage_overlay::set_font_texture(const uint32_t texture_slot) {
  state_->font->set_texture_id(texture_slot);
}

void visage_overlay::set_detail_lines(const std::span<const std::string> lines) {
  constexpr size_t max_detail_lines = 14;
  if (lines.size() > max_detail_lines) {
    utils::error{}("playground Visage overlay accepts at most {} detail lines, got {}", max_detail_lines, lines.size());
  }

  auto& env = state_->ui->script_env();
  sol::table details = env["playground_details"];
  for (size_t i = 0; i < lines.size(); ++i) {
    details[i + 1] = lines[i];
  }
  for (size_t i = lines.size(); i < state_->detail_count; ++i) {
    details[i + 1] = sol::nil;
  }
  state_->detail_count = lines.size();
  env["playground_detail_count"] = state_->detail_count;
}

void visage_overlay::set_number(const std::string_view name, const double value) {
  state_->ui->script_env()[std::string(name)] = value;
}

double visage_overlay::number(const std::string_view name, const double fallback) const {
  const sol::object value = state_->ui->script_env()[std::string(name)];
  return value.valid() && value.is<double>() ? value.as<double>() : fallback;
}

void visage_overlay::set_boolean(const std::string_view name, const bool value) {
  state_->ui->script_env()[std::string(name)] = value;
}

bool visage_overlay::boolean(const std::string_view name, const bool fallback) const {
  const sol::object value = state_->ui->script_env()[std::string(name)];
  return value.valid() && value.is<bool>() ? value.as<bool>() : fallback;
}

bool visage_overlay::update(
  const visage::input_snapshot_t& input,
  const uint64_t frame_delta_us,
  const uint64_t timestamp_us) {
  const double frame_ms = std::max(double(frame_delta_us) / 1000.0, 0.001);
  state_->smoothed_frame_ms = state_->smoothed_frame_ms == 0.0
                                ? frame_ms
                                : state_->smoothed_frame_ms * 0.9 + frame_ms * 0.1;
  state_->ui->set_env_number("playground_frame_ms", state_->smoothed_frame_ms);
  state_->ui->set_env_number("playground_fps", 1000.0 / state_->smoothed_frame_ms);

  state_->ui->input(input);
  if (!state_->ui->update(size_t(frame_delta_us), size_t(timestamp_us), timestamp_us ^ 0x9e3779b97f4a7c15ull)) {
    return false;
  }
  return state_->ui->convert();
}

bool visage_overlay::update(const uint64_t frame_delta_us, const uint64_t timestamp_us) {
  return update(visage::input_snapshot_t{}, frame_delta_us, timestamp_us);
}

bool visage_overlay::update_pointer(
  const float mouse_x,
  const float mouse_y,
  const bool mouse_left,
  const uint64_t frame_delta_us,
  const uint64_t timestamp_us) {
  visage::input_snapshot_t input{};
  input.mouse_x = mouse_x;
  input.mouse_y = mouse_y;
  input.mouse_left = mouse_left;
  return update(input, frame_delta_us, timestamp_us);
}

std::span<const uint8_t> visage_overlay::vertices() const noexcept {
  return state_->ui->vertices();
}

std::span<const uint8_t> visage_overlay::indices() const noexcept {
  return state_->ui->indices();
}

std::span<const visage::gui_draw_command_t> visage_overlay::commands() const noexcept {
  return state_->ui->commands();
}

} // namespace devils_engine::playground
