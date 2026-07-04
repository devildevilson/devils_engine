#ifndef DEVILS_ENGINE_PAINTER_TEXTURE_RESOURCE_H
#define DEVILS_ENGINE_PAINTER_TEXTURE_RESOURCE_H

#include "gpu_texture_resource.h"

namespace devils_engine {
namespace painter {

// png/bmp/jpg с диска -> декод stb (RGBA8) -> GPU (база gpu_texture_resource).
//   cold->warm (load_cold, поток ассетов): читает файл через demiurg-модуль + декодит в RGBA;
//   warm->hot  (load_warm, наследуется): заливка в assets_base, gpu_index.
class texture_resource : public gpu_texture_resource {
public:
  void load_cold(const utils::safe_handle_t& handle) override;
};

}
}

#endif
