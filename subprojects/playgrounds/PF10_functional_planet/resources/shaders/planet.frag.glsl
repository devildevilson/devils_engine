#version 450
#include "pf10_planet.glsl"

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 planet_to_world;
  mat4 world_to_planet;
  vec4 camera_position;
  vec4 light_direction;
  vec4 border_colour;
  uvec4 params;
  vec4 viewport_near;
} camera_data;

layout(location = 0) in vec3 in_local_direction;
layout(location = 1) in vec3 in_world_position;
layout(location = 2) in float in_height;
layout(location = 0) out vec4 out_color;

vec3 province_colour(const uint id) {
  const uint value = pf10_mix32(id);
  const vec3 random = vec3(float((value >> 0u) & 255u), float((value >> 8u) & 255u),
                           float((value >> 16u) & 255u)) / 255.0;
  return mix(vec3(0.24, 0.31, 0.18), vec3(0.64, 0.52, 0.31), random * 0.72);
}

void main() {
  const vec3 direction = normalize(in_local_direction);
  const pf10_region_sample region = pf10_sample_region(direction);
  const bool political = (camera_data.params.w & 1u) != 0u;
  vec3 albedo;
  if (region.kind == 2u) {
    albedo = direction.y > 0.0 ? vec3(0.76, 0.82, 0.86) : vec3(0.66, 0.73, 0.80);
  } else if (region.kind == 1u) {
    const float ocean_variant = float(pf10_mix32(region.id) & 255u) / 255.0;
    albedo = mix(vec3(0.035, 0.15, 0.25), vec3(0.055, 0.31, 0.42), ocean_variant);
  } else if (political) {
    albedo = province_colour(region.id);
  } else {
    const float elevation = clamp((in_height - PF10_MIN_HEIGHT) / (PF10_MAX_HEIGHT - PF10_MIN_HEIGHT), 0.0, 1.0);
    albedo = mix(vec3(0.16, 0.26, 0.12), vec3(0.54, 0.45, 0.31), elevation);
  }

  if (region.id == camera_data.params.x) albedo = mix(albedo, vec3(1.0, 0.68, 0.12), 0.62);
  else if (region.id == camera_data.params.y) albedo = mix(albedo, vec3(0.95), 0.30);

  vec3 normal = normalize(cross(dFdx(in_world_position), dFdy(in_world_position)));
  const vec3 radial = normalize((camera_data.planet_to_world * vec4(direction, 0.0)).xyz);
  if (dot(normal, radial) < 0.0) normal = -normal;
  const float direct = max(dot(normal, -camera_data.light_direction.xyz), 0.0);
  const float rim = pow(1.0 - max(dot(normal, normalize(camera_data.camera_position.xyz - in_world_position)), 0.0), 3.0);
  vec3 colour = albedo * (0.19 + direct * 0.86) + vec3(0.055, 0.085, 0.12) * rim;

  // edge is a continuous distance to the nearest political/coast/polar boundary. fwidth makes the line
  // stay readable while zooming and gives every palette the same geometric border.
  const float line_width = max(fwidth(region.edge) * 1.45, 0.012);
  const float border = 1.0 - smoothstep(0.0, line_width, region.edge);
  colour = mix(colour, camera_data.border_colour.rgb, border * camera_data.border_colour.a);
  out_color = vec4(colour, 1.0);
}
