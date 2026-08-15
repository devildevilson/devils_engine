#version 450

#ifndef PF02_PCF_RADIUS
#define PF02_PCF_RADIUS 1
#endif

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  mat4 view_projection;
  mat4 view;
  mat4 light_view_projection;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 light_direction;
  vec4 shadow_params;
} scene_data[3];

struct SpotLight {
  mat4 light_view_projection;
  vec4 position_range;
  vec4 direction_outer;
  vec4 color_intensity;
};

layout(set = 0, binding = 1, std430) readonly buffer SpotLightBuffer {
  SpotLight lights[];
} spot_data[3];

layout(set = 1, binding = 0) uniform sampler2D directional_shadow_image[3];
layout(set = 1, binding = 1) uniform sampler2D spot_shadow_atlas[3];

float directional_visibility(const float n_dot_l) {
  const vec4 clip = scene_data[0].light_view_projection * vec4(world_position, 1.0);
  const vec3 projected = clip.xyz / clip.w;
  const vec2 shadow_uv = projected.xy * 0.5 + 0.5;
  if (projected.z < 0.0 || projected.z > 1.0 || any(lessThan(shadow_uv, vec2(0.0))) || any(greaterThan(shadow_uv, vec2(1.0)))) {
    return 1.0;
  }

  const float resolution = scene_data[0].shadow_params.x;
  const float bias = scene_data[0].shadow_params.y + scene_data[0].shadow_params.z * (1.0 - n_dot_l);
  const vec2 texel = vec2(1.0 / resolution);
  float visible = 0.0;
  float samples = 0.0;
  for (int y = -PF02_PCF_RADIUS; y <= PF02_PCF_RADIUS; ++y) {
    for (int x = -PF02_PCF_RADIUS; x <= PF02_PCF_RADIUS; ++x) {
      const float stored_depth = texture(directional_shadow_image[0], shadow_uv + vec2(x, y) * texel).r;
      visible += projected.z + bias >= stored_depth ? 1.0 : 0.0;
      samples += 1.0;
    }
  }
  return visible / samples;
}

float spot_visibility(const int index, const vec3 position, const float n_dot_l) {
  const vec4 clip = spot_data[0].lights[index].light_view_projection * vec4(position, 1.0);
  const vec3 projected = clip.xyz / clip.w;
  const vec2 local_uv = projected.xy * 0.5 + 0.5;
  if (projected.z < 0.0 || projected.z > 1.0 || any(lessThan(local_uv, vec2(0.0))) || any(greaterThan(local_uv, vec2(1.0)))) {
    return 1.0;
  }

  const vec2 tile = vec2(float(index & 1), float(index >> 1));
  const vec2 atlas_uv = (local_uv + tile) * 0.5;
  const float resolution = scene_data[0].shadow_params.x;
  const float bias = scene_data[0].shadow_params.y + scene_data[0].shadow_params.z * (1.0 - n_dot_l);
  const vec2 texel = vec2(1.0 / resolution);
  float visible = 0.0;
  float samples = 0.0;
  for (int y = -PF02_PCF_RADIUS; y <= PF02_PCF_RADIUS; ++y) {
    for (int x = -PF02_PCF_RADIUS; x <= PF02_PCF_RADIUS; ++x) {
      const float stored_depth = texture(spot_shadow_atlas[0], atlas_uv + vec2(x, y) * texel).r;
      visible += projected.z + bias >= stored_depth ? 1.0 : 0.0;
      samples += 1.0;
    }
  }
  return visible / samples;
}

vec3 surface_albedo(const vec3 normal) {
  if (normal.y > 0.8) {
    const ivec2 cell = ivec2(floor(world_position.xz));
    const float checker = ((cell.x + cell.y) & 1) == 0 ? 0.62 : 0.78;
    return vec3(checker, checker * 0.97, checker * 0.90);
  }
  if (normal.z > 0.8) return vec3(0.55, 0.61, 0.70);
  return vec3(0.72, 0.48, 0.30);
}

void main() {
  const vec3 normal = normalize(world_normal);
  const vec3 to_light = normalize(-scene_data[0].light_direction.xyz);
  const float diffuse = max(dot(normal, to_light), 0.0);
  const float visibility = directional_visibility(diffuse);
  const vec3 albedo = surface_albedo(normal);
  const vec3 view_direction = normalize(scene_data[0].camera_position.xyz - world_position);
  const vec3 half_direction = normalize(to_light + view_direction);
  const float specular = pow(max(dot(normal, half_direction), 0.0), 48.0) * step(0.0, diffuse);
  const vec3 ambient = albedo * 0.055;
  const vec3 direct = (albedo * diffuse + vec3(0.16) * specular) * vec3(1.0, 0.93, 0.78) * 1.35 * visibility;

  vec3 spot_lighting = vec3(0.0);
  for (int index = 0; index < 4; ++index) {
    const vec3 to_light_vector = spot_data[0].lights[index].position_range.xyz - world_position;
    const float distance_to_light = length(to_light_vector);
    const float range = spot_data[0].lights[index].position_range.w;
    if (distance_to_light >= range) continue;
    const vec3 spot_to_surface = -to_light_vector / max(distance_to_light, 0.0001);
    const float cone_cosine = dot(spot_to_surface, spot_data[0].lights[index].direction_outer.xyz);
    const float outer_cosine = spot_data[0].lights[index].direction_outer.w;
    const float cone = smoothstep(outer_cosine, min(outer_cosine + 0.10, 0.999), cone_cosine);
    if (cone <= 0.0) continue;
    const vec3 surface_to_light = -spot_to_surface;
    const float spot_diffuse = max(dot(normal, surface_to_light), 0.0);
    const float edge = max(1.0 - distance_to_light / range, 0.0);
    const float attenuation = edge * edge * cone;
    const float spot_shadow = spot_visibility(index, world_position, spot_diffuse);
    const vec3 spot_half = normalize(surface_to_light + view_direction);
    const float spot_specular = pow(max(dot(normal, spot_half), 0.0), 48.0) * step(0.0, spot_diffuse);
    spot_lighting += (albedo * spot_diffuse + vec3(0.12) * spot_specular) *
      spot_data[0].lights[index].color_intensity.rgb *
      spot_data[0].lights[index].color_intensity.a * attenuation * spot_shadow;
  }

  const vec3 hdr = ambient + direct + spot_lighting;
  out_color = vec4(hdr / (hdr + vec3(1.0)), 1.0);
}
