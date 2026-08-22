#version 450

// Алгоритм: fullscreen triangle проходит только где (stencil & 0x30) == 0x10 — это результат front-face
// операции replace. Полупрозрачный зелёный прямоугольник делает эту область видимой без чтения stencil image.

layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(0.08, 0.92, 0.30, 0.78);
}
