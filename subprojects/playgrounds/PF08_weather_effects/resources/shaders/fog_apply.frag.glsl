#version 450

#include "pf08_records.glsl"

// Локальная среда композится в HDR ДО экспонометра: L = L_scene*T + S. Иначе автоэкспозиция
// измеряла бы мир без тумана и затем получала другое изображение, то есть сама создавала бы вспышку.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform sampler3D fog_volume;
layout(set = 0, binding = 3, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;
layout(set = 0, binding = 4, std140) uniform SkyBlock {
  pf08_sky_block sky;
} sky_data;

void main() {
  const vec3 radiance = texture(scene_color, in_uv).rgb;
  // Это не только ранний выход: clear обязан остаться ПОБИТНО тем же baseline. Умножение на единицу
  // и сложение нуля математически тождественны, но лишняя half-float выборка не обязана быть побитной.
  if (sky_data.sky.fog_params.x <= 0.0) {
    out_color = vec4(radiance, 1.0);
    return;
  }

  const vec2 ndc = in_uv * 2.0 - 1.0;
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const float tan_half_fov = camera_data.viewport_near.w;
  const vec3 view_ray = normalize(vec3(ndc.x * aspect * tan_half_fov,
                                        -ndc.y * tan_half_fov, -1.0));
  const float depth = texture(scene_depth, in_uv).r;
  const float max_range = max(sky_data.sky.fog_params.w, 1e-3);
  const float distance = depth > 0.0
    ? min(max_range, camera_data.viewport_near.z / max(depth * -view_ray.z, 1e-7))
    : max_range;

  const int slice_count = textureSize(fog_volume, 0).z;
  const float continuous_index = sqrt(clamp(distance / max_range, 0.0, 1.0)) * float(slice_count - 1);
  const float z = (continuous_index + 0.5) / float(slice_count);
  const vec4 fog = texture(fog_volume, vec3(in_uv, z));
  // Debug 8 показывает физическую долю прохождения напрямую. На небе глубины нет, поэтому там
  // обязано получиться exp(-extinction * range); это позволяет проверить GPU-интеграл числом.
  if (sky_data.sky.output_params.w >= 7.5 && sky_data.sky.output_params.w < 8.5) {
    out_color = vec4(vec3(fog.a), 1.0);
    return;
  }
  out_color = vec4(min(radiance * fog.a + fog.rgb, vec3(60000.0)), 1.0);
}
