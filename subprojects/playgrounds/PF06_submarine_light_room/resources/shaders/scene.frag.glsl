#version 450

// Алгоритм: два project lights и camera flashlight дают direct Lambert + простой Blinn specular на каждом пикселе.
// Room irradiance — дешёвый diffuse bounce proxy, энергия которого выводится только из реально включённых lights:
// при blackout direct и indirect равны нулю, поэтому exposure не может изобрести силуэты. Low-light shadow pattern
// намеренно здесь не считается: он принадлежит participating medium и потому не исчезает вместе с surface GI.

layout(location = 0) in vec3 in_world_position;
layout(location = 1) in vec3 in_world_normal;
layout(location = 2) in vec3 in_albedo;
layout(location = 3) flat in vec3 in_material;
layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 normal_color;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 1, binding = 0, std140) uniform LightingBlock {
  // state: weak weight, safe weight, room-bounce coefficient, flashlight weight.
  vec4 state;
  // presentation: exposure, time seconds, pattern strength, pattern speed.
  vec4 presentation;
  vec4 weak_position_radius;
  vec4 weak_color_energy;
  vec4 safe_position_radius;
  vec4 safe_color_energy;
  vec4 flashlight_direction_cos;
  vec4 flashlight_color_energy;
  vec4 room_irradiance;
  // Current spatial reach: weak point radius, safe point radius, flashlight front distance, room GI presence.
  vec4 source_reach;
  // density, HG anisotropy, god-ray strength, suspended-mote strength.
  vec4 medium_params;
  vec4 medium_absorption;
  // rgb scattering colour, w large-scale density heterogeneity.
  vec4 medium_scattering;
  // operator, contrast, saturation, black crush; helmet strength, rim, tint, edge dirt.
  vec4 tonemap_params;
  vec4 helmet_params;
} lighting;

layout(set = 2, binding = 0) uniform sampler2DShadow flashlight_shadow;
layout(set = 2, binding = 1) uniform sampler2DShadow window_shadow;
layout(set = 2, binding = 2, std140) uniform ShadowBlock {
  mat4 flashlight_view_projection;
  mat4 window_view_projection;
  vec4 params;
  vec4 flashlight_position_range;
} shadows;

float shadow_visibility(
  sampler2DShadow shadow_map,
  const mat4 light_view_projection,
  const vec3 world_position,
  const vec3 normal,
  const float n_dot_l) {
  if (shadows.params.x < 0.5) return 1.0;
  const vec4 unoffset_clip = light_view_projection * vec4(world_position, 1.0);
  if (unoffset_clip.w <= 0.0) return 1.0;
  const float world_texel = 2.0 * unoffset_clip.w * shadows.params.y;
  const vec3 receiver = world_position + normal * world_texel * (0.55 + 1.35 * (1.0 - n_dot_l));
  const vec4 clip = light_view_projection * vec4(receiver, 1.0);
  const vec3 projected = clip.xyz / clip.w;
  const vec2 uv = projected.xy * 0.5 + 0.5;
  if (projected.z < 0.0 || projected.z > 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
    return 1.0;
  }
  const vec2 texel = vec2(1.0 / 1024.0);
  float visibility = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      visibility += texture(shadow_map, vec3(clamp(uv + vec2(x, y) * texel, texel * 0.5, 1.0 - texel * 0.5), projected.z));
    }
  }
  return visibility / 9.0;
}

vec3 point_light(
  const vec3 position,
  const float radius,
  const vec3 color,
  const float energy,
  const vec3 normal,
  const vec3 view_direction,
  const float roughness) {
  const vec3 delta = position - in_world_position;
  const float distance_to_light = length(delta);
  const vec3 light_direction = delta / max(distance_to_light, 0.0001);
  const float radial = clamp(1.0 - distance_to_light / radius, 0.0, 1.0);
  const float attenuation = radial * radial / (1.0 + 0.12 * distance_to_light * distance_to_light);
  const float diffuse = max(dot(normal, light_direction), 0.0);
  const vec3 half_direction = normalize(light_direction + view_direction);
  const float shininess = mix(96.0, 10.0, clamp(roughness, 0.0, 1.0));
  const float specular = pow(max(dot(normal, half_direction), 0.0), shininess) * (1.0 - roughness) * 0.32;
  return color * energy * attenuation * (diffuse + specular);
}

