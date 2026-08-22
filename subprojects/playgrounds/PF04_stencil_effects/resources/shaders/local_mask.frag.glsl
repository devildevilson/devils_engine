#version 450

// Алгоритм находится в fixed-function state: прошедший depth fragment заменяет только stencil bit 0x02.
// Color output формально объявлен для совместимости с render target, но step ставит color mask = none.

layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(0.0);
}
