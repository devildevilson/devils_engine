#version 450

// Алгоритм: medium считается на половине ширины/высоты. Reverse-Z depth ограничивает camera ray первым opaque
// surface, после чего 20 midpoint samples интегрируют Beer-Lambert transmittance и single in-scattering. Результат
// не содержит surface colour: MRT хранит scattering.rgb + linear distance и transmittance.rgb, чтобы full-resolution
// compose мог применить коэффициенты к своему точному surface pixel без размытия материала через силуэты.
// Indoor volume shadow использует отдельное дешёвое плавное поле, не совпадающее с обычной мутью. В baseline оно
// лишь на 0..15% меняет density и совсем немного подавляет scattering. Только верхняя часть authored strength
// постепенно возвращает тяжёлую дополнительную optical depth как специальный, а не постоянный эффект.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_scattering_depth;
layout(location = 1) out vec4 out_transmittance;

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;
layout(set = 0, binding = 2, std140) uniform LightingBlock {
  vec4 state;
  vec4 presentation;
  vec4 weak_position_radius;
  vec4 weak_color_energy;
  vec4 safe_position_radius;
  vec4 safe_color_energy;
  vec4 flashlight_direction_cos;
  vec4 flashlight_color_energy;
  vec4 room_irradiance;
  vec4 source_reach;
  vec4 medium_params;
  vec4 medium_absorption;
  vec4 medium_scattering;
  vec4 tonemap_params;
  vec4 helmet_params;
} lighting;
layout(set = 0, binding = 3) uniform sampler2DShadow flashlight_shadow;
layout(set = 0, binding = 4) uniform sampler2DShadow window_shadow;
layout(set = 0, binding = 5, std140) uniform ShadowBlock {
  mat4 flashlight_view_projection;
  mat4 window_view_projection;
  vec4 params;
  vec4 flashlight_position_range;
} shadows;

const int medium_steps = 20;
const float pi = 3.14159265359;

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

float phase_hg(const float cosine, const float anisotropy) {
  const float g = clamp(anisotropy, -0.85, 0.85);
  const float denominator = max(1.0 + g * g - 2.0 * g * cosine, 0.001);
  return (1.0 - g * g) / (4.0 * pi * denominator * sqrt(denominator));
}

float signed_volume_shadow(const vec3 world_position, const float time) {
  const vec3 p = world_position * vec3(0.24, 0.085, 0.19) +
                 vec3(time * 0.010, -time * 0.006, time * 0.008);
  const float first = sin(p.x * 2.3 + sin(p.y * 1.7 + p.z * 0.8) * 1.15);
  const float second = sin(p.z * 2.8 - p.x * 0.55 + cos(p.y * 2.1) * 0.82);
  return clamp(first * 0.64 + second * 0.36, -1.0, 1.0);
}

float shadow_visibility(
  sampler2DShadow shadow_map,
  const mat4 light_view_projection,
  const vec3 position) {
  if (shadows.params.x < 0.5) return 1.0;
  const vec4 clip = light_view_projection * vec4(position, 1.0);
  if (clip.w <= 0.0) return 1.0;
  const vec3 projected = clip.xyz / clip.w;
  const vec2 uv = projected.xy * 0.5 + 0.5;
  if (projected.z < 0.0 || projected.z > 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
    return 1.0;
  }
  return texture(shadow_map, vec3(clamp(uv, vec2(0.0005), vec2(0.9995)), projected.z));
}

