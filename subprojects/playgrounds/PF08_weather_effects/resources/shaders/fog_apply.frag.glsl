#version 450

#include "pf08_clouds.glsl"
#include "pf08_precipitation.glsl"

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

float pf08_lightning_hash(const float value) {
  return fract(sin(value * 91.713 + 17.19) * 43758.5453);
}

vec3 pf08_lightning_path_node(const float t, const vec3 start, const vec3 end,
                              const vec3 side, const vec3 cross_side, const float seed) {
  const vec3 axis = end - start;
  const float length_m = length(axis);
  const float envelope = sin(3.14159265358979323846 * t);
  const float index = t * 12.0;
  const float lateral = pf08_lightning_hash(seed + index * 13.7) * 2.0 - 1.0;
  const float lateral2 = pf08_lightning_hash(seed * 1.71 + index * 29.3) * 2.0 - 1.0;
  return mix(start, end, t) + envelope * length_m * 0.022 *
    (side * lateral + cross_side * lateral2 * 0.55);
}

float pf08_distance_to_segment(const vec2 point, const vec2 start, const vec2 end) {
  const vec2 segment = end - start;
  const float length_squared = dot(segment, segment);
  const float along = length_squared > 1e-5
    ? clamp(dot(point - start, segment) / length_squared, 0.0, 1.0) : 0.0;
  return length(point - (start + segment * along));
}

bool pf08_project_lightning_node(const vec3 world_position, out vec2 pixel_position,
                                 out float view_distance) {
  const vec4 clip = camera_data.view_projection * vec4(world_position, 1.0);
  if (clip.w <= 1e-4) return false;
  const vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
  pixel_position = uv * camera_data.viewport_near.xy;
  view_distance = length(world_position - camera_data.camera_position.xyz);
  return true;
}

