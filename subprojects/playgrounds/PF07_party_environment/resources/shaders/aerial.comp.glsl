#version 450

#include "pf07_atmosphere.glsl"

// Таблица воздушной перспективы: свет, набранный воздухом между камерой и точкой сцены, и доля света
// самой точки, дошедшая до глаза. Трёхмерная сетка по кадру: две оси экранные, третья — расстояние.
//
// Зачем она нужна помимо скорости. Небо у нас уже есть, но небо — это луч, ушедший в космос. Всё, что
// стоит НА расстоянии, воздух тоже красит: дальний склон синеет и теряет контраст не потому, что он
// такой, а потому что между ним и глазом висит рассеивающая среда. Без этого слагаемого любая
// геометрия выглядит наклеенной поверх неба, каким бы верным само небо ни было.
//
// Её потребляет весь проход сцены: земля, рельеф, предметы и заросли получают одну и ту же воздушную
// перспективу. Таблица ничего не знает про источник глубины и работает с любой геометрией.
//
// Ось расстояния квадратичная: у камеры срезы густые, вдали редкие. Это не украшение — рассеяние на
// первых сотнях метров меняется быстрее всего, а на последних километрах почти постоянно.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 0, binding = 1, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

layout(set = 0, binding = 2) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 3) uniform sampler2D multiscatter_lut;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image3D aerial_image;

const int pf07_aerial_steps_per_slice = 3;

void main() {
  const ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
  const ivec3 size = imageSize(aerial_image);
  if (cell.x >= size.x || cell.y >= size.y) return;

  const float ground_radius = sky_data.sky.atmosphere_geometry.x;
  const float camera_height = sky_data.sky.march_params.x;
  const float max_range = sky_data.sky.march_params.z;

  // Направление луча строится ровно тем же способом, что во фрагментном шейдере: тангенс полу-FOV
  // берётся из камеры, а не из матрицы вида. Разойтись им нельзя, иначе таблица ляжет на кадр со сдвигом.
  const vec2 uv = (vec2(cell) + 0.5) / vec2(size.xy);
  const vec2 ndc = uv * 2.0 - 1.0;
  const float aspect = camera_data.viewport_near.x / max(camera_data.viewport_near.y, 1.0);
  const float tan_half_fov = camera_data.viewport_near.w;
  const mat3 camera_to_world = transpose(mat3(camera_data.view));
  const vec3 direction =
    normalize(camera_to_world * vec3(ndc.x * aspect * tan_half_fov, -ndc.y * tan_half_fov, -1.0));

  const vec3 origin = vec3(0.0, ground_radius + camera_height, 0.0);

  // Марш идёт НАРАСТАЮЩИМ итогом: один проход вдоль луча, и каждый срез записывается по дороге. Считать
  // каждый срез отдельно от камеры означало бы квадратичную работу вместо линейной.
  vec3 in_scattering = vec3(0.0);
  vec3 transmittance = vec3(1.0);
  float previous_distance = 0.0;

  for (int slice = 0; slice < size.z; ++slice) {
    const float normalized = (float(slice) + 1.0) / float(size.z);
    const float slice_distance = max_range * normalized * normalized;
    const float segment = slice_distance - previous_distance;
    previous_distance = slice_distance;

    if (segment > 0.0) {
      vec3 segment_transmittance;
      const vec3 segment_scattering =
        pf07_march_scattering(sky_data.sky, transmittance_lut, multiscatter_lut,
                              origin + direction * (slice_distance - segment), direction, segment,
                              pf07_aerial_steps_per_slice, segment_transmittance);
      in_scattering += transmittance * segment_scattering;
      transmittance *= segment_transmittance;
    }

    const float transmittance_luma = dot(transmittance, vec3(0.2126, 0.7152, 0.0722));
    imageStore(aerial_image, ivec3(cell, slice), vec4(in_scattering, transmittance_luma));
  }
}
