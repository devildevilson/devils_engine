#version 450

layout(set = 0, binding = 0, std140) uniform Camera { mat4 view_projection; } camera;

struct Vertex { vec4 position; vec4 color; };
layout(set = 0, binding = 1, std430) readonly buffer Scene { Vertex vertices[]; } scene;

layout(location = 0) out vec3 world_position;
layout(location = 1) out vec4 vertex_color;

void main() {
  Vertex vertex = scene.vertices[gl_VertexIndex];
  world_position = vertex.position.xyz;
  vertex_color = vertex.color;
  gl_Position = camera.view_projection * vertex.position;
}
