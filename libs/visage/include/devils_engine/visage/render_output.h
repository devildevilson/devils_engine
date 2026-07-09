#ifndef DEVILS_ENGINE_VISAGE_RENDER_OUTPUT_H
#define DEVILS_ENGINE_VISAGE_RENDER_OUTPUT_H

#include <cstddef>
#include <cstdint>
#include <devils_engine/utils/shared.h>

// Лёгкий контракт вывода интерфейса: POD-структуры без зависимостей от lua/nuklear/vulkan.
// Главный поток (visage::system) производит эти данные, рендер-поток (шаг draw_ui) их читает.
// Здесь намеренно нет тяжёлых include, чтобы хедер был дешёвым для обеих сторон.

namespace devils_engine {
namespace visage {

// Вершина UI. Layout жёстко зафиксирован под nk_convert (см. system::convert):
// pos (vec2 float) + uv (vec2 float) + цвет (R8G8B8A8, упакованный uint32).
struct gui_vertex_t {
  float pos[2];
  float uv[2];
  uint32_t color;
};

// Индексы Nuklear — 16-битные.
using gui_index_t = uint16_t;

// Режим отрисовки фрагмента UI. Вычисляется в convert по texture.id команды и совпадает с
// веткой в ui.frag: фигуры nuklear идут с null-текстурой (solid), текст — с текстурой атласа
// шрифта (msdf), всё прочее — обычная картинка.
namespace gui_draw_mode {
enum values : uint32_t {
  solid     = utils::shared::UI_DRAW_SOLID, // фигуры (фон окна, рамки, кнопки) — чистый цвет, без сэмпла
  msdf      = utils::shared::UI_DRAW_MSDF, // текст из mtsdf-атласа шрифта
  image     = utils::shared::UI_DRAW_IMAGE, // nk_image — сэмпл текстуры * цвет
  composite = utils::shared::UI_DRAW_COMPOSITE, // РЕЗЕРВ: составная картинка (геральдика — несколько слоёв); пока не реализовано
  cooldown  = utils::shared::UI_DRAW_COOLDOWN, // картинка проявляется по градиент-маске на долю fill (эффект перезарядки)
  mix       = utils::shared::UI_DRAW_MIX, // 4-blend: до 4 компонентов (картинки/цвета) по каналам маски
};
}

// Бит-кодировка id текстуры UI. Одно слово несёт ТИП (=gui_draw_mode), mirror по u/v и индекс ассета.
// Убирает эвристики в convert()/шейдере: тип читается из id напрямую. id уходит как nk_handle.id (int),
// поэтому бит 31 держим нулём. ВАЖНО: держи биты В СИНХРОНЕ с ui.frag (там продублированы те же маски).
//   [0..13] index (14 бит, 0..16383) | [14] mirror U | [15] mirror V | [16..19] type (4 бита) |
//   [20..23] sampler_id (4 бита, индекс в sampler-пуле) | [24..30] free (7 бит) | [31] не используем
namespace tex_id {
  inline constexpr uint32_t index_bits    = utils::shared::TEX_INDEX_BITS;
  inline constexpr uint32_t index_mask    = utils::shared::TEX_INDEX_MASK; // 0x3FFF
  inline constexpr uint32_t mirror_u_bit  = utils::shared::TEX_MIRROR_U;
  inline constexpr uint32_t mirror_v_bit  = utils::shared::TEX_MIRROR_V;
  inline constexpr uint32_t type_shift    = utils::shared::TEX_TYPE_SHIFT;
  inline constexpr uint32_t type_mask     = utils::shared::TEX_TYPE_MASK;  // 4 бита
  inline constexpr uint32_t sampler_shift = utils::shared::TEX_SAMPLER_SHIFT;
  inline constexpr uint32_t sampler_mask  = utils::shared::TEX_SAMPLER_MASK; // 4 бита (до 16 семплеров)

  inline uint32_t pack(const uint32_t type, const uint32_t index,
      const bool mirror_u = false, const bool mirror_v = false, const uint32_t sampler_id = 0) {
    return utils::shared::tex_pack(type, index, mirror_u, mirror_v, sampler_id);
  }
  inline uint32_t index_of(const uint32_t id)    { return utils::shared::tex_index_of(id); }
  inline uint32_t type_of(const uint32_t id)     { return utils::shared::tex_type_of(id); }
  inline uint32_t sampler_of(const uint32_t id)  { return utils::shared::tex_sampler_of(id); }
  inline bool     mirror_u_of(const uint32_t id) { return utils::shared::tex_mirror_u_of(id); }
  inline bool     mirror_v_of(const uint32_t id) { return utils::shared::tex_mirror_v_of(id); }
}

// Одна draw-команда после nk_convert: сколько идущих подряд индексов рисовать, прямоугольник
// отсечения (scissor, в пикселях окна), индекс текстуры, режим (см. gui_draw_mode) и transform_id.
// Это валюта шага draw_ui: рендер пробежит по массиву таких команд (setScissor + push + drawIndexed).
// Буфер команд самоописывающийся: впереди uint32 count, затем массив.
//
// SDF-эффекты (boldness/outline/softness) запекаются в команду на стороне visage (convert) из
// effect-арены по nk userdata команды и едут в шейдер per-draw push-константой. Хранилища сырых
// data в GPU нет (см. visage-ui-roadmap) — пока всё через push-константу.
// texture_id УПАКОВАН (tex_id): тип+mirror+индекс — шейдер декодит сам. payload — обобщённая нагрузка
// per-draw, интерпретируется ПО ТИПУ (держи в синхроне с ui.frag):
//   msdf(1):     [0]=boldness [1]=outline_width [2]=outline_color(RGBA8) [3]=softness  (float↔uint через bitcast)
//   cooldown(4): [0]=mask_index [1]=fill(float bits)
//   mix(5):      [0]=mask_index [1..4]=comp0..3 (слот если image, иначе RGBA8) [5]=is_image (биты 0..3)
//   image(2)/solid(0): игнорируется
struct gui_draw_command_t {
  uint32_t elem_count;
  float clip_x, clip_y, clip_w, clip_h;
  uint32_t texture_id;
  uint32_t payload[6];
};

}
}

#endif
