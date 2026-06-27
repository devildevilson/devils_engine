#include "font_atlas_resource.h"

#include <utility>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

font_atlas_resource::font_atlas_resource(std::vector<uint8_t> atlas_bytes, uint32_t w, uint32_t h, uint32_t channels)
  : stash(std::move(atlas_bytes)), stash_w(w), stash_h(h), stash_channels(channels)
{
  // ключ хранилища текстуры в assets_base (register_texture_storage(path)) — синтетический путь
  path = "font/atlas";
  id = path;
  // рендер вызывает render_bind_textures только для ресурсов с этим type_id
  loading_type_id = utils::type_id<texture_resource>();
  // texture_resource заливается как R8G8B8A8 (4 канала) — атлас должен быть mtsdf (4ch)
  if (stash_channels != 4) {
    utils::warn("font_atlas_resource: ожидалось 4 канала (mtsdf), получено {} — заливка как RGBA может быть некорректной", stash_channels);
  }
}

void font_atlas_resource::load_cold(const utils::safe_handle_t& handle) {
  // никакого диска: отдаём уже сгенерированные байты в поля texture_resource
  width = stash_w;
  height = stash_h;
  memory = std::move(stash);
  utils::info("font_atlas_resource '{}': {}x{} ({} bytes) готов на CPU", path, width, height, memory.size());
}

}
}
