#version 450

// Алгоритм: full-resolution surface colour не блюрится. Вместо этого четыре соседних half-resolution medium
// samples смешиваются bilinear weights, дополнительно подавленными относительной разницей linear depth. Полученные
// scattering/transmittance применяются к точному текущему surface pixel, после чего fixed exposure и ACES-like
// curve делают финальный LDR кадр. Depth-aware weights не дают туману дальней стены протекать через ближний силуэт.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D lit_color;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler2D medium_scattering;
layout(set = 0, binding = 3) uniform sampler2D medium_transmittance;
layout(set = 0, binding = 4, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;
layout(set = 0, binding = 5, std140) uniform LightingBlock {
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
  vec4 medium_scattering_data;
  vec4 tonemap_params;
  vec4 helmet_params;
} lighting;

vec3 aces_fitted(const vec3 value) {
  const vec3 a = value * (2.51 * value + 0.03);
  const vec3 b = value * (2.43 * value + 0.59) + 0.14;
  return clamp(a / b, 0.0, 1.0);
}

vec3 hable_curve(const vec3 value) {
  const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
  return ((value * (A * value + C * B) + D * E) /
          (value * (A * value + B) + D * F)) - E / F;
}

vec3 apply_tonemap(const vec3 value, const int op) {
  if (op == 0) return value / (1.0 + value);
  if (op == 1) return clamp(hable_curve(value * 2.0) / hable_curve(vec3(11.2)), 0.0, 1.0);
  return aces_fitted(value);
}

float linear_distance(const vec2 uv, const float depth) {
  if (depth <= 0.000001) return 18.0;
  const vec2 ndc = uv * 2.0 - 1.0;
  const float tan_half_fov = tan(radians(65.0) * 0.5);
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const vec3 view_ray = normalize(vec3(ndc.x * aspect * tan_half_fov, -ndc.y * tan_half_fov, -1.0));
  return camera_data.viewport_near.z / max(depth * -view_ray.z, 0.000001);
}

void main() {
  const vec3 surface = texture(lit_color, in_uv).rgb;
  const float target_distance = linear_distance(in_uv, texture(scene_depth, in_uv).r);
  const ivec2 medium_size = textureSize(medium_scattering, 0);
  const vec2 texel_position = in_uv * vec2(medium_size) - 0.5;
  const ivec2 base = ivec2(floor(texel_position));
  const vec2 fraction = fract(texel_position);

  vec3 scattering = vec3(0.0);
  vec3 transmittance = vec3(0.0);
  float total_weight = 0.0;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      const ivec2 offset = ivec2(x, y);
      const ivec2 coord = clamp(base + offset, ivec2(0), medium_size - 1);
      const vec4 candidate_scattering = texelFetch(medium_scattering, coord, 0);
      const vec3 candidate_transmittance = texelFetch(medium_transmittance, coord, 0).rgb;
      const vec2 axis_weight = mix(1.0 - fraction, fraction, vec2(offset));
      const float spatial_weight = axis_weight.x * axis_weight.y;
      const float relative_depth_error = abs(candidate_scattering.a - target_distance) /
                                         max(target_distance, 0.25);
      const float depth_weight = exp(-relative_depth_error * 28.0);
      const float weight = spatial_weight * depth_weight + 0.000001;
      scattering += candidate_scattering.rgb * weight;
      transmittance += candidate_transmittance * weight;
      total_weight += weight;
    }
  }
  scattering /= total_weight;
  transmittance /= total_weight;

  vec3 color = (surface * transmittance + scattering) * lighting.presentation.x;
  color *= vec3(0.91, 1.00, 1.06);
  color = max(color - lighting.tonemap_params.w, vec3(0.0));
  color = pow(color, vec3(max(lighting.tonemap_params.y, 0.01)));
  color = apply_tonemap(color, int(lighting.tonemap_params.x + 0.5));
  const float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
  color = mix(vec3(luma), color, lighting.tonemap_params.z);
  frag_color = vec4(color, 1.0);
}
