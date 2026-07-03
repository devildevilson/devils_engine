#ifndef TILE_FRONTIER_CORE_FONT_RESOURCE_H
#define TILE_FRONTIER_CORE_FONT_RESOURCE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "texture_resource.h"
#include <devils_engine/visage/font_atlas_packer.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Шрифт как МНОГОШАГОВЫЙ ресурс — первый настоящий 4-state (demiurg 1a срез 3), проверяет
// обобщённый N-step FSM на деле:
//   0→1 (ttf):  читает ttf-файл в память (CPU);
//   1→2 (MSDF): font_atlas_packer генерит атлас (RGBA-байты в texture_resource::memory) +
//               метрики глифов font_t (CPU);
//   2→3 (GPU):  texture_resource::load_warm заливает атлас в таблицу текстур (переиспользуем).
// is_external_step: внешний (поток рендера) ТОЛЬКО 2→3; 0→1 и 1→2 локальные (CPU).
// CPU-шаги (0..2) гоняются синхронно на главном потоке в setup_visage — visage::system нужны
// метрики глифов сразу; GPU-шаг (2→3) идёт штатным асинхронным путём ассетов/рендера.
// loading_type_id = type_id<texture_resource>() — рендер после заливки зовёт render_bind_textures.
class font_resource : public texture_resource {
public:
  explicit font_resource(std::string ttf_path);

  visage::font_t* font() const noexcept { return font_ptr.get(); }

  int32_t top_state() const override;
  bool is_external_step(int32_t from) const override;
  void load_step(int32_t from, const utils::safe_handle_t& handle) override;
  void unload_step(int32_t from, const utils::safe_handle_t& handle) override;

private:
  std::string ttf_path;
  std::vector<uint8_t> ttf_bytes;
  std::unique_ptr<visage::font_t> font_ptr;
};

}
}

#endif
