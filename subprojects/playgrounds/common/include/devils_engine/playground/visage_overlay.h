#ifndef DEVILS_ENGINE_PLAYGROUND_VISAGE_OVERLAY_H
#define DEVILS_ENGINE_PLAYGROUND_VISAGE_OVERLAY_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <devils_engine/visage/render_output.h>

namespace devils_engine::visage {
struct font_t;
struct input_snapshot_t;
}

namespace devils_engine::playground {

struct overlay_description {
  std::string title;
  std::string scene;
  std::string controls;
};

struct rgba_image_view {
  std::span<const uint8_t> bytes;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Small Visage shell shared by graphical playgrounds. The default update overload remains non-interactive;
// a lab may explicitly provide an input snapshot and exchange a bounded set of host-owned environment values.
// The playground host remains responsible for uploading the produced POD buffers through Painter.
class visage_overlay {
public:
  visage_overlay(std::string font_path, std::string script_path, overlay_description description);
  ~visage_overlay() noexcept;
  visage_overlay(visage_overlay&&) noexcept;
  visage_overlay& operator=(visage_overlay&&) noexcept;
  visage_overlay(const visage_overlay&) = delete;
  visage_overlay& operator=(const visage_overlay&) = delete;

  rgba_image_view font_atlas() const noexcept;
  // CPU metrics belong to the same atlas returned above. World-space MSDF fixtures can therefore
  // reuse Crimson without building a second atlas or depending on Nuklear's private vertex stream.
  const visage::font_t& font_metrics() const noexcept;
  void set_font_texture(uint32_t texture_slot);
  // Optional lab-owned diagnostic rows rendered below the common scene/controls/frame meter.
  // Strings are copied into the overlay's Lua environment and may be replaced every frame.
  void set_detail_lines(std::span<const std::string> lines);
  void set_number(std::string_view name, double value);
  double number(std::string_view name, double fallback) const;
  void set_boolean(std::string_view name, bool value);
  bool boolean(std::string_view name, bool fallback) const;
  bool update(uint64_t frame_delta_us, uint64_t timestamp_us);
  bool update(
    const visage::input_snapshot_t& input,
    uint64_t frame_delta_us,
    uint64_t timestamp_us);
  bool update_pointer(
    float mouse_x,
    float mouse_y,
    bool mouse_left,
    uint64_t frame_delta_us,
    uint64_t timestamp_us);

  std::span<const uint8_t> vertices() const noexcept;
  std::span<const uint8_t> indices() const noexcept;
  std::span<const visage::gui_draw_command_t> commands() const noexcept;

private:
  struct impl;
  std::unique_ptr<impl> state_;
};

} // namespace devils_engine::playground

#endif
