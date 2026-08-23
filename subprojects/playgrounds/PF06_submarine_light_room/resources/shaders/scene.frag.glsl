#version 450

// Алгоритм: два project lights и camera flashlight дают direct Lambert + простой Blinn specular на каждом пикселе.
// Room irradiance — дешёвый diffuse bounce proxy, энергия которого выводится только из реально включённых lights:
// при blackout direct и indirect равны нулю, поэтому exposure не может изобрести силуэты. В слабом свете world-space
// два по-разному текущих warped-ridge слоя темнят irradiance вытянутыми разветвляющимися полосами. Mode gate
// разрешает их только в exploration, direct-radiance gate убирает их из света, а screen-eccentricity gate оставляет
// центр спокойнее периферии. Это художественная модуляция для едва замечаемого боковым зрением движения, а не
// подмена настоящей shadow map.

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
} lighting;

float hash31(const vec3 p) {
  vec3 q = fract(p * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return fract((q.x + q.y) * q.z);
}

float value_noise(const vec3 p) {
  const vec3 cell = floor(p);
  vec3 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  const float n000 = hash31(cell + vec3(0, 0, 0));
  const float n100 = hash31(cell + vec3(1, 0, 0));
  const float n010 = hash31(cell + vec3(0, 1, 0));
  const float n110 = hash31(cell + vec3(1, 1, 0));
  const float n001 = hash31(cell + vec3(0, 0, 1));
  const float n101 = hash31(cell + vec3(1, 0, 1));
  const float n011 = hash31(cell + vec3(0, 1, 1));
  const float n111 = hash31(cell + vec3(1, 1, 1));
  return mix(
    mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
    mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
    f.z);
}

float shadow_pattern(const vec3 world_position) {
  const float t = lighting.presentation.y * lighting.presentation.w;
  vec3 p = world_position * vec3(0.28, 1.72, 0.25) + vec3(0.0, t * 0.56, t * 0.11);
  const float broad_warp = value_noise(p * 0.34 + vec3(0.0, t * 0.07, t * 0.03)) - 0.5;
  p.xz += vec2(broad_warp * 2.25, broad_warp * 1.55);
  p.x += sin(p.y * 0.72 + t * 0.23) * 0.32;

  // A narrow iso-ridge gives a long tongue; a slower offset ridge occasionally joins and splits it. Unlike the
  // discarded quantization, this changes the silhouette of the pattern rather than only posterizing its value.
  const float primary_ridge = pow(1.0 - abs(value_noise(p) * 2.0 - 1.0), 3.4);
  vec3 branch_p = p * vec3(1.46, 0.69, 1.31) + vec3(5.7, -t * 0.41, 9.3 + t * 0.08);
  branch_p.xz += vec2(sin(branch_p.y * 0.57 - t * 0.17), cos(branch_p.y * 0.49 + t * 0.13)) * 0.24;
  const float branch_ridge = pow(1.0 - abs(value_noise(branch_p) * 2.0 - 1.0), 4.2);
  const float filaments = max(primary_ridge, branch_ridge * 0.72);
  return smoothstep(0.14, 0.72, filaments);
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

  vec3 direct = point_light(
    lighting.weak_position_radius.xyz,
    max(lighting.source_reach.x, 0.001),
    lighting.weak_color_energy.rgb,
    lighting.weak_color_energy.w * lighting.state.x,
    normal,
    view_direction,
    roughness);
  direct += point_light(
    lighting.safe_position_radius.xyz,
    max(lighting.source_reach.y, 0.001),
    lighting.safe_color_energy.rgb,
    lighting.safe_color_energy.w * lighting.state.y,
    normal,
    view_direction,
    roughness);

  const vec3 to_surface = in_world_position - camera_data.camera_position.xyz;
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
  direct += point_light(
    camera_data.camera_position.xyz,
    12.0,
    lighting.flashlight_color_energy.rgb,
    lighting.flashlight_color_energy.w * lighting.state.w * cone * flashlight_front,
    normal,
    view_direction,
    roughness);

  vec3 indirect = lighting.room_irradiance.rgb *
                  lighting.room_irradiance.w * lighting.state.z * lighting.source_reach.w;
  const float direct_level = dot(direct, vec3(0.2126, 0.7152, 0.0722));
  const float exploration_gate = lighting.source_reach.w *
                                 (1.0 - smoothstep(0.05, 0.35, lighting.state.y));
  // A wide smooth interval makes the pattern strongest in indirect-only shadow, then lets the real source erase
  // it continuously through the penumbra instead of producing a second hard lighting boundary.
  const float direct_shadow_gate = 1.0 - smoothstep(0.018, 0.22, direct_level);
  const vec3 view_position = (camera_data.view * vec4(in_world_position, 1.0)).xyz;
  const float view_depth = max(-view_position.z, 0.001);
  const float tan_half_fov = tan(radians(65.0) * 0.5);
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const vec2 screen_position = view_position.xy / (view_depth * tan_half_fov * vec2(aspect, 1.0));
  const float peripheral = smoothstep(0.28, 0.88, length(screen_position));
  const float peripheral_weight = mix(0.48, 1.08, peripheral);
  float pattern = 0.0;
  // This coherent runtime bypass makes the CLI A/B a useful cost measurement too; a production graph generation
  // may still remove the feature entirely when the project does not use it.
  if (lighting.presentation.z > 0.0001 && exploration_gate > 0.0001 && direct_shadow_gate > 0.0001) {
    pattern = shadow_pattern(in_world_position) * lighting.presentation.z *
              exploration_gate * direct_shadow_gate * peripheral_weight;
  }
  indirect *= max(1.0 - pattern * 0.94, 0.035);

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
