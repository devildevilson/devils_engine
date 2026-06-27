#version 450

// Фрагментный шейдер интерфейса (visage/Nuklear). Один UI рисует РАЗНЫЕ вещи одним пайплайном:
//   mode=0 SOLID  — фигуры Nuklear (фон окна, рамки, кнопки): null-текстура, чистый цвет;
//   mode=1 MSDF   — текст из атласа шрифта (mtsdf): медиана RGB как signed distance, сглаживание
//                   через экранные производные (screenPxRange);
//   mode=2 IMAGE  — обычная картинка (nk_image): сэмпл текстуры * цвет.
// tex_id — индекс в дескрипторном массиве 'textures' (для MSDF/IMAGE). И tex_id, и mode приходят
// per-draw-command из шага draw_ui (push-константа). Точный layout финализируется в шаге 4.

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 frag_color;

// дескриптор 'textures' (set=0, binding=0): тот же массив combinedImageSampler, что у tile-шейдера
layout(set = 0, binding = 0) uniform sampler2D tex[8];

layout(push_constant) uniform pc {
  int tex_id;   // индекс текстуры/атласа в массиве
  int mode;     // 0 solid, 1 msdf-текст, 2 image
} pc;

// pixel_range, с которым генерился атлас (font_atlas_packer::config.pixel_range = 2.0).
// Позже можно вынести в общий UBO; пока константа, совпадающая с генерацией.
const float px_range = 2.0;

float median(float r, float g, float b) { return max(min(r, g), min(max(r, g), b)); }

// distance-range атласа -> экранные пиксели через производные uv (классика MSDF, Chlumsky)
float screen_px_range() {
  const vec2 unit_range = vec2(px_range) / vec2(textureSize(tex[pc.tex_id], 0));
  const vec2 screen_tex_size = vec2(1.0) / fwidth(uv);
  return max(0.5 * dot(unit_range, screen_tex_size), 1.0);
}

void main() {
  if (pc.mode == 1) {
    // mtsdf: rgb = msdf (резкие углы), a = истинный SDF. Берём медиану rgb.
    const vec4 s = texture(tex[pc.tex_id], uv);
    const float sd = median(s.r, s.g, s.b);
    const float screen_px = screen_px_range();
    const float alpha = clamp(screen_px * (sd - 0.5) + 0.5, 0.0, 1.0);
    frag_color = vec4(color.rgb, color.a * alpha);
  } else if (pc.mode == 2) {
    frag_color = texture(tex[pc.tex_id], uv) * color;
  } else {
    frag_color = color; // SOLID — фигуры Nuklear
  }
}
