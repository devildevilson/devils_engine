#version 450

#include "pf02_records.glsl"

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  PF02_SCENE_BLOCK_BODY
} scene_data;

void main() {
  const vec2 viewport = scene_data.viewport_near.xy;
  const vec2 clip = in_position / viewport * 2.0 - 1.0;
  gl_Position = vec4(clip, 0.0, 1.0);
  out_uv = in_uv;
  out_color = in_color;
}
