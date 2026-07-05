#ifndef DEVILS_ENGINE_VISAGE_RENDER_OUTPUT_H
#define DEVILS_ENGINE_VISAGE_RENDER_OUTPUT_H

#include <cstddef>
#include <cstdint>

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
  solid     = 0, // фигуры (фон окна, рамки, кнопки) — чистый цвет, без сэмпла
  msdf      = 1, // текст из mtsdf-атласа шрифта
  image     = 2, // nk_image — сэмпл текстуры * цвет
  composite = 3, // РЕЗЕРВ: составная картинка (геральдика — несколько слоёв); пока не реализовано
};
}

// Бит-кодировка id текстуры UI. Одно слово несёт ТИП (=gui_draw_mode), mirror по u/v и индекс ассета.
// Убирает эвристики в convert()/шейдере: тип читается из id напрямую. id уходит как nk_handle.id (int),
// поэтому бит 31 держим нулём. ВАЖНО: держи биты В СИНХРОНЕ с ui.frag (там продублированы те же маски).
//   [0..13] index (14 бит, 0..16383) | [14] mirror U | [15] mirror V | [16..19] type (4 бита) |
//   [20..30] free (11 бит — геральдика/палитра/эффект) | [31] не используем
namespace tex_id {
  inline constexpr uint32_t index_bits   = 14;
  inline constexpr uint32_t index_mask   = (1u << index_bits) - 1u; // 0x3FFF
  inline constexpr uint32_t mirror_u_bit = 1u << 14;
  inline constexpr uint32_t mirror_v_bit = 1u << 15;
  inline constexpr uint32_t type_shift   = 16;
  inline constexpr uint32_t type_mask    = 0xFu;                     // 4 бита

  inline constexpr uint32_t pack(const uint32_t type, const uint32_t index, const bool mirror_u = false, const bool mirror_v = false) {
    return (index & index_mask)
      | (mirror_u ? mirror_u_bit : 0u)
      | (mirror_v ? mirror_v_bit : 0u)
      | ((type & type_mask) << type_shift);
  }
  inline constexpr uint32_t index_of(const uint32_t id)  { return id & index_mask; }
  inline constexpr uint32_t type_of(const uint32_t id)   { return (id >> type_shift) & type_mask; }
  inline constexpr bool     mirror_u_of(const uint32_t id) { return (id & mirror_u_bit) != 0u; }
  inline constexpr bool     mirror_v_of(const uint32_t id) { return (id & mirror_v_bit) != 0u; }
}

// Одна draw-команда после nk_convert: сколько идущих подряд индексов рисовать, прямоугольник
// отсечения (scissor, в пикселях окна), индекс текстуры, режим (см. gui_draw_mode) и transform_id.
// Это валюта шага draw_ui: рендер пробежит по массиву таких команд (setScissor + push + drawIndexed).
// Буфер команд самоописывающийся: впереди uint32 count, затем массив.
//
// SDF-эффекты (boldness/outline/softness) запекаются в команду на стороне visage (convert) из
// effect-арены по nk userdata команды и едут в шейдер per-draw push-константой. Хранилища сырых
// data в GPU нет (см. visage-ui-roadmap) — пока всё через push-константу.
struct gui_draw_command_t {
  uint32_t elem_count;
  float clip_x, clip_y, clip_w, clip_h;
  uint32_t texture_id;      // УПАКОВАННЫЙ id (см. tex_id): тип+mirror+индекс. Шейдер декодит сам.
  // SDF-эффекты текста (type=msdf); для прочих типов игнорируются
  float boldness;           // сдвиг порога: >0 жирнее, <0 тоньше
  float outline_width;      // ширина контура в SDF-единицах (0 = нет контура)
  uint32_t outline_color;   // R8G8B8A8
  float softness;           // размытие края (0 = резко)
};

}
}

#endif
