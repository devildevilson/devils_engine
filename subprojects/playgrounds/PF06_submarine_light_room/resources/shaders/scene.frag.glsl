#version 450

// Алгоритм: два project lights и camera flashlight дают direct Lambert + простой Blinn specular на каждом пикселе.
// Room irradiance — дешёвый diffuse bounce proxy, энергия которого выводится только из реально включённых lights:
// при blackout direct и indirect равны нулю, поэтому exposure не может изобрести силуэты. При наличии хотя бы одного
// source небольшой orientation floor не даёт exploration превратиться в blackout. Отдельный медленный surface ridge
// модулирует только этот indirect в пределах нескольких десятков процентов; direct light плавно стирает его. Два
// разных warp-состояния непрерывно перетекают друг в друга, поэтому ridge меняет форму, а не просто скользит по стене.

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
  // shadow wall: strength, camera-centred clear radius, transition width, boundary-noise amplitude.
  vec4 shadow_wall_params;
} lighting;

layout(set = 2, binding = 0) uniform sampler2DShadow flashlight_shadow;
layout(set = 2, binding = 1) uniform sampler2DShadow window_shadow;
layout(set = 2, binding = 2, std140) uniform ShadowBlock {
  mat4 flashlight_view_projection;
  mat4 window_view_projection;
  vec4 params;
  vec4 flashlight_position_range;
} shadows;

float hash21(const vec2 p) {
  vec3 q = fract(vec3(p.xyx) * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return fract((q.x + q.y) * q.z);
}

float value_noise(const vec2 p) {
  const vec2 cell = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(
    mix(hash21(cell), hash21(cell + vec2(1, 0)), f.x),
    mix(hash21(cell + vec2(0, 1)), hash21(cell + vec2(1, 1)), f.x),
    f.y);
}

float surface_pressure_pattern(const vec3 world_position, const vec3 normal) {
  // Project one coherent world-space flow direction into the receiver tangent plane. The pattern therefore follows
  // the actual surface normal: on the floor it lies in XZ, on walls it turns into their plane, and neither inherits
  // the screen axes. The fallback only handles a normal accidentally parallel to the preferred flow direction.
  const vec3 n = normalize(normal);
  const vec3 preferred_flow = normalize(vec3(0.58, 0.21, 0.79));
  vec3 tangent = preferred_flow - n * dot(preferred_flow, n);
  if (dot(tangent, tangent) < 0.0025) {
    const vec3 fallback_flow = vec3(0.13, 0.97, -0.21);
    tangent = fallback_flow - n * dot(fallback_flow, n);
  }
  tangent = normalize(tangent);
  const vec3 bitangent = normalize(cross(n, tangent));
  const vec2 plane = vec2(dot(world_position, tangent), dot(world_position, bitangent));
  const float t = lighting.presentation.y * lighting.presentation.w;
  // At the default speed this cycle takes roughly two minutes. The two samples have unrelated axes, so their
  // cross-fade changes ridge topology instead of looking like a texture translation.
  const float morph = 0.5 + 0.5 * sin(t * 0.70 - 0.8);
  const vec2 breathing_scale = vec2(
    1.0 + sin(t * 0.43) * 0.075,
    1.0 + cos(t * 0.31) * 0.055);
  vec2 p = plane * vec2(0.34, 1.28) * breathing_scale + vec2(t * 0.055, -t * 0.19);
  const float warp_a = value_noise(p * 0.31 + vec2(-t * 0.018, t * 0.011));
  const float warp_b = value_noise(p.yx * vec2(-0.24, 0.37) + vec2(13.7 + t * 0.013, -5.4 - t * 0.016));
  const float warp = mix(warp_a, warp_b, morph) - 0.5;
  p.x += warp * 2.1 + sin(p.y * 0.61 + t * 0.42) * (0.24 + morph * 0.10);
  p.y += sin(p.x * 0.23 - t * 0.29) * 0.13;
  const float primary = pow(1.0 - abs(value_noise(p) * 2.0 - 1.0), 4.0);
  const vec2 branch_p = p * vec2(1.41, 0.73) + vec2(7.3 - t * 0.08, 3.1 + t * 0.045);
  const float branch = pow(1.0 - abs(value_noise(branch_p) * 2.0 - 1.0), 5.0);
  const float branch_weight = mix(0.48, 0.68, 0.5 + 0.5 * cos(t * 0.53 + 1.7));
  return smoothstep(0.18, 0.76, max(primary, branch * branch_weight));
}

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

  const float safe_gate = smoothstep(0.05, 0.60, lighting.state.y);
  const float exploration_gate = lighting.source_reach.w * (1.0 - safe_gate);
  const float orientation_floor = 0.085 * exploration_gate;
  const float indirect_strength = max(lighting.state.z * lighting.source_reach.w, orientation_floor);
  vec3 indirect = lighting.room_irradiance.rgb * indirect_strength;

  const float direct_level = dot(direct, vec3(0.2126, 0.7152, 0.0722));
  const float direct_shadow_gate = 1.0 - smoothstep(0.018, 0.22, direct_level);
  const float pressure_strength = clamp(lighting.presentation.z, 0.0, 2.0);
  float surface_pressure = 0.0;
  if (pressure_strength * exploration_gate * direct_shadow_gate > 0.0001) {
    const vec3 view_position = (camera_data.view * vec4(in_world_position, 1.0)).xyz;
    const float view_depth = max(-view_position.z, 0.001);
    const float tan_half_fov = tan(radians(65.0) * 0.5);
    const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
    const vec2 screen_position = view_position.xy / (view_depth * tan_half_fov * vec2(aspect, 1.0));
    const float peripheral = smoothstep(0.30, 0.90, length(screen_position));
    surface_pressure = surface_pressure_pattern(in_world_position, normal) * pressure_strength *
                       exploration_gate * direct_shadow_gate * mix(0.38, 1.0, peripheral);
  }
  indirect *= 1.0 - min(surface_pressure * 0.17, 0.30);

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
