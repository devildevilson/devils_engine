#version 450

// Алгоритм: один procedural fullscreen triangle покрывает viewport без vertex buffer и без диагонального seam.
// Его vertex coordinates и UV намеренно идут от 0 до 2: clip-space triangle имеет углы (-1,-1), (3,-1),
// (-1,3), а после интерполяции видимая часть viewport получает UV 0..1. Дополнительный scale UV здесь ошибочен:
// он выбрал бы только левую нижнюю четверть texture и растянул её на весь экран.

layout(location = 0) out vec2 out_uv;

void main() {
  const vec2 position = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  out_uv = position;
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
