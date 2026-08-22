#version 450

// Алгоритм: fullscreen triangle обрабатывается только где (stencil & 0x04) == 0x04. Там он задаёт фон
// второго вида и пишет reverse-Z far depth 0, превращая участок общего depth attachment в чистую локальную
// глубину. После этого alternate-view geometry может делать обычный depth test, не сравниваясь с первым видом.

layout(location = 0) out vec4 out_color;

void main() {
  gl_FragDepth = 0.0;
  out_color = vec4(0.025, 0.045, 0.09, 1.0);
}
