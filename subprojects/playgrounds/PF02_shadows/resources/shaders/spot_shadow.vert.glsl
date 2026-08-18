#version 450

#include "pf02_records.glsl"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_instance;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  PF02_SCENE_BLOCK_BODY
} scene_data[3];


layout(set = 0, binding = 1, std430) readonly buffer SpotLightBuffer {
  SpotLight lights[];
} spot_data[3];

layout(push_constant) uniform RegionPush {
  uint data_index;
} region;

void main() {
  const vec3 unused_attributes = (in_normal + vec3(in_uv, 0.0)) * scene_data[0].shadow_params.w;
  const vec3 world_position = in_position + in_instance.xyz + unused_attributes;
  gl_Position = spot_data[0].lights[region.data_index].light_view_projection * vec4(world_position, 1.0);
}
