#include "shadows.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "sky_frame.h"

namespace devils_engine::pf07 {
namespace {

// Порог вхождения в двойку теневых источников, в люксах.
//
// Ниже него тень не рисуется вовсе, а на подходе к нему её сила гасится плавно. Величина выбрана по
// физике, а не на глаз: полная луна даёт около 0.3 лк на горизонтальную площадку, и её тень человек
// видит. Порог поставлен на порядок ниже, чтобы луна в четверти фазы ещё отбрасывала тень, но чтобы
// щелчок при смене состава двойки приходился на источник, чья тень и так неразличима.
constexpr double shadow_threshold_lx = 0.03;
constexpr double shadow_fade_span = 4.0;   // во сколько раз выше порога тень набирает полную силу

// Обратная Z-ортография. Дальняя плоскость отображается в ноль, ближняя в единицу — тот же порядок,
// что у основной камеры, поэтому сравнение глубины в шейдере не приходится разворачивать.
glm::mat4 reverse_z_ortho(const float left, const float right, const float bottom, const float top,
                          const float near_plane, const float far_plane) {
  glm::mat4 out(1.0f);
  out[0][0] = 2.0f / (right - left);
  out[1][1] = 2.0f / (top - bottom);
  out[2][2] = 1.0f / (far_plane - near_plane);
  out[3][0] = -(right + left) / (right - left);
  out[3][1] = -(top + bottom) / (top - bottom);
  out[3][2] = far_plane / (far_plane - near_plane);
  return out;
}

double fade_strength(const double illuminance_lx) {
  if (illuminance_lx <= shadow_threshold_lx) return 0.0;
  const double ratio = illuminance_lx / (shadow_threshold_lx * shadow_fade_span);
  return std::clamp(ratio, 0.0, 1.0);
}

void consider(const body_view& body, const float body_code, shadow_source_set& out) {
  // Тело под горизонтом теней не отбрасывает. Здесь берётся ОБЫЧНАЯ освещённость, с множителем
  // горизонта: в отличие от атмосферы, у поверхности сцены горизонт ровно один — её собственный.
  if (body.altitude_deg <= 0.0 || body.illuminance_lx <= shadow_threshold_lx) return;

  shadow_source candidate{};
  candidate.direction = horizon_to_world(body.direction);
  candidate.color = glm::vec3(float(body.color_linear.x), float(body.color_linear.y),
                              float(body.color_linear.z));
  candidate.illuminance_lx = body.illuminance_lx;
  candidate.strength = fade_strength(body.illuminance_lx);
  candidate.name = body.name;
  candidate.body_code = body_code;
  candidate.active = true;

  // Вставка с сохранением убывающего порядка. Источников всего пять, поэтому сортировка избыточна.
  for (auto& slot : out) {
    if (candidate.illuminance_lx > slot.illuminance_lx) {
      std::swap(slot, candidate);
    }
  }
}

} // namespace

shadow_source_set select_shadow_sources(const sky_state& state) {
  shadow_source_set out{};
  for (size_t i = 0; i < state.stars.size(); ++i) consider(state.stars[i], float(i), out);
  for (size_t m = 0; m < state.moons.size(); ++m) {
    consider(state.moons[m], float(state.stars.size() + m), out);
  }
  return out;
}

void build_cascades(const shadow_source& source, const playground::free_camera& camera, const float aspect,
                    const float vertical_fov, const float camera_near, const float shadow_far,
                    const float caster_height, const float split_lambda,
                    const std::span<const painter::atlas_region> regions,
                    const uint32_t atlas_width, const uint32_t atlas_height,
                    const std::span<cascade_record> out) {
  // Разбиение по практической схеме: логарифмическое даёт верную плотность текселей у камеры,
  // равномерное не вырождает дальний каскад, доля между ними приходит снаружи.
  constexpr float blend_fraction = 0.12f;
  // Запас по направлению света: приёмник виден в срезе камеры, а кастер может стоять снаружи и всё
  // равно бросать в него тень. Без запаса тени обрываются на границе среза.
  //
  // Запас обязан РАСТИ у горизонта, и постоянная величина здесь — скрытая ошибка. Чтобы кастер высотой
  // h попал в карту, вдоль луча света нужно h / sin(высоты светила): при 30° это шестнадцать метров,
  // при 5° — девяносто два, при 1° — четыреста пятьдесят. Зашитые двадцать метров верны ровно до
  // двадцати трёх градусов, а ниже молча теряют дальнюю часть каждой тени.
  //
  // Пол по синусу ограничивает запас снизу: у самого горизонта формула уходит в бесконечность, а свет
  // там всё равно скользит по поверхности и прямого вклада почти не даёт.
  const float elevation_sine = std::max(source.direction.y, 0.02f);
  const float caster_depth_margin = std::clamp(caster_height / elevation_sine, 20.0f, 1500.0f);

  std::array<float, cascade_count> split_fars{};
  for (uint32_t index = 0; index < cascade_count; ++index) {
    const float fraction = float(index + 1) / float(cascade_count);
    const float logarithmic = camera_near * std::pow(shadow_far / camera_near, fraction);
    const float uniform = camera_near + (shadow_far - camera_near) * fraction;
    split_fars[index] = logarithmic * split_lambda + uniform * (1.0f - split_lambda);
  }

  const glm::vec3 forward = camera.forward();
  const glm::vec3 right = camera.right();
  const glm::vec3 up = glm::normalize(glm::cross(right, forward));
  // Базис света берётся без переноса: ортографии важен только поворот.
  const glm::vec3 reference = std::abs(source.direction.y) > 0.95f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                   : glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::mat4 light_view =
    glm::mat4(glm::mat3(glm::lookAtRH(source.direction, glm::vec3(0.0f), reference)));
  const float tangent = std::tan(vertical_fov * 0.5f);

  float split_near = camera_near;
  for (uint32_t index = 0; index < cascade_count; ++index) {
    const float split_far = split_fars[index];
    std::array<glm::vec3, 8> corners{};
    uint32_t corner = 0;
    for (const float distance : {split_near, split_far}) {
      const glm::vec3 centre = camera.position + forward * distance;
      const float half_height = tangent * distance;
      const float half_width = half_height * aspect;
      for (const float vertical : {-1.0f, 1.0f}) {
        for (const float horizontal : {-1.0f, 1.0f}) {
          corners[corner++] = centre + right * (horizontal * half_width) + up * (vertical * half_height);
        }
      }
    }

    glm::vec3 centre{0.0f};
    for (const auto point : corners) centre += point;
    centre /= float(corners.size());
    float radius = 0.0f;
    for (const auto point : corners) radius = std::max(radius, glm::length(point - centre));
    // Сфера вместо коробки и квантованный радиус: и то и другое против ДЫШАНИЯ проекции при
    // повороте камеры внутри одного интервала. Коробка меняет размер от поворота, сфера — нет.
    radius = std::ceil(radius * 16.0f) / 16.0f;

    const glm::vec3 centre_light = glm::vec3(light_view * glm::vec4(centre, 1.0f));
    // Привязка к текселю. Без неё тень ползёт по краю при движении камеры: проекция сдвигается на
    // долю текселя, и граница тени перескакивает между соседними текселями карты.
    const float texel_world = (radius * 2.0f) / float(regions[index].size);
    const float snapped_x = std::round(centre_light.x / texel_world) * texel_world;
    const float snapped_y = std::round(centre_light.y / texel_world) * texel_world;

    float minimum_z = 1.0e30f;
    float maximum_z = -1.0e30f;
    for (const auto point : corners) {
      const float light_z = (light_view * glm::vec4(point, 1.0f)).z;
      minimum_z = std::min(minimum_z, light_z);
      maximum_z = std::max(maximum_z, light_z);
    }

    const glm::mat4 projection =
      reverse_z_ortho(snapped_x - radius, snapped_x + radius, snapped_y - radius, snapped_y + radius,
                      -maximum_z - caster_depth_margin, -minimum_z + caster_depth_margin);

    auto& record = out[index];
    record.light_view_projection = projection * light_view;
    record.split_depths =
      glm::vec4(split_near, split_far, split_far - (split_far - split_near) * blend_fraction, 0.0f);
    record.shadow_params = glm::vec4(texel_world, float(source.strength), 0.0f, 0.0f);
    const auto uv = painter::atlas_region_uv(regions[index], atlas_width, atlas_height);
    record.uv_scale_offset = glm::vec4(uv.scale_x, uv.scale_y, uv.offset_x, uv.offset_y);
    split_near = split_far;
  }
}

} // namespace devils_engine::pf07
