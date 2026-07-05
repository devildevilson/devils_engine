#version 450

// Фрагментный шейдер интерфейса (visage/Nuklear). ТИП + sampler_id берутся из упакованного tex_id
// (см. tex_id в render_output.h), payload[6] — нагрузка per-draw по типу:
//   0 SOLID · 1 MSDF (+SDF-эффекты) · 2 IMAGE (+mirror) · 4 COOLDOWN (проявление по маске+fill) ·
//   5 MIX (4-blend по каналам маски).
// asset-текстуры — bindless v2: раздельные sampled-image массив + sampler-пул; пара sampler2D(tex,samp)
// собирается на месте. sampler_id из id выбирает семплер (0=linear/1=nearest); маски эффектов задают
// семплер по смыслу (mix-маска nearest, cooldown-градиент linear).

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 frag_color;

layout(set = 1, binding = 0) uniform texture2D tex[16];  // asset-картинки (пишутся рендером)
layout(set = 1, binding = 1) uniform sampler   samp[2];  // immutable пул: 0=linear, 1=nearest

layout(push_constant) uniform ui_pc_block {
  uint tex_id;       // [0..13] индекс | [14/15] mirror u/v | [16..19] тип | [20..23] sampler_id
  uint payload[6];   // нагрузка по типу (см. render_output.h)
} pc;

// Раскладка tex_id — держи В СИНХРОНЕ с tex_id в render_output.h.
const uint TEX_INDEX_MASK   = 0x3FFFu;
const uint TEX_MIRROR_U     = 0x4000u;
const uint TEX_MIRROR_V     = 0x8000u;
const uint TEX_TYPE_SHIFT   = 16u;
const uint TEX_TYPE_MASK    = 0xFu;
const uint TEX_SAMPLER_SHIFT = 20u;
const uint TEX_SAMPLER_MASK  = 0xFu;
const uint SAMPLER_LINEAR   = 0u;
const uint SAMPLER_NEAREST  = 1u;

const float px_range = 4.0; // font_atlas_packer::config.pixel_range

float median(float r, float g, float b) { return max(min(r, g), min(max(r, g), b)); }

// combined-семплер из раздельных image+sampler (bindless v2). МАКРОС: sampler2D в SPIR-V нельзя ни
// возвращать из функции, ни хранить в локальной переменной — только строить на месте выражением.
#define S(idx, sid) sampler2D(tex[clamp((idx), 0u, 15u)], samp[clamp((sid), 0u, 1u)])

// sampler2D-конструктор из раздельных допустим ТОЛЬКО в точке использования (в textureSize),
// поэтому берём index/sid, а не готовый sampler2D (его нельзя передать аргументом).
float screen_px_range(uint index, uint sid) {
  const vec2 unit_range = vec2(px_range) / vec2(textureSize(S(index, sid), 0));
  const vec2 screen_tex_size = vec2(1.0) / fwidth(uv);
  return max(0.5 * dot(unit_range, screen_tex_size), 1.0);
}

// компонент mix: картинка (сэмпл слота через samp[sid]) или цвет (RGBA8)
vec4 mix_comp(uint raw, bool is_image, vec2 s, uint sid) {
  if (is_image) return texture(S(raw & TEX_INDEX_MASK, sid), s);
  return unpackUnorm4x8(raw);
}

void main() {
  const uint index = pc.tex_id & TEX_INDEX_MASK;
  const uint mode  = (pc.tex_id >> TEX_TYPE_SHIFT) & TEX_TYPE_MASK;
  const uint sid   = (pc.tex_id >> TEX_SAMPLER_SHIFT) & TEX_SAMPLER_MASK;

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

    const vec4 t = texture(S(index, sid), uv);
    const float sd = median(t.r, t.g, t.b);
    float spx = screen_px_range(index, sid) / (1.0 + softness * 4.0);
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
    frag_color = texture(S(index, sid), s) * color;
  } else if (mode == 4u) {
    // COOLDOWN: main = tex[index], mask.r <= fill => проявлено; иначе затемнено. Градиент-маска — linear.
    const uint  mask_index = pc.payload[0] & TEX_INDEX_MASK;
    const float fill       = uintBitsToFloat(pc.payload[1]);
    const float m   = texture(S(mask_index, SAMPLER_LINEAR), s).r;
    const float rev = step(m, fill);
    const vec4  c   = texture(S(index, sid), s);
    frag_color = vec4(c.rgb * mix(0.35, 1.0, rev), c.a) * color;
  } else if (mode == 5u) {
    // MIX: веса из каналов маски (R/G/B = comp0..2, comp3 = 1-R-G-B), альфа = непрозрачность.
    // Region-маска — NEAREST (резкие зоны без бида); компоненты-картинки — по sampler_id (обычно linear).
    const uint mask_index = pc.payload[0] & TEX_INDEX_MASK;
    const uint is_img     = pc.payload[5];
    const vec4 mk = texture(S(mask_index, SAMPLER_NEAREST), uv);
    const float w0 = mk.r, w1 = mk.g, w2 = mk.b;
    const float w3 = clamp(1.0 - (w0 + w1 + w2), 0.0, 1.0);
    const vec4 c0 = mix_comp(pc.payload[1], (is_img & 1u) != 0u, uv, sid);
    const vec4 c1 = mix_comp(pc.payload[2], (is_img & 2u) != 0u, uv, sid);
    const vec4 c2 = mix_comp(pc.payload[3], (is_img & 4u) != 0u, uv, sid);
    const vec4 c3 = mix_comp(pc.payload[4], (is_img & 8u) != 0u, uv, sid);
    vec4 mixed = c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3;
    mixed.a *= mk.a;
    frag_color = mixed * color;
  } else {
    frag_color = color; // SOLID (mode 0)
  }
}
