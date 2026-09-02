#version 450

// Вершина поверхности приходит НЕ через вершинный буфер, а из storage-буфера по gl_VertexIndex.
// Причина простая: сетка процедурная, у неё нет ни атрибутов, ни индексов, а треугольники уже
// развёрнуты по три вершины на CPU — общие вершины не нужны, потому что нормаль это направление.
layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;        // масштаб рельефа, плоская закраска, радиус, режим
  vec4 viewport_near;
} camera;

layout(set = 0, binding = 1, std430) readonly buffer SurfaceVertices { vec4 vertices[]; } surface;
layout(set = 0, binding = 2, std430) readonly buffer CellVisuals { vec4 cells[]; } visuals;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_colour;
layout(location = 2) flat out vec3 out_flat_colour;
layout(location = 3) out float out_height;

void main() {
  const vec4 vertex = surface.vertices[gl_VertexIndex];
  const vec3 direction = normalize(vertex.xyz);
  // Индекс клетки лежит в четвёртой компоненте битами целого: вершина ровно шестнадцать байт, и
  // отдельный буфер индексов ради одного числа заводить незачем.
  const uint cell = floatBitsToUint(vertex.w);
  const vec4 visual = visuals.cells[cell];

  const float radius = camera.params.z + camera.params.x * visual.w;
  const vec3 local = direction * radius;
  gl_Position = camera.view_projection * camera.planet_to_world * vec4(local, 1.0);

  out_normal = normalize((camera.planet_to_world * vec4(direction, 0.0)).xyz);
  out_colour = visual.rgb;
  out_flat_colour = visual.rgb;
  out_height = visual.w;
}