// Возвращает HDR radiance канала и его расстояние. Дальний физический канал не расширяется до
// обязательного пикселя: ниже 0.75 px consumer выключается, а облачная вспышка продолжает жить во froxel.
vec3 pf08_lightning_channel_radiance(const float scene_distance, const float volume_range,
                                     const bool medium_active, out float channel_distance) {
  const pf08_sky_block sky = sky_data.sky;
  const float channel = max(sky.lightning_start_channel.w, 0.0);
  const float luminance = max(sky.lightning_shape.y, 0.0);
  channel_distance = 0.0;
  if (channel <= 1e-5 || luminance <= 0.0) return vec3(0.0);

  const vec3 start = sky.lightning_start_channel.xyz;
  const vec3 end = sky.lightning_end_flash.xyz;
  const vec3 midpoint = (start + end) * 0.5;
  const float midpoint_distance = length(midpoint - camera_data.camera_position.xyz);
  const float vertical_fov = 2.0 * atan(camera_data.viewport_near.w);
  const float width_pixels = 2.0 * atan(sky.lightning_shape.x / max(midpoint_distance, 1e-4)) /
                             max(vertical_fov, 1e-4) * camera_data.viewport_near.y;
  if (width_pixels < 0.75) return vec3(0.0);

  const vec3 axis = normalize(end - start);
  const vec3 reference = abs(axis.y) < 0.92 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  const vec3 side = normalize(cross(axis, reference));
  const vec3 cross_side = normalize(cross(axis, side));
  const float seed = sky.lightning_shape.w;
  const vec2 pixel = gl_FragCoord.xy;
  vec2 bound_start;
  vec2 bound_end;
  float bound_start_distance;
  float bound_end_distance;
  if (!pf08_project_lightning_node(start, bound_start, bound_start_distance) ||
      !pf08_project_lightning_node(end, bound_end, bound_end_distance)) return vec3(0.0);
  // Fullscreen pass нужен для корректной композиции с уже готовой глубиной/средой, но дорогую ломаную
  // считают только фрагменты её консервативного screen-space прямоугольника. Запас покрывает jitter,
  // три ветви и bloom; дальний sub-pixel gate срабатывает ещё раньше.
  const float bounds_padding = max(18.0, length(bound_end - bound_start) * 0.22);
  const vec2 bounds_minimum = min(bound_start, bound_end) - bounds_padding;
  const vec2 bounds_maximum = max(bound_start, bound_end) + bounds_padding;
  if (any(lessThan(pixel, bounds_minimum)) || any(greaterThan(pixel, bounds_maximum))) {
    return vec3(0.0);
  }
  float nearest_pixels = 1e20;
  float nearest_distance = midpoint_distance;

  const int segment_count = 12;
  for (int i = 0; i < segment_count; ++i) {
    const float t0 = float(i) / float(segment_count);
    const float t1 = float(i + 1) / float(segment_count);
    const vec3 world0 = pf08_lightning_path_node(t0, start, end, side, cross_side, seed);
    const vec3 world1 = pf08_lightning_path_node(t1, start, end, side, cross_side, seed);
    vec2 pixel0;
    vec2 pixel1;
    float distance0;
    float distance1;
    if (!pf08_project_lightning_node(world0, pixel0, distance0) ||
        !pf08_project_lightning_node(world1, pixel1, distance1)) continue;
    const float distance_pixels = pf08_distance_to_segment(pixel, pixel0, pixel1);
    if (distance_pixels < nearest_pixels) {
      nearest_pixels = distance_pixels;
      nearest_distance = 0.5 * (distance0 + distance1);
    }

    // Три дешёвые боковые ветви принадлежат тому же пути. Они не являются отдельными источниками:
    // энергия облачной/поверхностной вспышки всё ещё вычисляется от основного world segment.
    if (i == 3 || i == 6 || i == 8) {
      const float branch_sign = pf08_lightning_hash(seed + float(i) * 47.0) < 0.5 ? -1.0 : 1.0;
      const float main_length = length(end - start);
      const vec3 branch_end = world1 + axis * main_length * 0.10 +
        side * branch_sign * main_length * (0.07 + 0.025 * float(i == 6)) +
        cross_side * main_length * (pf08_lightning_hash(seed + float(i) * 71.0) - 0.5) * 0.055;
      vec2 branch_pixel;
      float branch_distance;
      if (pf08_project_lightning_node(branch_end, branch_pixel, branch_distance)) {
        const float branch_pixels = pf08_distance_to_segment(pixel, pixel1, branch_pixel) / 0.58;
        if (branch_pixels < nearest_pixels) {
          nearest_pixels = branch_pixels;
          nearest_distance = 0.5 * (distance1 + branch_distance);
        }
      }
    }
  }

  channel_distance = nearest_distance;
  if (nearest_distance > scene_distance + 0.25 || nearest_pixels > max(14.0, width_pixels * 6.0)) {
    return vec3(0.0);
  }
  const float core = 1.0 - smoothstep(max(width_pixels * 0.35, 0.35),
                                      max(width_pixels * 0.35, 0.35) + 1.0, nearest_pixels);
  const float bloom_width = max(2.2, width_pixels * 2.4);
  const float bloom = exp2(-nearest_pixels * nearest_pixels / (bloom_width * bloom_width));
  float medium_transmittance = 1.0;
  if (medium_active && nearest_distance < volume_range) {
    const int slice_count = textureSize(fog_volume, 0).z;
    const float continuous_index = sqrt(clamp(nearest_distance / volume_range, 0.0, 1.0)) *
                                   float(slice_count - 1);
    medium_transmittance = texture(fog_volume,
      vec3(in_uv, (continuous_index + 0.5) / float(slice_count))).a;
  }
  return sky.lightning_colour_intensity.rgb * luminance * channel *
         (core + bloom * 0.075) * medium_transmittance;
}

