#version 450

// Фрагментный шейдер интерфейса (visage/Nuklear). Один пайплайн рисует разное; ТИП берётся из
// упакованного tex_id (см. tex_id в render_output.h), payload[6] — нагрузка per-draw по типу:
//   0 SOLID — фигуры nuklear: чистый цвет, без сэмпла;
//   1 MSDF  — текст из mtsdf-атласа + SDF-эффекты (boldness/контур/softness) из payload;
//   2 IMAGE — nk_image: сэмпл текстуры * цвет (+ mirror по u/v);
//   4 COOLDOWN — картинка проявляется по градиент-маске на долю fill (незаполненное затемнено);
//   5 MIX   — 4-blend: до 4 компонентов (картинки/цвета) по каналам маски.
// tex_id/payload приходят per-draw-command из draw_ui (push-константа, см. ui_push_t / ui_push).

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 frag_color;

// дескриптор 'textures' (set1, binding0): массив combinedImageSampler (атлас + картинки).
// 16 = гарантированный минимум Vulkan (maxPerStageDescriptorSampledImages>=16). Больше — в пассе
// огромных дескрипторов (descriptor_indexing/nonuniform).
layout(set = 1, binding = 0) uniform sampler2D tex[16];

layout(push_constant) uniform ui_pc_block {
  uint tex_id;       // УПАКОВАН: [0..13] индекс | [14] mirror U | [15] mirror V | [16..19] тип
  uint payload[6];   // нагрузка по типу (см. ниже / render_output.h)
} pc;

// Раскладка tex_id — держи В СИНХРОНЕ с tex_id в render_output.h (visage).
const uint TEX_INDEX_MASK = 0x3FFFu; // 14 бит
const uint TEX_MIRROR_U   = 0x4000u; // бит 14
const uint TEX_MIRROR_V   = 0x8000u; // бит 15
const uint TEX_TYPE_SHIFT = 16u;
const uint TEX_TYPE_MASK  = 0xFu;

// pixel_range, с которым генерился атлас (font_atlas_packer::config.pixel_range)
const float px_range = 4.0;

float median(float r, float g, float b) { return max(min(r, g), min(max(r, g), b)); }

// distance-range атласа -> экранные пиксели через производные uv (классика MSDF, Chlumsky)
float screen_px_range(uint index) {
  const vec2 unit_range = vec2(px_range) / vec2(textureSize(tex[index], 0));
  const vec2 screen_tex_size = vec2(1.0) / fwidth(uv);
  return max(0.5 * dot(unit_range, screen_tex_size), 1.0);
}

// компонент mix: картинка (сэмпл слота) или цвет (RGBA8). raw = слот ИЛИ packed color.
vec4 mix_comp(uint raw, bool is_image, vec2 s) {
  if (is_image) return texture(tex[clamp(raw & TEX_INDEX_MASK, 0u, 15u)], s);
  return unpackUnorm4x8(raw);
}

void main() {
  // декод упакованного tex_id: индекс/тип/mirror
  const uint index = clamp(pc.tex_id & TEX_INDEX_MASK, 0u, 15u);
  const uint mode  = (pc.tex_id >> TEX_TYPE_SHIFT) & TEX_TYPE_MASK;

  // mirror-флип uv (корректно для картинки во весь регион; суб-регион mirror — позже)
  vec2 s = uv;
  if ((pc.tex_id & TEX_MIRROR_U) != 0u) s.x = 1.0 - s.x;
  if ((pc.tex_id & TEX_MIRROR_V) != 0u) s.y = 1.0 - s.y;

  if (mode == 1u) {
    // MSDF: payload = boldness / outline_width / outline_color / softness
    const float boldness      = uintBitsToFloat(pc.payload[0]);
    const float outline_width = uintBitsToFloat(pc.payload[1]);
    const uint  outline_color = pc.payload[2];
    const float softness      = uintBitsToFloat(pc.payload[3]);

    const vec4 t = texture(tex[index], uv);
    const float sd = median(t.r, t.g, t.b);
    float spx = screen_px_range(index) / (1.0 + softness * 4.0);
    const float fill_thresh = 0.5 - boldness;
    const float fill = clamp(spx * (sd - fill_thresh) + 0.5, 0.0, 1.0);

    if (outline_width > 0.0) {
      const float td = t.a;
      const float outer = clamp(spx * (td - (fill_thresh - outline_width)) + 0.5, 0.0, 1.0);
      const vec4 oc = unpackUnorm4x8(outline_color);
      frag_color = vec4(mix(oc.rgb, color.rgb, fill), max(fill * color.a, outer * oc.a));
    } else {
      frag_color = vec4(color.rgb, color.a * fill);
    }
  } else if (mode == 2u) {
    frag_color = texture(tex[index], s) * color;
  } else if (mode == 4u) {
    // COOLDOWN: main = tex[index], mask.r <= fill => проявлено; иначе затемнено
    const uint  mask_index = pc.payload[0] & TEX_INDEX_MASK;
    const float fill       = uintBitsToFloat(pc.payload[1]);
    const float m   = texture(tex[clamp(mask_index, 0u, 15u)], s).r;
    const float rev = step(m, fill);
    const vec4  c   = texture(tex[index], s);
    frag_color = vec4(c.rgb * mix(0.35, 1.0, rev), c.a) * color;
  } else if (mode == 5u) {
    // MIX: веса из каналов маски (R/G/B = comp0..2, comp3 = 1-R-G-B), альфа маски = непрозрачность
    const uint mask_index = pc.payload[0] & TEX_INDEX_MASK;
    const uint is_img     = pc.payload[5];
    const vec4 mk = texture(tex[clamp(mask_index, 0u, 15u)], uv);
    const float w0 = mk.r, w1 = mk.g, w2 = mk.b;
    const float w3 = clamp(1.0 - (w0 + w1 + w2), 0.0, 1.0);
    const vec4 c0 = mix_comp(pc.payload[1], (is_img & 1u) != 0u, uv);
    const vec4 c1 = mix_comp(pc.payload[2], (is_img & 2u) != 0u, uv);
    const vec4 c2 = mix_comp(pc.payload[3], (is_img & 4u) != 0u, uv);
    const vec4 c3 = mix_comp(pc.payload[4], (is_img & 8u) != 0u, uv);
    vec4 mixed = c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3;
    mixed.a *= mk.a;
    frag_color = mixed * color;
  } else {
    frag_color = color; // SOLID (mode 0)
  }
}
