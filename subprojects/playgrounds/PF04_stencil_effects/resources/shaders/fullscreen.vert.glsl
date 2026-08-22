#version 450

// Алгоритм: fullscreen triangle выводится из gl_VertexIndex без vertex buffer. Его вершины покрывают весь
// экран, но rasterizer создаст fragment только в viewport, после чего fixed-function stencil test материала
// оставит нужную маску.

void main() {
  const vec2 position = vec2(
    float((gl_VertexIndex << 1) & 2),
    float(gl_VertexIndex & 2));
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
