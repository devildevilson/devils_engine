#version 450

// Мировой интерфейс: надпись привязана к точке В МИРЕ, а раскладка задана В ПИКСЕЛЯХ. Иначе не выходит
// ни то, ни другое: чисто мировой прямоугольник растягивается перспективой и наклоном камеры и перестаёт
// быть читаемым текстом, а чисто экранный отрывается от персонажа при повороте.
//
// Поэтому якорь проецируется обычной матрицей камеры, а смещение прибавляется к clip.xy с множителем
// clip.w — так пиксельный сдвиг остаётся пиксельным на любой глубине, а сама надпись сохраняет честную
// глубину якоря. Порог видимости считает CPU: он знает ширину обзора, шейдер про зум не знает ничего.

struct label_vertex {
  vec3 anchor;      // точка в мире, к которой прицеплена надпись
  uint tint;        // упакованный RGBA
  vec2 offset_px;   // смещение от якоря в пикселях экрана, y растёт вниз
  vec2 uv;          // атлас MSDF; отрицательный u означает сплошной прямоугольник
};

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;   // xy — размер вьюпорта в пикселях, w — слот атласа шрифта
} camera_data;

layout(set = 0, binding = 1, std430) readonly buffer LabelStream {
  label_vertex vertices[];
} stream;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) flat out uint out_font;

vec4 unpack_tint(const uint value) {
  return vec4(float((value >> 0) & 0xffu), float((value >> 8) & 0xffu), float((value >> 16) & 0xffu),
              float((value >> 24) & 0xffu)) / 255.0;
}

void main() {
  const label_vertex vertex = stream.vertices[gl_VertexIndex];
  const vec4 clip = camera_data.view_projection * vec4(vertex.anchor, 1.0);

  if (clip.w <= 0.0) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // якорь за камерой: надпись выкидывается целиком
  } else {
    const vec2 offset = vertex.offset_px / camera_data.viewport_near.xy * 2.0 * clip.w;
    gl_Position = vec4(clip.xy + offset, clip.z, clip.w);
  }

  out_uv = vertex.uv;
  out_color = unpack_tint(vertex.tint);
  out_font = uint(camera_data.viewport_near.w);
}
