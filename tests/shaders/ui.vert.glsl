#version 450

// Вершинный шейдер интерфейса (visage/Nuklear). Вершина = gui_vertex_t (см. render_output.h):
// pos (экранные пиксели), uv, цвет (R8G8B8A8 unorm -> vec4). Переводим пиксели окна в clip
// через ortho-проекцию из общего uniform-буфера.
//
// ВНИМАНИЕ: set/binding и состав push-константы ФИНАЛИЗИРУЮТСЯ вместе с UI-материалом и шагом
// draw_ui в render-config (шаг 4). Здесь — намеренный черновик контракта.

layout(location = 0) in vec2 in_pos;    // позиция в пикселях окна
layout(location = 1) in vec2 in_uv;     // uv в атласе (для текста) / null (для фигур)
layout(location = 2) in vec4 in_color;  // R8G8B8A8 unorm -> vec4

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

out gl_PerVertex { vec4 gl_Position; };

// Общий uniform-буфер: ortho-проекция UI (пиксели окна -> clip), а позже сюда же камера,
// размеры экрана, обратные матрицы и прочая мишура (один UBO на все шейдеры).
layout(set = 0, binding = 1, std140) uniform global_ubo {
  mat4 ui_proj;
} g;

void main() {
  gl_Position = g.ui_proj * vec4(in_pos, 0.0, 1.0);
  out_uv = in_uv;
  out_color = in_color;
}
