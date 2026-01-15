#version 450

const vec4 vertices[] = {
  vec4(-1,-1,0,1),
  vec4(1,-1,0,1),
  vec4(0,1,0,1),
};

const vec4 colors[] = {
  vec4(1,0,0,1),
  vec4(0,1,0,1),
  vec4(0,0,1,1),
};

layout(location = 0) out vec4 vertex_color;

out gl_PerVertex {
  vec4 gl_Position;
};

void main() {
  gl_Position = vertices[gl_VertexIndex];
  vertex_color = colors[gl_VertexIndex];
}