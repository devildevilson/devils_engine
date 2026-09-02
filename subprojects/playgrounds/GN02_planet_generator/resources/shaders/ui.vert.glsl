#version 450
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
// Блок камеры повторяется здесь ЦЕЛИКОМ и байт в байт, включая то, чем оверлей не пользуется. Пропуск
// одного поля не даёт ни ошибки компиляции, ни предупреждения: все следующие поля просто читаются с
// чужого смещения. Именно так оверлей и исчез, когда в блок добавилась обратная матрица — интерфейс
// брал размер окна из места, где лежат параметры отрисовки, и уезжал за экран целиком.
layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;
  vec4 viewport_near;
} camera;
void main() {
  const vec2 clip = in_position / camera.viewport_near.xy * 2.0 - 1.0;
  gl_Position = vec4(clip, 0.0, 1.0);
  out_uv = in_uv;
  out_color = in_color;
}
