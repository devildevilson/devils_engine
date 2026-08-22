#version 450

// Алгоритм: rasterizer строит экранную проекцию выбранной mesh, fragment выдаёт {coverage, device depth},
// а fixed-function MAX blending оставляет на каждом texel максимальную reverse-Z depth — ближайшую поверхность.
// При linear sampling обе компоненты интерполируются от нуля, поэтому outline делит depth на coverage.

layout(location = 0) out vec2 out_mask;

void main() {
  out_mask = vec2(1.0, gl_FragCoord.z);
}
