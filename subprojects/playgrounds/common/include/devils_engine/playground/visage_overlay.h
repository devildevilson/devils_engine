#ifndef DEVILS_ENGINE_PLAYGROUND_VISAGE_OVERLAY_H
#define DEVILS_ENGINE_PLAYGROUND_VISAGE_OVERLAY_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <devils_engine/visage/render_output.h>

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

// Small non-interactive Visage shell shared by graphical playgrounds. It owns the CPU font atlas,
// Lua/Nuklear runtime and a smoothed frame meter; the playground host remains responsible for
// uploading the atlas and the produced POD buffers through its ordinary painter path.
class visage_overlay {
public:
  visage_overlay(std::string font_path, std::string script_path, overlay_description description);
  ~visage_overlay() noexcept;
  visage_overlay(visage_overlay&&) noexcept;
  visage_overlay& operator=(visage_overlay&&) noexcept;
  visage_overlay(const visage_overlay&) = delete;
  visage_overlay& operator=(const visage_overlay&) = delete;

  rgba_image_view font_atlas() const noexcept;
  void set_font_texture(uint32_t texture_slot);
  bool update(uint64_t frame_delta_us, uint64_t timestamp_us);

  std::span<const uint8_t> vertices() const noexcept;
  std::span<const uint8_t> indices() const noexcept;
  std::span<const visage::gui_draw_command_t> commands() const noexcept;

private:
  struct impl;
  std::unique_ptr<impl> state_;
};

} // namespace devils_engine::playground

#endif
