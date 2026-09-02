#version 450

// Подпись места на планете.
//
// Не декаль по рельефу, как в PF10, а ЩИТОК, привязанный к направлению: подпись стоит над центром
// области, вращается вместе с планетой, прячется за горизонт — и при этом всегда читается, потому что
// её высота задана в долях экрана, а не в радианах поверхности. Для карты это правильный выбор: у
// названия места на глобусе нет своего наклона, у него есть только место.
layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;
  vec4 viewport_near;
} camera;

// Глиф: якорь с высотой, прямоугольник в долях высоты, координаты в атласе и цвет с номером текстуры.
struct label_glyph {
  vec4 anchor;
  vec4 rect;
  vec4 uv;
  vec4 tint;
};
layout(set = 0, binding = 1, std430) readonly buffer LabelGlyphs { label_glyph glyphs[]; } labels;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_tint;

void main() {
  const uint glyph_index = uint(gl_VertexIndex) / 6u;
  const uint corner_index = uint(gl_VertexIndex) % 6u;
  // Шесть вершин на глиф без буфера индексов: индексы ради двух треугольников — это второй буфер,
  // который надо держать согласованным с первым.
  const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                  vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
  const vec2 corner = corners[corner_index];

  const label_glyph glyph = labels.glyphs[glyph_index];
  const vec3 world_direction = normalize((camera.planet_to_world * vec4(glyph.anchor.xyz, 0.0)).xyz);
  const vec3 to_camera = normalize(camera.camera_position.xyz);

  // ГОРИЗОНТ. Порог не нулевой: у самого края диска подпись лежит почти вдоль взгляда, растягивается
  // и читается как артефакт. Ноль двадцать пять — примерно 75 градусов от центра диска.
  if (dot(world_direction, to_camera) < 0.25) {
    gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
    out_uv = vec2(0.0);
    out_tint = vec4(0.0);
    return;
  }

  const vec4 clip = camera.view_projection * camera.planet_to_world *
                    vec4(glyph.anchor.xyz * camera.params.z, 1.0);
  if (clip.w <= 0.0) {
    gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
    out_uv = vec2(0.0);
    out_tint = vec4(0.0);
    return;
  }

  const vec2 ndc = clip.xy / clip.w;
  const float aspect = max(camera.viewport_near.x, 1.0) / max(camera.viewport_near.y, 1.0);
  const vec2 offset = (glyph.rect.xy + corner * glyph.rect.zw) * glyph.anchor.w;
  // Поправка на соотношение сторон идёт по X: высота задана в долях экранной высоты, и без поправки
  // буква растягивалась бы вместе с окном.
  gl_Position = vec4(ndc + vec2(offset.x / aspect, offset.y), 0.5, 1.0);

  out_uv = mix(glyph.uv.xy, glyph.uv.zw, vec2(corner.x, 1.0 - corner.y));
  out_tint = glyph.tint;
}