void main() {
  const vec3 radiance = texture(scene_color, in_uv).rgb;
  // Это не только ранний выход: clear обязан остаться ПОБИТНО тем же baseline. Умножение на единицу
  // и сложение нуля математически тождественны, но лишняя half-float выборка не обязана быть побитной.
  const bool fog_active = sky_data.sky.fog_params.x > 0.0;
  const bool cloud_active = sky_data.sky.cloud_params.x > 0.0 && sky_data.sky.cloud_params.y > 0.0;
  const bool rain_active = pf08_rain_active(sky_data.sky) && sky_data.sky.precipitation_shape.y > 0.0;
  const bool snow_active = pf08_snow_active(sky_data.sky) && sky_data.sky.snow_shape.y > 0.0;
  const bool medium_active = fog_active || cloud_active || rain_active || snow_active;
  const bool channel_active = sky_data.sky.lightning_start_channel.w > 1e-5 &&
                              sky_data.sky.lightning_shape.y > 0.0;
  if (!medium_active && !channel_active) {
    out_color = vec4(radiance, 1.0);
    return;
  }

  const vec2 ndc = in_uv * 2.0 - 1.0;
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const float tan_half_fov = camera_data.viewport_near.w;
  const vec3 view_ray = normalize(vec3(ndc.x * aspect * tan_half_fov,
                                        -ndc.y * tan_half_fov, -1.0));
  const float depth = texture(scene_depth, in_uv).r;
  const float max_range = max(pf08_weather_volume_range(sky_data.sky), 1e-3);
  const float scene_distance = depth > 0.0
    ? camera_data.viewport_near.z / max(depth * -view_ray.z, 1e-7) : 1e20;
  const float distance = min(max_range, scene_distance);

  const int slice_count = textureSize(fog_volume, 0).z;
  const float continuous_index = sqrt(clamp(distance / max_range, 0.0, 1.0)) * float(slice_count - 1);
  const float z = (continuous_index + 0.5) / float(slice_count);
  const vec4 fog = medium_active ? texture(fog_volume, vec3(in_uv, z)) : vec4(0.0, 0.0, 0.0, 1.0);
  // Debug 8 показывает физическую долю прохождения напрямую. На небе глубины нет, поэтому там
  // обязано получиться exp(-extinction * range); это позволяет проверить GPU-интеграл числом.
  if (sky_data.sky.output_params.w >= 7.5 && sky_data.sky.output_params.w < 8.5) {
    out_color = vec4(vec3(fog.a), 1.0);
    return;
  }
  // Debug 9 показывает сам world-space cloud field в середине слоя. В отличие от итогового цвета,
  // здесь движение и coverage можно проверять без влияния экспозиции и освещения.
  if (sky_data.sky.output_params.w >= 8.5 && sky_data.sky.output_params.w < 9.5) {
    float cloud_density = 0.0;
    const vec3 world_direction = normalize(transpose(mat3(camera_data.view)) * view_ray);
    if (cloud_active && world_direction.y > 1e-4) {
      const float middle_height = 0.5 * (sky_data.sky.cloud_shape.x + sky_data.sky.cloud_shape.y);
      const float ray_distance = (middle_height - camera_data.camera_position.y) / world_direction.y;
      if (ray_distance > 0.0 && ray_distance <= max_range) {
        const vec3 world_position = camera_data.camera_position.xyz + world_direction * ray_distance;
        cloud_density = pf08_cloud_horizontal_density(sky_data.sky, world_position.xz);
      }
    }
    out_color = vec4(vec3(cloud_density), 1.0);
    return;
  }
  // Debug 10 изолирует облачную тень на геометрии. Карта теней здесь не участвует: значение —
  // только Beer-Lambert transmittance облачного столба к главному светилу.
  if (sky_data.sky.output_params.w >= 9.5 && sky_data.sky.output_params.w < 10.5) {
    float cloud_shadow = 0.0;
    if (depth > 0.0) {
      const vec3 world_direction = normalize(transpose(mat3(camera_data.view)) * view_ray);
      const vec3 world_position = camera_data.camera_position.xyz + world_direction * distance;
      cloud_shadow = pf08_cloud_light_transmittance(
        sky_data.sky, world_position, sky_data.sky.star_direction[0].xyz);
    }
    out_color = vec4(vec3(cloud_shadow), 1.0);
    return;
  }
  float channel_distance;
  const vec3 channel_radiance = pf08_lightning_channel_radiance(
    scene_distance, max_range, medium_active, channel_distance);
  out_color = vec4(min(radiance * fog.a + fog.rgb + channel_radiance, vec3(60000.0)), 1.0);
}
