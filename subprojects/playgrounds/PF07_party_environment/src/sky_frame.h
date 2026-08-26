#ifndef DEVILS_ENGINE_PF07_SKY_FRAME_H
#define DEVILS_ENGINE_PF07_SKY_FRAME_H

// Перевод состояния неба в блок для шейдера. Вынесено отдельно от рендера, потому что это чистая
// функция без Vulkan: её можно проверить численно, и именно здесь живёт единственная смена системы
// координат — небесный модуль работает в ENU (восток, север, зенит), мир рендера имеет Y вверх.

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "celestial.h"

namespace devils_engine::pf07 {

constexpr size_t sky_star_count = 2;
constexpr size_t sky_moon_capacity = 4;

// Раскладка обязана совпадать с `pf07_sky_block` в resources/shaders/pf07_records.glsl.
struct alignas(16) sky_gpu_block {
  glm::vec4 star_direction[sky_star_count];
  glm::vec4 star_color_illuminance[sky_star_count];
  glm::vec4 moon_direction[sky_moon_capacity];
  glm::vec4 moon_color_illuminance[sky_moon_capacity];
  glm::vec4 moon_phase[sky_moon_capacity];
  glm::vec4 atmosphere_geometry;
  glm::vec4 atmosphere_medium;
  glm::vec4 march_params;
  glm::vec4 output_params;
  // Базис горизонта в инерциальной системе: звёздное поле закреплено за небом, а не за наблюдателем,
  // и обязано вращаться вместе с планетой.
  glm::vec4 sky_basis_east;
  glm::vec4 sky_basis_north;
  glm::vec4 sky_basis_up;
  // x — множитель размера рисуемых дисков, y — плотность звёзд, z — порог яркости звёзд, w — резерв.
  glm::vec4 presentation_params;
};
static_assert(sizeof(sky_gpu_block) == 384);

struct atmosphere_settings {
  double height_km = 100.0;         // верх атмосферы над поверхностью
  double rayleigh_scale_km = 8.0;
  double mie_scale_km = 1.2;
  double ozone_center_km = 25.0;
  double ozone_width_km = 15.0;
  double mie_anisotropy = 0.76;
  double turbidity = 1.0;           // множитель аэрозоля; позже сюда придёт погодный вектор
  double ground_albedo = 0.10;
};

struct march_settings {
  double camera_height_km = 0.002;
  int32_t primary_steps = 32;
  int32_t light_steps = 8;
};

struct output_settings {
  double exposure = 1.0e-4;
  double debug_mode = 0.0;
  // Диски светил и лун физически малы: у Selen 0.90°, что при поле зрения 65° занимает десяток
  // пикселей. Это осознанное преувеличение для читаемости, и оно НЕ трогает освещённость: диск
  // становится крупнее, но света от него в сцене столько же. Угловые радиусы, которыми считаются
  // затмения, живут в небесной механике и этим множителем не затрагиваются.
  double disc_scale = 3.0;
  double star_density = 1.0;
  double star_brightness = 1.0;
  double galaxy_brightness = 1.0;
  // Доля физической скорости вращения звёздного неба. Сутки здесь идут за двадцать четыре реальные
  // минуты, поэтому честное вращение выглядит вертолётом; единица возвращает физику, ноль полностью
  // останавливает небо. Луны и светила при этом продолжают идти по-настоящему.
  double star_rotation_scale = 0.15;
};

// Направление из локальной горизонтальной системы (x восток, y север, z зенит) в мир рендера
// (x восток, y вверх, z на юг).
glm::vec3 horizon_to_world(const glm::dvec3& horizon_direction);

// `star_frame` даёт базис горизонта для звёздного поля. Обычно это то же состояние, но при замедленном
// вращении неба сюда приходит состояние, посчитанное на замедленное время.
sky_gpu_block pack_sky_block(const sky_state& state, const sky_state& star_frame,
                             const atmosphere_settings& atmosphere, const march_settings& march,
                             const output_settings& output, const double planet_radius_km,
                             const std::vector<moon_config>& moons);

} // namespace devils_engine::pf07

#endif
