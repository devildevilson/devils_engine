#version 450

// Алгоритм: запись тонкого G-buffer без цвета материала. Мировая нормаль упаковывается октаэдрально в два
// канала, а motion vector строится как previous_uv - current_uv, чтобы потребитель мог прибавить его к
// текущему uv и найти историю. Из обеих проекций вычитается их jitter: статичная сцена должна иметь нулевое
// геометрическое движение, иначе TAA начал бы компенсировать собственную последовательность выборок.

#include "pf03_frame.glsl"

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec4 previous_clip;

// Порядок location = порядок ресурсов в render target 'gbuffer_target'
layout(location = 0) out vec2 out_normal;
layout(location = 1) out vec2 out_motion;

void main() {
  out_normal = pf03_encode_normal(normalize(world_normal));

  // Текущий uv берём из gl_FragCoord, а не из интерполированного клипа: это точный центр пикселя, и
  // ошибка репроекции тогда меряет motion, а не погрешность интерполяции.
  // G-buffer при TAAU живёт в разрешении РЕНДЕРА, а viewport_near.xy хранит размер ДИСПЛЕЯ. Деление на
  // display extent сжимало current_uv в угол (0..render_scale), тогда как previous_uv оставался 0..1, —
  // motion становился неверным почти во всём кадре. blur_params.yz несёт честный render extent.
  const vec2 current_uv = gl_FragCoord.xy / frame.blur_params.yz;
  const vec2 previous_uv = (previous_clip.xy / previous_clip.w) * 0.5 + 0.5;

  // Джиттер ОБОИХ кадров вычитается: обе позиции получены из джиттеренных проекций, а motion обязан быть
  // чисто геометрическим. Иначе субпиксельное дрожание протекает в вектор, TAA репроецирует по нему и
  // начинает бороться с собственным джиттером — картинка мылится, а сглаживания не появляется.
  const vec2 current_unjittered = current_uv - frame.taa_jitter.xy;
  const vec2 previous_unjittered = previous_uv - frame.taa_jitter.zw;

  // Договор: motion — это СМЕЩЕНИЕ К прошлому кадру, поэтому читатель делает uv + motion
  out_motion = previous_unjittered - current_unjittered;
}
