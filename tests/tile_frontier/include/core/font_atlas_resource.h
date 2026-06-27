#ifndef TILE_FRONTIER_CORE_FONT_ATLAS_RESOURCE_H
#define TILE_FRONTIER_CORE_FONT_ATLAS_RESOURCE_H

#include <cstdint>
#include <vector>

#include "texture_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Атлас шрифта на GPU как обычная текстура в таблице ассетов (как grass-текстуры), но без
// чтения с диска: байты MSDF-атласа генерятся в рантайме (font_atlas_packer на главном потоке)
// и передаются сюда. Дальше идёт по ШТАТНОМУ пути texture_resource:
//   cold→warm (load_cold, поток ассетов): отдаём заранее сгенерированные байты в memory;
//   warm→hot  (load_warm, поток рендера): наследуется от texture_resource — заливка в
//             texture storage, gpu_index = слот, render_bind_textures подхватит слот в дескриптор.
// loading_type_id выставлен в type_id<texture_resource>(), чтобы рендер после заливки вызвал
// render_bind_textures (он матчит именно этот type_id).
// ВРЕМЕННО: создание шрифта/атласа позже уедет в demiurg как полноценный ресурс.
class font_atlas_resource : public texture_resource {
public:
  font_atlas_resource(std::vector<uint8_t> atlas_bytes, uint32_t w, uint32_t h, uint32_t channels);

  void load_cold(const utils::safe_handle_t& handle) override;

private:
  std::vector<uint8_t> stash; // байты атласа до перехода в memory на load_cold
  uint32_t stash_w;
  uint32_t stash_h;
  uint32_t stash_channels;
};

}
}

#endif
