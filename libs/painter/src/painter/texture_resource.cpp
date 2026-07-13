#include <vector>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <stb_image.h>

#include "texture_resource.h"

namespace devils_engine {
namespace painter {

void texture_resource::load_cold(const utils::safe_handle_t&) {
  std::vector<uint8_t> raw;
  module->load_binary(path, raw);

  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(raw.data(), int(raw.size()), &w, &h, &channels, STBI_rgb_alpha);
  if (pixels == nullptr) {
    utils::error{}("texture_resource '{}': stb failed to decode ({})", id, stbi_failure_reason());
    return;
  }

  width = uint32_t(w);
  height = uint32_t(h);
  const size_t bytes = size_t(width) * size_t(height) * 4;
  memory.assign(pixels, pixels + bytes);
  stbi_image_free(pixels);

  DE_LOG(catalogue::log_domain::resource, flow, "texture_resource '{}': decoded {}x{} ({} bytes RGBA)", id, width, height, memory.size());
}

} // namespace painter
} // namespace devils_engine
