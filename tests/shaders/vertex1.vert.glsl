#version 450

const vec4 vertices[] = {
  vec4(-1,-1,0,1),
  vec4( 1,-1,0,1),
  vec4( 0, 1,0,1),
};

const vec4 colors[] = {
  vec4(1,0,0,1),
  vec4(0,1,0,1),
  vec4(0,0,1,1),
};

layout(location = 0) in  vec3 in_vertex_pos;
layout(location = 1) in  vec4 in_color;
layout(location = 2) in  vec4 in_pos;
layout(location = 0) out vec4 vertex_color;

out gl_PerVertex {
  vec4 gl_Position;
};

void main() {
  //const vec4 pos = vec4(vertices[gl_VertexIndex].xyz * 0.75, 1.0);
  const vec4 pos = vec4(in_vertex_pos * 0.5, 1.0) + in_pos;
  gl_Position = pos;
  //vertex_color = colors[gl_VertexIndex];
  vertex_color = in_color;
}