#version 450

// Вершинный шейдер интерфейса (visage/Nuklear). Вершина = gui_vertex_t (geometry "ui" = v2v2c4):
// pos (экранные пиксели), uv, цвет (R8G8B8A8 unorm -> vec4). Переводим пиксели окна в clip через
// ortho-проекцию из общего UBO.

layout(location = 0) in vec2 in_pos;    // позиция в пикселях окна
layout(location = 1) in vec2 in_uv;     // uv в атласе (текст) / произвольное (фигуры)
layout(location = 2) in vec4 in_color;  // R8G8B8A8 unorm -> vec4

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

out gl_PerVertex { vec4 gl_Position; };

// Общий UBO (set0, binding0). Массив копий шейдеру НЕ виден: биндинг без 'history' — одиночный,
// и всегда указывает на текущую копию ресурса (update_descriptors ротирует её каждый кадр).
// Массив появляется только у биндинга с 'history = N', и тогда [0] — текущий кадр, [1..N] — история.
layout(set = 0, binding = 0, std140) uniform global_ubo {
  mat4 view_proj;  // мир -> clip (камера)
  mat4 ui_proj;    // пиксели окна -> clip (UI)
  vec4 misc;       // x=screen_w, y=screen_h, z=sdf_px_range, w=reserved
} g;

void main() {
  gl_Position = g.ui_proj * vec4(in_pos, 0.0, 1.0);
  out_uv = in_uv;
  out_color = in_color;
}
