#include <devils_engine/catalogue/logging.h>
#include "font_resource.h"

#include <utility>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <devils_engine/demiurg/module_interface.h>

#include "font.h"
#include "font_atlas_packer.h"

namespace devils_engine {
namespace visage {

font_resource::font_resource() {
  set_flag(demiurg::resource_flags::binary, true);
}

int32_t font_resource::top_state() const {
  return static_cast<int32_t>(demiurg::state::hot) + 1; // 4 состояния: unloaded/ttf/msdf/gpu → top=3
}

bool font_resource::is_external_step(const int32_t from) const {
  return from == static_cast<int32_t>(demiurg::state::hot); // только 2→3 (GPU-заливка) на потоке рендера
}

void font_resource::load_step(const int32_t from, const utils::safe_handle_t& handle) {
  switch (from) {
    case 0: { // ttf → память (через модуль реестра, как любой demiurg-ресурс)
      if (module == nullptr) utils::error{}("font_resource '{}': no demiurg module (resource must come from a registry)", id);
      ttf_bytes = module->load_binary(path);
      if (ttf_bytes.empty()) utils::error{}("font_resource '{}': could not read ttf '{}'", id, path);
    } break;

    case 1: { // MSDF: атлас (RGBA) + метрики глифов
      font_atlas_packer packer;
      packer.setup_font(ttf_bytes, "ttf");

      font_atlas_packer::config fcfg{};
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
      memory = std::move(img.bytes); // gpu_texture_resource::memory — зальётся на GPU в шаге 2→3
      ttf_bytes.clear(); ttf_bytes.shrink_to_fit();

      DE_LOG(catalogue::log_domain::ui, flow, "font_resource: MSDF atlas {}x{}x{}ch, {} glyphs", width, height, img.channels, font_ptr->glyphs.size());
    } break;

    case 2: // GPU-заливка (переиспользуем базу)
      painter::gpu_texture_resource::load_warm(handle);
      break;

    default: break;
  }
}

void font_resource::unload_step(const int32_t from, const utils::safe_handle_t& handle) {
  switch (from) {
    case 3: painter::gpu_texture_resource::unload_hot(handle); break; // GPU
    case 2: memory.clear(); memory.shrink_to_fit(); break;           // атлас-байты
    case 1: ttf_bytes.clear(); ttf_bytes.shrink_to_fit(); break;     // ttf
    default: break;
  }
}

}
}
