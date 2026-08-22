#version 450

// Алгоритм: fullscreen triangle проходит только где (stencil & 0x30) == 0x30 — это результат back-face
// операции invert. Полупрозрачный жёлтый прямоугольник визуально отделяет back state от front state.

layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(1.0, 0.62, 0.06, 0.78);
}
