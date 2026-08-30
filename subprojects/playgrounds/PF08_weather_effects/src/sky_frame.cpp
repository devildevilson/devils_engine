#include "sky_frame.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include <glm/geometric.hpp>

#include <tavl/deserialize.h>
#include <tavl/parser.h>

namespace devils_engine::pf08 {

atmosphere_cache_gate::atmosphere_cache_gate(const uint32_t minimum_frame_gap) noexcept
  : minimum_frame_gap_(std::max(minimum_frame_gap, 1u)) {}

bool atmosphere_cache_gate::try_rebuild(const uint32_t submitted_frame) noexcept {
  // Unsigned difference сохраняет смысл и после wrap frame counter.
  if (has_rebuilt_ && submitted_frame - last_rebuild_frame_ < minimum_frame_gap_) return false;
  last_rebuild_frame_ = submitted_frame;
  has_rebuilt_ = true;
  return true;
}

bool valid_snow_sparkle_settings(const snow_sparkle_settings& settings) {
  return std::isfinite(settings.intensity) && settings.intensity >= 0.0 && settings.intensity <= 16.0 &&
         std::isfinite(settings.density) && settings.density >= 0.0 && settings.density <= 1.0 &&
         std::isfinite(settings.sharpness) && settings.sharpness >= 0.1 && settings.sharpness <= 4.0 &&
         std::isfinite(settings.source_balance) && settings.source_balance >= 0.0 &&
         settings.source_balance <= 1.0;
}

namespace {

constexpr double pi = std::numbers::pi;

double to_radians(const double degrees) {
  return degrees * pi / 180.0;
}

} // namespace

bool parse_colour_script(const std::string& text, colour_script& out, std::string& diagnostics) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();

  tavl::ct_context context;
  out = colour_script{};
  tavl::deserialize(parser, context, out);

  if (context.diagnostics.empty()) return true;

  diagnostics.clear();
  for (const auto& entry : context.diagnostics) {
    diagnostics += std::format("  {} at {}:{} field '{}'\n", tavl::to_string(entry.error.type), entry.error.span.line,
                               entry.error.span.column, entry.field);
  }
  return false;
}

bool parse_view_presets(const std::string& text, view_preset_list& out, std::string& diagnostics) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();

  tavl::ct_context context;
  out = view_preset_list{};
  tavl::deserialize(parser, context, out);

  if (context.diagnostics.empty()) return true;

  diagnostics.clear();
  for (const auto& entry : context.diagnostics) {
    diagnostics += std::format("  {} at {}:{} field '{}'\n", tavl::to_string(entry.error.type), entry.error.span.line,
                               entry.error.span.column, entry.field);
  }
  return false;
}

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

  // Освещённость дисков светил: без затмения и без горизонта. Она хранится в состоянии явно: при
  // полном перекрытии восстановить её делением фактического нуля на видимую долю уже невозможно.
  for (size_t i = 0; i < sky_star_count && i < state.stars.size(); ++i) {
    const auto& star = state.stars[i];
    block.star_disc_illuminance[i] = static_cast<float>(std::max(0.0, star.space_unocculted_lx));
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
    // В x лежит АЛЬБЕДО, а не фаза. Фазу шейдер восстанавливает сам из геометрии терминатора, а вот
    // альбедо ему нужно: яркость диска теперь считается физически, от света звёзд, и не берётся из
    // освещённости, которую луна шлёт нам, — та обращается в ноль в новолуние вместе со всем диском.
    const double albedo = m < moons.size() ? moons[m].albedo : 0.12;
    block.moon_phase[m] = glm::vec4(static_cast<float>(albedo), static_cast<float>(moon.occluded_fraction),
                                    static_cast<float>(boost), static_cast<float>(visual_scale));
    block.moon_star_visibility[m] =
      glm::vec4(static_cast<float>(moon.star_visibility[0]), static_cast<float>(moon.star_visibility[1]), 0.0f, 0.0f);
    block.moon_distance_km[m] = static_cast<float>(moon.distance_km);
  }

  block.atmosphere_geometry =
    glm::vec4(static_cast<float>(planet_radius_km), static_cast<float>(planet_radius_km + atmosphere.height_km),
              static_cast<float>(atmosphere.rayleigh_scale_km), static_cast<float>(atmosphere.mie_scale_km));
  block.atmosphere_medium =
    glm::vec4(static_cast<float>(atmosphere.mie_anisotropy), static_cast<float>(atmosphere.ozone_center_km),
              static_cast<float>(atmosphere.ozone_width_km), static_cast<float>(atmosphere.turbidity));
  block.march_params =
    glm::vec4(static_cast<float>(march.camera_height_km), static_cast<float>(march.primary_steps),
              static_cast<float>(march.aerial_range_km), static_cast<float>(moon_count));
  block.output_params =
    glm::vec4(static_cast<float>(output.exposure), static_cast<float>(state.time_days),
              static_cast<float>(atmosphere.ground_albedo), static_cast<float>(output.debug_mode));

  block.sky_basis_east = glm::vec4(glm::vec3(star_frame.east_inertial), 0.0f);
  block.sky_basis_north = glm::vec4(glm::vec3(star_frame.north_inertial), 0.0f);
  block.sky_basis_up = glm::vec4(glm::vec3(star_frame.up_inertial), 0.0f);
  block.grade_tint_saturation =
    glm::vec4(output.grade_tint, static_cast<float>(output.grade_saturation));
  block.grade_curve = glm::vec4(static_cast<float>(output.grade_contrast),
                                static_cast<float>(output.scotopic_strength),
                                static_cast<float>(output.corona_strength), 0.0f);
  block.presentation_params =
    glm::vec4(static_cast<float>(output.disc_scale), static_cast<float>(output.star_density),
              static_cast<float>(output.star_brightness), static_cast<float>(output.galaxy_concentration));
  block.rainbow_appearance =
    glm::vec4(static_cast<float>(output.rainbow.intensity),
              static_cast<float>(output.rainbow.saturation),
              static_cast<float>(output.rainbow.width),
              static_cast<float>(output.rainbow.sharpness));
  block.rainbow_context =
    glm::vec4(static_cast<float>(output.rainbow.veil_strength),
              static_cast<float>(output.rainbow.background_contrast),
              static_cast<float>(output.rainbow.persistence),
              static_cast<float>(output.rainbow.rain_cutoff_mm_h));
  block.rainbow_sources =
    glm::vec4(static_cast<float>(output.rainbow.sources),
              static_cast<float>(output.rainbow.secondary_bow_strength),
              static_cast<float>(output.rainbow.source_balance),
              static_cast<float>(output.rainbow.source_separation_scale));
  block.snow_sparkle =
    glm::vec4(static_cast<float>(output.snow_sparkle.intensity),
              static_cast<float>(output.snow_sparkle.density),
              static_cast<float>(output.snow_sparkle.sharpness),
              static_cast<float>(output.snow_sparkle.source_balance));

  return block;
}

} // namespace devils_engine::pf08