void main() {
  const vec3 normal = normalize(in_world_normal);
  const vec3 view_direction = normalize(camera_data.camera_position.xyz - in_world_position);
  const float roughness = clamp(in_material.y, 0.04, 1.0);

  const vec3 weak_to_light = normalize(lighting.weak_position_radius.xyz - in_world_position);
  const vec3 window_direction = normalize(vec3(1.0, -0.08, 0.10));
  const vec3 window_ray = normalize(in_world_position - lighting.weak_position_radius.xyz);
  const float window_cone = smoothstep(0.64, 0.78, dot(window_ray, window_direction));
  const float window_visibility = shadow_visibility(
    window_shadow, shadows.window_view_projection, in_world_position, normal, max(dot(normal, weak_to_light), 0.0));
  vec3 direct = point_light(
    lighting.weak_position_radius.xyz,
    max(lighting.source_reach.x, 0.001),
    lighting.weak_color_energy.rgb,
    lighting.weak_color_energy.w * lighting.state.x,
    normal,
    view_direction,
    roughness) * window_cone * window_visibility;
  direct += point_light(
    lighting.safe_position_radius.xyz,
    max(lighting.source_reach.y, 0.001),
    lighting.safe_color_energy.rgb,
    lighting.safe_color_energy.w * lighting.state.y,
    normal,
    view_direction,
    roughness);

  const vec3 to_surface = in_world_position - shadows.flashlight_position_range.xyz;
  const float flashlight_distance = length(to_surface);
  const vec3 flashlight_ray = to_surface / max(flashlight_distance, 0.0001);
  const float cone = smoothstep(
    lighting.flashlight_direction_cos.w,
    min(lighting.flashlight_direction_cos.w + 0.09, 0.999),
    dot(flashlight_ray, normalize(lighting.flashlight_direction_cos.xyz)));
  const float flashlight_front = 1.0 - smoothstep(
    max(lighting.source_reach.z - 0.75, 0.0),
    max(lighting.source_reach.z, 0.001),
    flashlight_distance);
  const vec3 flashlight_to_light = normalize(shadows.flashlight_position_range.xyz - in_world_position);
  const float flashlight_visibility = shadow_visibility(
    flashlight_shadow,
    shadows.flashlight_view_projection,
    in_world_position,
    normal,
    max(dot(normal, flashlight_to_light), 0.0));
  direct += point_light(
    shadows.flashlight_position_range.xyz,
    shadows.flashlight_position_range.w,
    lighting.flashlight_color_energy.rgb,
    lighting.flashlight_color_energy.w * lighting.state.w * cone * flashlight_front,
    normal,
    view_direction,
    roughness) * flashlight_visibility;

  vec3 indirect = lighting.room_irradiance.rgb *
                  lighting.room_irradiance.w * lighting.state.z * lighting.source_reach.w;

  // Material 2 = weak bioluminescent fixture, 3 = safe ceiling lamp. Their emission obeys source state.
  vec3 emission = vec3(0.0);
  if (in_material.x > 1.5 && in_material.x < 2.5) {
    emission = lighting.weak_color_energy.rgb * lighting.state.x * 0.42;
  } else if (in_material.x > 2.5) {
    emission = lighting.safe_color_energy.rgb * lighting.state.y * 0.55;
  }

  frag_color = vec4(in_albedo * (direct + indirect) + emission, 1.0);
  normal_color = vec4(normal * 0.5 + 0.5, 1.0);
}
