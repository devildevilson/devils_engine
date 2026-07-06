#ifndef DEVILS_ENGINE_VISAGE_FONT_RESOURCE_H
#define DEVILS_ENGINE_VISAGE_FONT_RESOURCE_H

#include <cstdint>
#include <memory>
#include <vector>

#include <devils_engine/painter/gpu_texture_resource.h>

namespace devils_engine {
namespace visage {

struct font_t;

// Шрифт как МНОГОШАГОВЫЙ ресурс (demiurg N-step FSM), первый настоящий 4-state:
//   0->1 (ttf):  читает ttf-файл через demiurg-модуль в память (CPU);
//   1->2 (MSDF): font_atlas_packer генерит атлас (RGBA-байты в base memory) + метрики глифов font_t;
//   2->3 (GPU):  painter::gpu_texture_resource::load_warm заливает атлас в таблицу текстур.
// is_external_step: внешний (поток рендера) ТОЛЬКО 2->3; 0->1 и 1->2 локальные (CPU).
// Наследует painter::gpu_texture_resource (нужен лишь gpu_index + GPU-заливка) — БЕЗ texture_resource
// (png/stb шрифту не нужны). Регистрация: register_type<painter::gpu_texture_resource, font_resource>
// ("fonts", "ttf") — loading_type_id = база (рендер льёт как обычную текстуру), type_id = точный тип
// (lua/хост достают через handle.get<font_resource>()).
class font_resource : public painter::gpu_texture_resource {
public:
  font_resource();

  font_t* font() const noexcept { return font_ptr.get(); }

  int32_t top_state() const override;
  bool is_external_step(int32_t from) const override;
  void load_step(int32_t from, const utils::safe_handle_t& handle) override;
  void unload_step(int32_t from, const utils::safe_handle_t& handle) override;

private:
  std::vector<uint8_t> ttf_bytes;
  std::unique_ptr<font_t> font_ptr;
};

}
}

#endif
