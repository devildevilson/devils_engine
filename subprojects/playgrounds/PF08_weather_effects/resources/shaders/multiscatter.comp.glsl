#version 450

#include "pf08_atmosphere.glsl"

// Таблица многократного рассеяния.
//
// До неё небо считало только ОДНОкратное рассеяние: фотон, свернувший в глаз наблюдателя после
// единственного столкновения. Весь свет, свернувший дважды и больше, отсутствовал, и именно поэтому
// небо вдали от светила выходило заметно темнее и серее настоящего, а сумеречный зенит проваливался
// в черноту. В синем канале доля многократного рассеяния у поверхности доходит до половины: там, где
// оптическая толща порядка единицы, «рассеялся один раз» перестаёт быть хорошим приближением.
//
// Способ — Hillaire 2020. Точное решение потребовало бы четырёхмерной таблицы и итераций по порядкам
// рассеяния; вместо этого делаются два предположения: рассеяние высоких порядков считается изотропным,
// а среда вокруг точки — однородной. Тогда сумма всех порядков сворачивается в геометрическую прогрессию
// и берётся в замкнутом виде: нужно знать только свет ВТОРОГО порядка и долю света, которая рассеется
// ещё раз. Двумерной таблицы «высота × зенитный угол светила» для этого достаточно.
//
// Двухзвёздность и здесь ничего не стоит: таблица зависит от направления на светило, поэтому
// вычисляется один раз, а читается по разу на каждое светило.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform SkyBlock {
  pf08_sky_block sky;
} sky_data;

layout(set = 0, binding = 1) uniform sampler2D transmittance_lut;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D multiscatter_image;

const int pf08_multiscatter_directions = 64;
const int pf08_multiscatter_steps = 20;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(multiscatter_image);
  if (pixel.x >= size.x || pixel.y >= size.y) return;

  const float ground_radius = sky_data.sky.atmosphere_geometry.x;
  const float top_radius = sky_data.sky.atmosphere_geometry.y;
  const float ground_albedo = sky_data.sky.output_params.z;

  const vec2 texel_count = vec2(size);
  const vec2 uv = (vec2(pixel) + 0.5) / texel_count;

  float radius;
  float mu_sun;
  pf08_multiscatter_from_uv(uv, ground_radius, top_radius, texel_count, radius, mu_sun);

  // Опорная система: наблюдатель на оси Y, светило в плоскости YZ. Ориентация произвольна, потому что
  // задача симметрична относительно поворота вокруг направления «вверх».
  const vec3 position = vec3(0.0, radius, 0.0);
  const vec3 sun_direction = vec3(0.0, mu_sun, sqrt(max(0.0, 1.0 - mu_sun * mu_sun)));

  vec3 second_order = vec3(0.0);
  vec3 scatter_fraction = vec3(0.0);

  // Равномерная выборка сферы 8x8. Нормировка вынесена за цикл, см. комментарий ниже.
  for (int index = 0; index < pf08_multiscatter_directions; ++index) {
    const float u1 = (float(index / 8) + 0.5) / 8.0;
    const float u2 = (float(index % 8) + 0.5) / 8.0;
    const float cosine = 1.0 - 2.0 * u1;
    const float sine = sqrt(max(0.0, 1.0 - cosine * cosine));
    const float azimuth = 2.0 * pf08_pi * u2;
    const vec3 direction = vec3(sine * cos(azimuth), cosine, sine * sin(azimuth));

    const float ground_hit = pf08_sphere_hit(position, direction, ground_radius);
    const float top_hit = pf08_sphere_hit(position, direction, top_radius);
    const float march_length = ground_hit > 0.0 ? ground_hit : top_hit;
    if (march_length <= 0.0) continue;

    const float step_length = march_length / float(pf08_multiscatter_steps);
    vec3 throughput = vec3(1.0);
    for (int i = 0; i < pf08_multiscatter_steps; ++i) {
      const vec3 point = position + direction * ((float(i) + 0.5) * step_length);
      const pf08_medium medium = pf08_sample_medium(sky_data.sky, length(point) - ground_radius);
      const vec3 step_transmittance = exp(-medium.extinction * step_length);
      const vec3 safe_extinction = max(medium.extinction, vec3(1e-9));

      const vec3 scattering = medium.scattering_rayleigh + vec3(medium.scattering_mie);
      const vec3 sun_transmittance =
        pf08_transmittance_to_light(sky_data.sky, transmittance_lut, point, sun_direction);

      // Свет второго порядка: пришёл от светила, рассеялся здесь один раз изотропно.
      const vec3 source = scattering * sun_transmittance;
      second_order += throughput * (source - source * step_transmittance) / safe_extinction;
      // Доля света, которая рассеется ещё раз. Тот же интеграл, но без множителя от светила: именно
      // она и сворачивает бесконечный ряд порядков в 1/(1 - f).
      scatter_fraction += throughput * (scattering - scattering * step_transmittance) / safe_extinction;

      throughput *= step_transmittance;
    }

    if (ground_hit > 0.0) {
      // Отражение от поверхности — полноценный источник рассеяния, а не мелочь: над светлой землёй
      // небо у горизонта заметно светлее, и без этого слагаемого разница исчезает.
      const vec3 ground_point = position + direction * ground_hit;
      const vec3 normal = normalize(ground_point);
      const float cosine_sun = max(0.0, dot(normal, sun_direction));
      const vec3 sun_transmittance =
        pf08_transmittance_to_light(sky_data.sky, transmittance_lut, ground_point, sun_direction);
      second_order += throughput * ground_albedo * sun_transmittance * cosine_sun / pf08_pi;
      scatter_fraction += throughput * ground_albedo;
    }
  }

  // Здесь два разных множителя, и путать их нельзя.
  //
  // `second_order` — ЯРКОСТЬ, и она получает изотропную фазу 1/(4pi): свет, пришедший с каждого
  // направления, сам был рассеян в это направление изотропно. Телесный угол выборки 4pi/N сокращается
  // с усреднением, а вот фаза не сокращается ни с чем, и без неё добавка выходит в 4pi раз больше —
  // небо удваивалось по яркости вместо разумных процентов.
  //
  // `scatter_fraction` — ВЕРОЯТНОСТЬ, доля света, которая рассеется ещё раз. Она безразмерна, фазы не
  // несёт и остаётся простым средним по направлениям.
  second_order /= float(pf08_multiscatter_directions) * 4.0 * pf08_pi;
  scatter_fraction /= float(pf08_multiscatter_directions);

  // Геометрическая сумма всех порядков рассеяния. Знаменатель не может обратиться в ноль физически
  // (часть света всегда поглощается), но у самой поверхности со светлым альбедо подходит к нему близко,
  // поэтому ограничение обязательно.
  const vec3 total = second_order / max(vec3(1.0) - scatter_fraction, vec3(1e-3));
  imageStore(multiscatter_image, pixel, vec4(total, 1.0));
}
