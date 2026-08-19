#version 450

#include "pf03_frame.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
// Инстанс-атрибуты идут по location сразу за вершинными. Здесь это только индекс объекта: сами трансформы
// приходят из per_frame ресурса, потому что меняются каждый кадр.
layout(location = 2) in vec4 in_instance;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

struct ObjectTransform {
  vec4 offset; // xyz — смещение, w — угол поворота вокруг Y
  vec4 reserved;
};

layout(set = 1, binding = 0, std430) readonly buffer ObjectBuffer {
  ObjectTransform items[];
} objects;

// Прошлый кадр приходит ОТДЕЛЬНЫМ биндингом того же ресурса ('history = 1'): хост пишет только текущий
// трансформ, а прошлый движок отдаёт сам. Дублировать его в записи инстанса не нужно.
layout(set = 1, binding = 1, std430) readonly buffer ObjectHistoryBuffer {
  ObjectTransform items[];
} objects_previous;

layout(location = 0) out vec3 world_normal;
// Позиция ТОЙ ЖЕ точки поверхности в клипе прошлого кадра. Два источника движения складываются здесь и
// только здесь: прошлый трансформ объекта и прошлая view-projection камеры.
layout(location = 1) out vec4 previous_clip;

vec3 rotate_y(const vec3 p, const float angle) {
  const float s = sin(angle);
  const float c = cos(angle);
  return vec3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

void main() {
  const int index = int(in_instance.x + 0.5);
  const vec4 current = objects.items[index].offset;
  // На первом кадре после сброса копия истории пуста (движок чистит её в нули), поэтому берём текущий
  // трансформ: «истории нет» должно читаться как отсутствие движения, а не как прыжок из начала координат.
  const bool history_valid = frame.viewport_near.w > 1.5;
  const vec4 previous = history_valid ? objects_previous.items[index].offset : current;

  const vec3 world_position = rotate_y(in_position, current.w) + current.xyz;
  const vec3 previous_position = rotate_y(in_position, previous.w) + previous.xyz;

  world_normal = rotate_y(in_normal, current.w);
  previous_clip = frame.previous_view_projection * vec4(previous_position, 1.0);
  gl_Position = frame.view_projection * vec4(world_position, 1.0);
}
