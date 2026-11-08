#version 450

// ОТЛАДОЧНАЯ ЛИНИЯ: вершина — позиция относительно чанка камеры и цвет RGBA8 в битах w. Процессор
// приводит мировые метры к чанку камеры сам, каждый кадр, как у сущностей: линий тысячи.

#include "camera_block.glsl"

layout(set = 0, binding = 1, std430) readonly buffer Lines { vec4 data[]; } lines;

layout(location = 0) out vec4 out_colour;

void main() {
  const vec4 vertex = lines.data[gl_VertexIndex];
  out_colour = unpackUnorm4x8(floatBitsToUint(vertex.w));
  // Дальние линии тусклее: связи ближе к камере читаются первыми, а дальние не сливаются в кашу.
  const float distance = length(vertex.xyz - camera.camera_position.xyz);
  out_colour.rgb *= mix(1.0, 0.3, smoothstep(40.0, 420.0, distance));
  gl_Position = camera.view_projection * vec4(vertex.xyz, 1.0);
}
