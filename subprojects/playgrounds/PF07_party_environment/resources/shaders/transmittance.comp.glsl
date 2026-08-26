#version 450

#include "pf07_atmosphere.glsl"

// Таблица прохождения атмосферы: доля света, доходящая от точки на радиусе r до верхней границы вдоль
// луча с косинусом зенитного угла mu.
//
// Смысл её появления чисто измеренный. Прямой марш считал прохождение к светилу вложенным циклом:
// восемь выборок среды на каждую из тридцати двух точек основного марша, на каждое светило — 512
// выборок на пиксель. Пока светило под горизонтом, внутренний цикл выходил сразу и кадр стоил 8 мс;
// стоило светилу подняться, тот же кадр стоил 36 мс. Таблица считается один раз за кадр на 256x64
// отсчёта и превращает внутренний цикл в одну выборку из текстуры.
//
// Зависит только от параметров среды, поэтому пересчёт каждый кадр стоит копейки и автоматически
// подхватывает смену мутности — будущий погодный вектор.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D transmittance_image;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(transmittance_image);
  if (pixel.x >= size.x || pixel.y >= size.y) return;

  const float ground_radius = sky_data.sky.atmosphere_geometry.x;
  const float top_radius = sky_data.sky.atmosphere_geometry.y;
  const vec2 texel_count = vec2(size);
  const vec2 uv = (vec2(pixel) + 0.5) / texel_count;

  float radius;
  float mu;
  pf07_transmittance_from_uv(uv, ground_radius, top_radius, texel_count, radius, mu);

  // Длина луча до верхней границы. Луч, упирающийся в планету, в таблицу не попадает по построению
  // параметризации: такие пары (r, mu) в неё не отображаются.
  const float distance_to_top =
    max(0.0, -radius * mu + sqrt(max(0.0, radius * radius * (mu * mu - 1.0) + top_radius * top_radius)));

  // Сорок шагов — не роскошь: у горизонта луч идёт сквозь всю толщу, и на двадцати шагах у самой
  // границы появляется заметная ступенька яркости неба.
  const int steps = 40;
  const float step_length = distance_to_top / float(steps);
  vec3 optical_depth = vec3(0.0);
  for (int i = 0; i < steps; ++i) {
    const float distance = (float(i) + 0.5) * step_length;
    // Высота точки на луче через теорему косинусов: считать её через явные координаты незачем.
    const float sample_radius =
      sqrt(max(0.0, distance * distance + 2.0 * radius * mu * distance + radius * radius));
    optical_depth += pf07_sample_medium(sky_data.sky, sample_radius - ground_radius).extinction * step_length;
  }

  imageStore(transmittance_image, pixel, vec4(exp(-optical_depth), 1.0));
}