void main() {
  const float depth = texture(scene_depth, in_uv).r;
  const vec2 ndc = in_uv * 2.0 - 1.0;
  const float tan_half_fov = tan(radians(65.0) * 0.5);
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const vec3 view_ray = normalize(vec3(ndc.x * aspect * tan_half_fov, -ndc.y * tan_half_fov, -1.0));
  const vec3 ray_direction = normalize(transpose(mat3(camera_data.view)) * view_ray);
  const float surface_distance = depth > 0.000001
    ? camera_data.viewport_near.z / max(depth * -view_ray.z, 0.000001)
    : 18.0;
  const float ray_length = min(surface_distance, 18.0);
  const float step_length = ray_length / float(medium_steps);

  const float safe_gate = smoothstep(0.05, 0.60, lighting.state.y);
  const float base_density = lighting.medium_params.x * mix(1.0, 0.34, safe_gate);
  const float exploration_volume_gate = lighting.source_reach.w * (1.0 - safe_gate);
  const float volume_shadow_strength = clamp(lighting.medium_absorption.w, 0.0, 2.0);
  const float subtle_shadow_strength = min(volume_shadow_strength, 1.0);
  const float heavy_shadow_strength = smoothstep(1.0, 2.0, volume_shadow_strength);
  const vec3 absorption = max(lighting.medium_absorption.rgb, vec3(0.001));
  const vec3 flashlight_forward = normalize(lighting.flashlight_direction_cos.xyz);
  const vec3 ambient_source = lighting.room_irradiance.rgb * lighting.source_reach.w * 0.42;

  vec3 transmittance = vec3(1.0);
  vec3 in_scattering = vec3(0.0);
  float heavy_shadow_peak = 0.0;
  float cached_window_visibility = 1.0;
  float cached_flashlight_visibility = 1.0;
  const float time = lighting.presentation.y;
  if (base_density > 0.000001) {
    for (int i = 0; i < medium_steps; ++i) {
      const float distance_along_ray = (float(i) + 0.5) * step_length;
      const vec3 sample_position = camera_data.camera_position.xyz + ray_direction * distance_along_ray;
      const vec3 drift = vec3(time * 0.018, -time * 0.010, time * 0.013);
      const float broad_noise = value_noise(sample_position * 0.16 + drift);
      const float filament = value_noise(sample_position * vec3(0.42, 0.18, 0.42) - drift * 0.7);
      const float mote_noise = hash31(floor((sample_position + drift * 4.0) * 12.0));
      const float mote = smoothstep(0.991, 0.999, mote_noise) * lighting.medium_params.w;
      const float heterogeneity = mix(1.0, 0.45 + broad_noise * 1.35, lighting.medium_scattering.w);
      float volume_shadow = 0.0;
      if (volume_shadow_strength * exploration_volume_gate > 0.0001) {
        volume_shadow = signed_volume_shadow(sample_position, time) * exploration_volume_gate;
      }
      const float positive_shadow = max(volume_shadow, 0.0);
      const float density_shadow = 1.0 + volume_shadow * subtle_shadow_strength * 0.15;
      const float near_volume_weight = 1.0 - smoothstep(8.0, 14.0, distance_along_ray);
      heavy_shadow_peak = max(heavy_shadow_peak, positive_shadow * near_volume_weight);
      const float local_density = base_density * heterogeneity *
                                  (0.78 + filament * 0.38 + mote * 2.2) * density_shadow;

      const float weak_distance = length(sample_position - lighting.weak_position_radius.xyz);
      const float weak_radial = clamp(1.0 - weak_distance / max(lighting.source_reach.x, 0.001), 0.0, 1.0);
      const vec3 window_direction = normalize(vec3(1.0, -0.08, 0.10));
      const vec3 window_ray = normalize(sample_position - lighting.weak_position_radius.xyz);
      const float window_cone = smoothstep(0.64, 0.78, dot(window_ray, window_direction));
      // Shadow visibility varies much more slowly along a half-resolution ray than density. Reuse one compare for
      // two neighbouring midpoint samples; source/cone gates also avoid map reads where contribution is zero.
      if ((i & 1) == 0 && lighting.state.x * window_cone > 0.0001) {
        cached_window_visibility = shadow_visibility(window_shadow, shadows.window_view_projection, sample_position);
      }
      const vec3 weak_scatter = lighting.weak_color_energy.rgb * lighting.weak_color_energy.w *
                                lighting.state.x * weak_radial * weak_radial * window_cone * cached_window_visibility * 0.055;
      const vec3 flashlight_delta = sample_position - shadows.flashlight_position_range.xyz;
      const float flashlight_distance = length(flashlight_delta);
      const vec3 flashlight_ray = flashlight_delta / max(flashlight_distance, 0.0001);
      const float flashlight_angle = dot(flashlight_ray, flashlight_forward);
      const float flashlight_cone = smoothstep(
        lighting.flashlight_direction_cos.w,
        min(lighting.flashlight_direction_cos.w + 0.075, 0.999),
        flashlight_angle);
      const float phase = phase_hg(dot(flashlight_ray, ray_direction), lighting.medium_params.y) * 4.0 * pi;
      const float flashlight_front = 1.0 - smoothstep(
        max(lighting.source_reach.z - 0.85, 0.0),
        max(lighting.source_reach.z, 0.001),
        flashlight_distance);
      const float flashlight_falloff = 1.0 / (1.0 + flashlight_distance * flashlight_distance * 0.075);
      if ((i & 1) == 0 && lighting.state.w * flashlight_cone > 0.0001) {
        cached_flashlight_visibility = shadow_visibility(
          flashlight_shadow, shadows.flashlight_view_projection, sample_position);
      }
      const vec3 flashlight_scatter = lighting.flashlight_color_energy.rgb *
        lighting.flashlight_color_energy.w * lighting.state.w * flashlight_cone * flashlight_front *
        flashlight_falloff * lighting.medium_params.z * phase * cached_flashlight_visibility * 0.006;
      const float shadowed_scattering = 1.0 - positive_shadow * subtle_shadow_strength * 0.08;
      const vec3 source_radiance = (ambient_source + weak_scatter + flashlight_scatter) * shadowed_scattering;

      const vec3 step_transmittance = exp(-absorption * local_density * step_length);
      in_scattering += transmittance * source_radiance * lighting.medium_scattering.rgb *
                       local_density * step_length;
      transmittance *= step_transmittance;
    }
  }

  // The striking old black volumes remain reachable only above strength 1.0. Normal indoor tuning (default < 1)
  // never enters this branch and therefore cannot form opaque random clouds.
  const float heavy_occlusion = smoothstep(0.52, 0.92, heavy_shadow_peak) * heavy_shadow_strength;
  const float heavy_optical_depth = base_density * ray_length * heavy_occlusion * 0.38;
  in_scattering *= 1.0 - heavy_occlusion * 0.42;
  transmittance *= exp(-absorption * heavy_optical_depth);

  out_scattering_depth = vec4(in_scattering, surface_distance);
  out_transmittance = vec4(transmittance, 1.0);
}
