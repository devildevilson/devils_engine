#version 450

// Алгоритм: один procedural triangle покрывает viewport без vertex buffer. Координаты выходят за обычный NDC
// квадрат намеренно: после clipping получается ровно fullscreen triangle без диагонального seam двух quad-triangles.

void main() {
  const vec2 position = vec2(
    float((gl_VertexIndex << 1) & 2),
    float(gl_VertexIndex & 2));
  gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
