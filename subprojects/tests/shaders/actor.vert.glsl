#version 450

layout(location = 0) in vec3 in_vertex_pos;
layout(location = 1) in vec4 in_vertex_color;
layout(location = 2) in vec2 in_actor_center;
layout(location = 3) in uint in_tex;
layout(location = 4) in vec4 in_actor_color;
layout(location = 5) in float in_actor_size;

layout(location = 0) out vec4 out_color;
layout(location = 1) flat out uint out_tex;

layout(set = 0, binding = 0) uniform global_ubo {
  mat4 view_proj;
  mat4 ui_proj;
  vec4 misc;
  vec4 reserved[3];
} g[2];

out gl_PerVertex {
  vec4 gl_Position;
};

void main() {
  const vec2 world_pos = in_actor_center + in_vertex_pos.xy * in_actor_size;
  gl_Position = g[0].view_proj * vec4(world_pos, 0.0, 1.0);
  out_color = in_vertex_color * in_actor_color
    + vec4(float(in_tex & 1u) * 0.001)
    + vec4(in_actor_size * 0.001);
  out_tex = in_tex;
}
