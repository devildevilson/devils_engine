#version 450

// Фрагментный шейдер интерфейса (visage/Nuklear). Один пайплайн рисует разное (mode в push):
//   0 SOLID — фигуры nuklear (фон/рамки/кнопки): чистый цвет, без сэмпла;
//   1 MSDF  — текст из mtsdf-атласа: медиана RGB как signed distance + сглаживание; плюс SDF-эффекты
//             (boldness/контур/softness) из push-константы (запекаются в visage::convert по userdata);
//   2 IMAGE — nk_image: сэмпл текстуры * цвет.
// tex_id/mode/эффекты приходят per-draw-command из draw_ui (push-константа, см. ui_push_t / ui_push).

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 frag_color;

// дескриптор 'textures' (set1, binding0): массив combinedImageSampler (атлас + картинки)
layout(set = 1, binding = 0) uniform sampler2D tex[8];

layout(push_constant) uniform ui_pc_block {
  uint tex_id;          // индекс в массиве textures
  uint mode;            // 0 solid, 1 msdf, 2 image
  float boldness;       // сдвиг порога: >0 жирнее, <0 тоньше
  float outline_width;  // ширина контура в SDF-единицах (0 = нет)
  uint outline_color;   // R8G8B8A8
  float softness;       // размытие края (0 = резко)
} pc;

// pixel_range, с которым генерился атлас (font_atlas_packer::config.pixel_range = 2.0)
const float px_range = 2.0;

float median(float r, float g, float b) { return max(min(r, g), min(max(r, g), b)); }

// distance-range атласа -> экранные пиксели через производные uv (классика MSDF, Chlumsky)
float screen_px_range() {
  const vec2 unit_range = vec2(px_range) / vec2(textureSize(tex[pc.tex_id], 0));
  const vec2 screen_tex_size = vec2(1.0) / fwidth(uv);
  return max(0.5 * dot(unit_range, screen_tex_size), 1.0);
}

void main() {
  if (pc.mode == 1u) {
    const vec4 t = texture(tex[pc.tex_id], uv);
    const float sd = median(t.r, t.g, t.b); // MSDF (rgb): резкие углы — для заливки
    // softness снижает резкость края (размытие); boldness сдвигает порог заливки
    float spx = screen_px_range() / (1.0 + pc.softness * 4.0);
    const float fill_thresh = 0.5 - pc.boldness;
    const float fill = clamp(spx * (sd - fill_thresh) + 0.5, 0.0, 1.0);

    if (pc.outline_width > 0.0) {
      // контур считаем по a-каналу mtsdf (истинный SDF — гладкий, без шума вдали от края).
      // outer = заливка+контур; внутри заливки цвет текста, в полосе — цвет контура; альфа = outer.
      const float td = t.a;
      const float outer = clamp(spx * (td - (fill_thresh - pc.outline_width)) + 0.5, 0.0, 1.0);
      const vec4 oc = unpackUnorm4x8(pc.outline_color);
      frag_color = vec4(mix(oc.rgb, color.rgb, fill), max(fill * color.a, outer * oc.a));
    } else {
      frag_color = vec4(color.rgb, color.a * fill);
    }
  } else if (pc.mode == 2u) {
    frag_color = texture(tex[pc.tex_id], uv) * color;
  } else {
    frag_color = color; // SOLID
  }
}
