#ifndef DEVILS_ENGINE_VISAGE_IMAGE_H
#define DEVILS_ENGINE_VISAGE_IMAGE_H

#include <cstdint>

// Lua-facing хендл картинки UI: слот текстуры в bindless-массиве + необязательный суб-регион.
// Намеренно БЕЗ зависимостей от nuklear/vulkan — хост (tile_frontier) строит его, не зная про nuklear,
// а visage конвертирует в nk_image при отрисовке. Позже такой же хендл будет отдавать demiurg-ресурс
// (см. app.image -> будущий require/request), поэтому lua-код при переходе не меняется.

namespace devils_engine {
namespace visage {

struct image {
  uint32_t texture_id = 0;           // слот в bindless-массиве textures (= gpu_index ресурса), 0..7
  uint16_t w = 0, h = 0;             // полный размер картинки в текселях
  uint16_t region[4] = {0, 0, 0, 0}; // суб-регион (x, y, w, h) в текселях; region[2]==0 ⇒ вся картинка
};

} // namespace visage
} // namespace devils_engine

#endif
