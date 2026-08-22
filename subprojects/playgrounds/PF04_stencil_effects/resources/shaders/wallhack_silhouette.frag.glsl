#version 450

// Алгоритм: fullscreen triangle проходит fixed-function stencil test только где bit 0x40 был записан
// depth-fail проходом target mesh. Поэтому красный alpha blend покрывает только невидимую за
// ближней геометрией часть силуэта; видимая часть остаётся обычным scene shading.

layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(0.98, 0.08, 0.16, 0.82);
}
