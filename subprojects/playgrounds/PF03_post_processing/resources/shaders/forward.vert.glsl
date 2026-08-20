#version 450

#include "pf03_frame.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
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

layout(location = 0) out vec3 world_normal;
layout(location = 1) out vec3 world_position;

vec3 rotate_y(const vec3 p, const float angle) {
  const float s = sin(angle);
  const float c = cos(angle);
  return vec3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

// Forward-ветвь: истории объекта здесь НЕ нужно. Прошлый трансформ существует ради motion-векторов, а векторы
// нужны репроекции и motion blur — то есть ровно тем техникам, которые в MSAA-ветви не живут. Отсюда и один
// биндинг вместо двух: развилка видна уже в вершинном шейдере.
void main() {
  const int index = int(in_instance.x + 0.5);
  const vec4 current = objects.items[index].offset;

  world_position = rotate_y(in_position, current.w) + current.xyz;
  world_normal = rotate_y(in_normal, current.w);
  gl_Position = frame.view_projection * vec4(world_position, 1.0);
}
