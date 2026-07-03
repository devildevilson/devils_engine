#include "font_resource.h"

#include <utility>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/fileio.h>
#include <devils_engine/utils/safe_handle.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

font_resource::font_resource(std::string ttf_path) : ttf_path(std::move(ttf_path)) {
  path = "font/atlas"; // синтетический id (шрифт пока не в demiurg-реестре)
  id = path;
  loading_type_id = utils::type_id<texture_resource>();
}

int32_t font_resource::top_state() const {
  return static_cast<int32_t>(demiurg::state::hot) + 1; // 4 состояния: unloaded/ttf/msdf/gpu → top=3
}

bool font_resource::is_external_step(const int32_t from) const {
  return from == static_cast<int32_t>(demiurg::state::hot); // только 2→3 (GPU-заливка) на потоке рендера
}

void font_resource::load_step(const int32_t from, const utils::safe_handle_t& handle) {
  switch (from) {
    case 0: { // ttf → память
      ttf_bytes = file_io::read<uint8_t>(ttf_path);
      if (ttf_bytes.empty()) utils::error{}("font_resource: could not read ttf '{}'", ttf_path);
    } break;

    case 1: { // MSDF: атлас (RGBA) + метрики глифов
      visage::font_atlas_packer packer;
      packer.setup_font(ttf_bytes, "ttf");

      visage::font_atlas_packer::config fcfg{};
      fcfg.max_corner_angle = 3.0;
      fcfg.minimum_scale = 32.0;
      fcfg.pixel_range = 4.0; // = px_range в ui.frag
      fcfg.mitter_limit = 1.0;
      fcfg.color_channels = 4; // mtsdf
      fcfg.thread_count = 4;
      fcfg.save_png = false;

      auto [fonts, img] = packer.load_fonts(fcfg);
      if (fonts.empty()) utils::error{}("font_resource: packer produced no fonts");
      font_ptr = std::move(fonts.front());
      if (img.channels != 4) utils::warn("font_resource: expected 4 channels (mtsdf), got {}", img.channels);

      width = img.width;
      height = img.height;
      memory = std::move(img.bytes); // texture_resource::memory — зальётся на GPU в шаге 2→3
      ttf_bytes.clear(); ttf_bytes.shrink_to_fit();

      utils::info("font_resource: MSDF atlas {}x{}x{}ch, {} glyphs", width, height, img.channels, font_ptr->glyphs.size());
    } break;

    case 2: // GPU-заливка (переиспользуем texture_resource)
      texture_resource::load_warm(handle);
      break;

    default: break;
  }
}

void font_resource::unload_step(const int32_t from, const utils::safe_handle_t& handle) {
  switch (from) {
    case 3: texture_resource::unload_hot(handle); break;         // GPU
    case 2: memory.clear(); memory.shrink_to_fit(); break;       // атлас-байты
    case 1: ttf_bytes.clear(); ttf_bytes.shrink_to_fit(); break; // ttf
    default: break;
  }
}

}
}
