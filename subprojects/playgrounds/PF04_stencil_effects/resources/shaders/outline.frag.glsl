#version 450

// Алгоритм outline целиком находится в geometry expansion + stencil/depth state vertex-прохода; fragment
// stage только задаёт хорошо различимый цвет оставшегося силуэтного кольца.

layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(1.0, 0.42, 0.06, 1.0);
}
