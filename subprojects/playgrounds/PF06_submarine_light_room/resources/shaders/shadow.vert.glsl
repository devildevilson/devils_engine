#version 450

// Алгоритм: тот же instanced unit cube, что и в основном scene pass, заново проецируется из одного из двух
// project-owned источников. Две material definitions выбирают camera flashlight либо фиксированный боковой
// window light; геометрия и instance stream остаются общими, поэтому shadow caster не расходится с видимой сценой.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_position_material;
layout(location = 3) in vec4 in_half_roughness;
layout(location = 4) in vec4 in_albedo_metallic;

layout(set = 0, binding = 0, std140) uniform ShadowBlock {
  mat4 flashlight_view_projection;
  mat4 window_view_projection;
  vec4 params;
  vec4 flashlight_position_range;
} shadows;

void main() {
  const vec3 layout_guard = (in_normal + in_albedo_metallic.xyz) * shadows.params.w;
  const vec3 world_position = in_position * in_half_roughness.xyz * 2.0 +
                              in_position_material.xyz + layout_guard;
#if PF06_WINDOW_SHADOW
  gl_Position = shadows.window_view_projection * vec4(world_position, 1.0);
#else
  gl_Position = shadows.flashlight_view_projection * vec4(world_position, 1.0);
#endif
}
