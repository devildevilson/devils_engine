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
  solid = 0, // фигуры (фон окна, рамки, кнопки) — чистый цвет, без сэмпла
  msdf  = 1, // текст из mtsdf-атласа шрифта
  image = 2, // nk_image — сэмпл текстуры * цвет
};
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
  uint32_t texture_id;
  uint32_t mode;            // gui_draw_mode::values
  // SDF-эффекты текста (mode=msdf); для прочих режимов игнорируются
  float boldness;           // сдвиг порога: >0 жирнее, <0 тоньше
  float outline_width;      // ширина контура в SDF-единицах (0 = нет контура)
  uint32_t outline_color;   // R8G8B8A8
  float softness;           // размытие края (0 = резко)
};

}
}

#endif
