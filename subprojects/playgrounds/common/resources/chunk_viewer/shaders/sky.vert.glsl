#version 450

// Небо — один треугольник, накрывающий экран: ни вершинного буфера, ни геометрии, три вершины
// строятся из gl_VertexIndex. Треугольник, а не два, потому что по диагонали квадрата проходит шов,
// на котором интерполяция считается дважды.
layout(location = 0) out vec2 out_ndc;

void main() {
  const vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  out_ndc = corner * 2.0 - 1.0;
  gl_Position = vec4(out_ndc, 0.0, 1.0);
}
