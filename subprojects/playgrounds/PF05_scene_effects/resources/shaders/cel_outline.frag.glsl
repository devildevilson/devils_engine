#version 450

// Алгоритм: fullscreen edge detector читает уже готовые opaque depth и world normals. Policy `silhouette`
// считает только относительные разрывы linear depth и границу geometry/background; `feature` добавляет разрывы
// normal и поэтому подчёркивает складки/грани. Круглая окрестность runtime-радиуса 1..3 px даёт постоянную
// экранную толщину. Контур композится сразу после opaque lighting, поэтому более поздние decals/particles/text
// остаются самостоятельными слоями и могут его перекрыть.

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 1, binding = 0) uniform sampler2D scene_depth_texture;
layout(set = 1, binding = 1) uniform sampler2D scene_normal_texture;

layout(set = 2, binding = 0, std140) uniform CelSettingsBlock {
  vec4 lighting;
  vec4 outline;
  vec4 outline_color;
} cel;

layout(location = 0) out vec4 out_color;

float linear_depth(const float device_depth) {
  return camera_data.viewport_near.z / max(device_depth, 1e-7);
}

void main() {
  const uint policy = uint(cel.outline.x + 0.5);
  if (policy == 0u) discard;

  const ivec2 size = textureSize(scene_depth_texture, 0);
  const ivec2 center_coord = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
  const float center_device_depth = texelFetch(scene_depth_texture, center_coord, 0).r;
  const bool center_geometry = center_device_depth > 0.0;
  const float center_depth = center_geometry ? linear_depth(center_device_depth) : 0.0;
  const vec3 center_normal = normalize(
    texelFetch(scene_normal_texture, center_coord, 0).xyz * 2.0 - 1.0);
  const int radius = clamp(int(cel.outline.y + 0.5), 1, 3);

  float edge = 0.0;
  for (int y = -3; y <= 3; ++y) {
    for (int x = -3; x <= 3; ++x) {
      if ((x == 0 && y == 0) || abs(x) > radius || abs(y) > radius || x * x + y * y > radius * radius) continue;
      const ivec2 sample_coord = clamp(center_coord + ivec2(x, y), ivec2(0), size - 1);
      const float sample_device_depth = texelFetch(scene_depth_texture, sample_coord, 0).r;
      const bool sample_geometry = sample_device_depth > 0.0;

      if (center_geometry != sample_geometry) {
        edge = 1.0;
        continue;
      }
      if (!center_geometry) continue;

      const float sample_depth = linear_depth(sample_device_depth);
      const float relative_depth_delta = abs(sample_depth - center_depth) /
        max(min(sample_depth, center_depth), camera_data.viewport_near.z);
      const float depth_edge = smoothstep(cel.outline.z, cel.outline.z * 1.75, relative_depth_delta);
      edge = max(edge, depth_edge);

      if (policy >= 2u) {
        const vec3 sample_normal = normalize(
          texelFetch(scene_normal_texture, sample_coord, 0).xyz * 2.0 - 1.0);
        const float normal_edge = 1.0 - smoothstep(
          max(cel.outline.w - 0.08, 0.0), cel.outline.w, dot(center_normal, sample_normal));
        edge = max(edge, normal_edge);
      }
    }
  }

  if (edge <= 1.0 / 255.0) discard;
  out_color = vec4(cel.outline_color.rgb, cel.outline_color.a * edge);
}
