#include "sky_frame.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devils_engine::pf07 {
namespace {

constexpr double pi = std::numbers::pi;

double to_radians(const double degrees) {
  return degrees * pi / 180.0;
}

} // namespace

glm::vec3 horizon_to_world(const glm::dvec3& horizon_direction) {
  return glm::vec3(static_cast<float>(horizon_direction.x), static_cast<float>(horizon_direction.z),
                   static_cast<float>(-horizon_direction.y));
}

sky_gpu_block pack_sky_block(const sky_state& state, const sky_state& star_frame,
                             const atmosphere_settings& atmosphere, const march_settings& march,
                             const output_settings& output, const double planet_radius_km,
                             const std::vector<moon_config>& moons) {
  sky_gpu_block block{};

  for (size_t i = 0; i < sky_star_count && i < state.stars.size(); ++i) {
    const auto& star = state.stars[i];
    const auto direction = horizon_to_world(star.direction);
    block.star_direction[i] = glm::vec4(direction, static_cast<float>(to_radians(star.angular_radius_deg)));

    // Освещённость передаётся БЕЗ множителя горизонта. Гасить светило на CPU в момент захода
    // нельзя: сумерки существуют ровно потому, что верхние слои атмосферы продолжают его видеть,
    // и решать это должен марш по атмосфере отдельно для каждой своей точки.
    const double illuminance = star.space_illuminance_lx;
    block.star_color_illuminance[i] =
      glm::vec4(static_cast<float>(star.color_linear.x), static_cast<float>(star.color_linear.y),
                static_cast<float>(star.color_linear.z), static_cast<float>(std::max(0.0, illuminance)));
  }

  const size_t moon_count = std::min(state.moons.size(), sky_moon_capacity);
  for (size_t m = 0; m < moon_count; ++m) {
    const auto& moon = state.moons[m];
    const auto direction = horizon_to_world(moon.direction);
    block.moon_direction[m] = glm::vec4(direction, static_cast<float>(to_radians(moon.angular_radius_deg)));

    const double illuminance = moon.space_illuminance_lx;
    block.moon_color_illuminance[m] =
      glm::vec4(static_cast<float>(moon.color_linear.x), static_cast<float>(moon.color_linear.y),
                static_cast<float>(moon.color_linear.z), static_cast<float>(std::max(0.0, illuminance)));
    const double boost = m < moons.size() ? moons[m].disc_boost : 1.0;
    const double visual_scale = m < moons.size() ? moons[m].disc_visual_scale : 1.0;
    block.moon_phase[m] = glm::vec4(static_cast<float>(moon.phase), static_cast<float>(moon.occluded_fraction),
                                    static_cast<float>(boost), static_cast<float>(visual_scale));
  }

  block.atmosphere_geometry =
    glm::vec4(static_cast<float>(planet_radius_km), static_cast<float>(planet_radius_km + atmosphere.height_km),
              static_cast<float>(atmosphere.rayleigh_scale_km), static_cast<float>(atmosphere.mie_scale_km));
  block.atmosphere_medium =
    glm::vec4(static_cast<float>(atmosphere.mie_anisotropy), static_cast<float>(atmosphere.ozone_center_km),
              static_cast<float>(atmosphere.ozone_width_km), static_cast<float>(atmosphere.turbidity));
  block.march_params =
    glm::vec4(static_cast<float>(march.camera_height_km), static_cast<float>(march.primary_steps),
              static_cast<float>(march.light_steps), static_cast<float>(moon_count));
  block.output_params =
    glm::vec4(static_cast<float>(output.exposure), static_cast<float>(state.time_days),
              static_cast<float>(atmosphere.ground_albedo), static_cast<float>(output.debug_mode));

  block.sky_basis_east = glm::vec4(glm::vec3(star_frame.east_inertial), 0.0f);
  block.sky_basis_north = glm::vec4(glm::vec3(star_frame.north_inertial), 0.0f);
  block.sky_basis_up = glm::vec4(glm::vec3(star_frame.up_inertial), 0.0f);
  block.presentation_params =
    glm::vec4(static_cast<float>(output.disc_scale), static_cast<float>(output.star_density),
              static_cast<float>(output.star_brightness), static_cast<float>(output.galaxy_brightness));

  return block;
}

} // namespace devils_engine::pf07
