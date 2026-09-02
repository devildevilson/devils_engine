#version 450
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

// Оверлей берёт из блока камеры только размер окна, но включает его ЦЕЛИКОМ: раскладка в std140
// читается по смещениям, и именно на этом оверлей однажды исчез — он брал размер окна оттуда, где
// лежат параметры отрисовки. Теперь раскладка одна на все шейдеры и лежит в одном файле.
#include "camera_block.glsl"
void main() {
  const vec2 clip = in_position / camera.viewport_near.xy * 2.0 - 1.0;
  gl_Position = vec4(clip, 0.0, 1.0);
  out_uv = in_uv;
  out_color = in_color;
}
