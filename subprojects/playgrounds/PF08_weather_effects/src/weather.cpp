#include "weather.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

#include <tavl/deserialize.h>
#include <tavl/parser.h>

namespace devils_engine::pf08 {
namespace {

void append_diagnostic(std::string& diagnostics, const std::string& message) {
  diagnostics += "  " + message + "\n";
}

bool valid_state(const weather_state& state, const std::string_view name, std::string& diagnostics) {
  bool valid = true;
  if (!std::isfinite(state.aerosol_turbidity) || state.aerosol_turbidity <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive aerosol_turbidity", name));
    valid = false;
  }
  if (!std::isfinite(state.wind_direction_deg)) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-finite wind_direction_deg", name));
    valid = false;
  }
  if (!std::isfinite(state.wind_strength_m) || state.wind_strength_m < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative wind_strength_m", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_extinction_per_m) || state.fog_extinction_per_m < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative fog_extinction_per_m", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_scattering_albedo) || state.fog_scattering_albedo < 0.0 ||
      state.fog_scattering_albedo > 1.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has fog_scattering_albedo outside [0, 1]", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_anisotropy) || std::abs(state.fog_anisotropy) > 0.85) {
    append_diagnostic(diagnostics, std::format("weather '{}' has fog_anisotropy outside [-0.85, 0.85]", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_base_height_m)) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-finite fog_base_height_m", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_scale_height_m) || state.fog_scale_height_m <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive fog_scale_height_m", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_density_variation) || state.fog_density_variation < 0.0 ||
      state.fog_density_variation > 0.95) {
    append_diagnostic(diagnostics, std::format("weather '{}' has fog_density_variation outside [0, 0.95]", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_cell_size_m) || state.fog_cell_size_m <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive fog_cell_size_m", name));
    valid = false;
  }
  if (!std::isfinite(state.fog_advection_speed_m_s) || state.fog_advection_speed_m_s < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative fog_advection_speed_m_s", name));
    valid = false;
  }
  if (!std::isfinite(state.cloud_coverage) || state.cloud_coverage < 0.0 || state.cloud_coverage > 1.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has cloud_coverage outside [0, 1]", name));
    valid = false;
  }
  if (!std::isfinite(state.cloud_extinction_per_m) || state.cloud_extinction_per_m < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative cloud_extinction_per_m", name));
    valid = false;
  }
  if (!std::isfinite(state.cloud_scattering_albedo) || state.cloud_scattering_albedo < 0.0 ||
      state.cloud_scattering_albedo > 1.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has cloud_scattering_albedo outside [0, 1]", name));
    valid = false;
  }
  if (!std::isfinite(state.cloud_anisotropy) || std::abs(state.cloud_anisotropy) > 0.85) {
    append_diagnostic(diagnostics, std::format("weather '{}' has cloud_anisotropy outside [-0.85, 0.85]", name));
    valid = false;
  }
  if (!std::isfinite(state.cloud_base_height_m) || !std::isfinite(state.cloud_top_height_m) ||
      state.cloud_top_height_m <= state.cloud_base_height_m) {
    append_diagnostic(diagnostics, std::format("weather '{}' has an empty cloud height interval", name));
    valid = false;
  }
  if (!std::isfinite(state.cloud_cell_size_m) || state.cloud_cell_size_m <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive cloud_cell_size_m", name));
    valid = false;
  }
  if (!std::isfinite(state.cloud_advection_speed_m_s) || state.cloud_advection_speed_m_s < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative cloud_advection_speed_m_s", name));
    valid = false;
  }
  if (!std::isfinite(state.rain_rate_mm_h) || state.rain_rate_mm_h < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative rain_rate_mm_h", name));
    valid = false;
  }
  if (!std::isfinite(state.rain_fall_speed_m_s) || state.rain_fall_speed_m_s <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive rain_fall_speed_m_s", name));
    valid = false;
  }
  if (!std::isfinite(state.rain_wind_speed_m_s) || state.rain_wind_speed_m_s < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative rain_wind_speed_m_s", name));
    valid = false;
  }
  if (!std::isfinite(state.rain_drop_length_m) || state.rain_drop_length_m <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive rain_drop_length_m", name));
    valid = false;
  }
  if (!std::isfinite(state.rain_near_radius_m) || state.rain_near_radius_m <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive rain_near_radius_m", name));
    valid = false;
  }
  if (!std::isfinite(state.rain_far_extinction_per_m) || state.rain_far_extinction_per_m < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative rain_far_extinction_per_m", name));
    valid = false;
  }
  if (!std::isfinite(state.snow_rate_mm_h) || state.snow_rate_mm_h < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative snow_rate_mm_h", name));
    valid = false;
  }
  if (!std::isfinite(state.snow_fall_speed_m_s) || state.snow_fall_speed_m_s <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive snow_fall_speed_m_s", name));
    valid = false;
  }
  if (!std::isfinite(state.snow_wind_speed_m_s) || state.snow_wind_speed_m_s < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative snow_wind_speed_m_s", name));
    valid = false;
  }
  if (!std::isfinite(state.snow_flake_size_m) || state.snow_flake_size_m <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive snow_flake_size_m", name));
    valid = false;
  }
  if (!std::isfinite(state.snow_near_radius_m) || state.snow_near_radius_m <= 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has non-positive snow_near_radius_m", name));
    valid = false;
  }
  if (!std::isfinite(state.snow_far_extinction_per_m) || state.snow_far_extinction_per_m < 0.0) {
    append_diagnostic(diagnostics, std::format("weather '{}' has negative snow_far_extinction_per_m", name));
    valid = false;
  }
  return valid;
}

} // namespace

bool parse_weather_presets(const std::string& text, weather_preset_list& out, std::string& diagnostics) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();

  tavl::ct_context context;
  out = weather_preset_list{};
  tavl::deserialize(parser, context, out);

  diagnostics.clear();
  for (const auto& entry : context.diagnostics) {
    diagnostics += std::format("  {} at {}:{} field '{}'\n", tavl::to_string(entry.error.type),
                               entry.error.span.line, entry.error.span.column, entry.field);
  }

  bool valid = context.diagnostics.empty();
  if (!std::isfinite(out.transition_seconds) || out.transition_seconds < 0.0) {
    append_diagnostic(diagnostics, "transition_seconds must be finite and non-negative");
    valid = false;
  }
  if (out.presets.empty()) {
    append_diagnostic(diagnostics, "at least one weather preset is required");
    valid = false;
  }

  std::unordered_set<std::string> names;
  for (auto& preset : out.presets) {
    if (preset.name.empty()) {
      append_diagnostic(diagnostics, "weather preset has an empty name");
      valid = false;
    } else if (!names.insert(preset.name).second) {
      append_diagnostic(diagnostics, std::format("duplicate weather preset '{}'", preset.name));
      valid = false;
    }
    preset.wind_direction_deg = normalize_weather_direction(preset.wind_direction_deg);
    valid = valid_state(state_from_preset(preset), preset.name, diagnostics) && valid;
  }
  return valid;
}

weather_state state_from_preset(const weather_preset& preset) {
  return weather_state{preset.aerosol_turbidity, normalize_weather_direction(preset.wind_direction_deg),
                       preset.wind_strength_m, preset.fog_extinction_per_m,
                       preset.fog_scattering_albedo, preset.fog_anisotropy,
                       preset.fog_base_height_m, preset.fog_scale_height_m,
                       preset.fog_density_variation, preset.fog_cell_size_m,
                       preset.fog_advection_speed_m_s, preset.cloud_coverage,
                       preset.cloud_extinction_per_m, preset.cloud_scattering_albedo,
                       preset.cloud_anisotropy, preset.cloud_base_height_m,
                       preset.cloud_top_height_m, preset.cloud_cell_size_m,
                       preset.cloud_advection_speed_m_s, preset.rain_rate_mm_h,
                       preset.rain_fall_speed_m_s, preset.rain_wind_speed_m_s,
                       preset.rain_drop_length_m, preset.rain_near_radius_m,
                       preset.rain_far_extinction_per_m, preset.snow_rate_mm_h,
                       preset.snow_fall_speed_m_s, preset.snow_wind_speed_m_s,
                       preset.snow_flake_size_m, preset.snow_near_radius_m,
                       preset.snow_far_extinction_per_m};
}

const weather_preset* find_weather_preset(const weather_preset_list& list, const std::string_view name) {
  const auto it = std::find_if(list.presets.begin(), list.presets.end(),
                               [name](const weather_preset& preset) { return preset.name == name; });
  return it == list.presets.end() ? nullptr : &*it;
}

double normalize_weather_direction(const double degrees) {
  if (!std::isfinite(degrees)) return degrees;
  const double wrapped = std::fmod(degrees, 360.0);
  return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
}

weather_state interpolate_weather(const weather_state& from, const weather_state& to, const double fraction) {
  const double t = std::clamp(fraction, 0.0, 1.0);
  // remainder выбирает кратчайшую дугу в [-180, 180]. На переходе 350 -> 10 середина поэтому 0,
  // а не 180 градусов; линейная интерполяция самих authored углов дала бы разворот ветра назад.
  const double direction_delta = std::remainder(to.wind_direction_deg - from.wind_direction_deg, 360.0);
  return weather_state{
    std::lerp(from.aerosol_turbidity, to.aerosol_turbidity, t),
    normalize_weather_direction(from.wind_direction_deg + direction_delta * t),
    std::lerp(from.wind_strength_m, to.wind_strength_m, t),
    std::lerp(from.fog_extinction_per_m, to.fog_extinction_per_m, t),
    std::lerp(from.fog_scattering_albedo, to.fog_scattering_albedo, t),
    std::lerp(from.fog_anisotropy, to.fog_anisotropy, t),
    std::lerp(from.fog_base_height_m, to.fog_base_height_m, t),
    std::lerp(from.fog_scale_height_m, to.fog_scale_height_m, t),
    std::lerp(from.fog_density_variation, to.fog_density_variation, t),
    std::lerp(from.fog_cell_size_m, to.fog_cell_size_m, t),
    std::lerp(from.fog_advection_speed_m_s, to.fog_advection_speed_m_s, t),
    std::lerp(from.cloud_coverage, to.cloud_coverage, t),
    std::lerp(from.cloud_extinction_per_m, to.cloud_extinction_per_m, t),
    std::lerp(from.cloud_scattering_albedo, to.cloud_scattering_albedo, t),
    std::lerp(from.cloud_anisotropy, to.cloud_anisotropy, t),
    std::lerp(from.cloud_base_height_m, to.cloud_base_height_m, t),
    std::lerp(from.cloud_top_height_m, to.cloud_top_height_m, t),
    std::lerp(from.cloud_cell_size_m, to.cloud_cell_size_m, t),
    std::lerp(from.cloud_advection_speed_m_s, to.cloud_advection_speed_m_s, t),
    std::lerp(from.rain_rate_mm_h, to.rain_rate_mm_h, t),
    std::lerp(from.rain_fall_speed_m_s, to.rain_fall_speed_m_s, t),
    std::lerp(from.rain_wind_speed_m_s, to.rain_wind_speed_m_s, t),
    std::lerp(from.rain_drop_length_m, to.rain_drop_length_m, t),
    std::lerp(from.rain_near_radius_m, to.rain_near_radius_m, t),
    std::lerp(from.rain_far_extinction_per_m, to.rain_far_extinction_per_m, t),
    std::lerp(from.snow_rate_mm_h, to.snow_rate_mm_h, t),
    std::lerp(from.snow_fall_speed_m_s, to.snow_fall_speed_m_s, t),
    std::lerp(from.snow_wind_speed_m_s, to.snow_wind_speed_m_s, t),
    std::lerp(from.snow_flake_size_m, to.snow_flake_size_m, t),
    std::lerp(from.snow_near_radius_m, to.snow_near_radius_m, t),
    std::lerp(from.snow_far_extinction_per_m, to.snow_far_extinction_per_m, t)};
}

homogeneous_fog_integral integrate_homogeneous_fog(const double extinction_per_m,
                                                   const double scattering_albedo,
                                                   const double distance_m) {
  const double extinction = std::max(0.0, extinction_per_m);
  const double albedo = std::clamp(scattering_albedo, 0.0, 1.0);
  const double distance = std::max(0.0, distance_m);
  const double transmittance = std::exp(-extinction * distance);
  return {transmittance, albedo * (1.0 - transmittance)};
}

double fog_density_at_height(const double height_m, const double base_height_m,
                             const double scale_height_m) {
  const double scale = std::max(scale_height_m, 1e-9);
  return std::exp(-std::max(height_m - base_height_m, 0.0) / scale);
}

double fog_light_transmittance(const double extinction_per_m, const double receiver_height_m,
                               const double light_vertical_component, const double base_height_m,
                               const double scale_height_m) {
  const double extinction = std::max(extinction_per_m, 0.0);
  if (extinction == 0.0) return 1.0;
  if (light_vertical_component <= 0.0) return 0.0;

  const double scale = std::max(scale_height_m, 1e-9);
  const double inverse_vertical = 1.0 / light_vertical_component;
  const double column_m = receiver_height_m < base_height_m
    ? (base_height_m - receiver_height_m + scale) * inverse_vertical
    : scale * fog_density_at_height(receiver_height_m, base_height_m, scale) * inverse_vertical;
  return std::exp(-extinction * column_m);
}

double cloud_vertical_column(const double receiver_height_m, const double base_height_m,
                             const double top_height_m) {
  const double thickness = std::max(top_height_m - base_height_m, 1e-9);
  if (receiver_height_m >= top_height_m) return 0.0;
  if (receiver_height_m <= base_height_m) return thickness * 0.5;
  const double u = (receiver_height_m - base_height_m) / thickness;
  constexpr double pi = 3.14159265358979323846;
  return thickness * ((1.0 - u) * 0.5 + std::sin(2.0 * pi * u) / (4.0 * pi));
}

double cloud_light_transmittance(const double extinction_per_m, const double horizontal_density,
                                 const double receiver_height_m, const double light_vertical_component,
                                 const double base_height_m, const double top_height_m) {
  if (extinction_per_m <= 0.0 || horizontal_density <= 0.0) return 1.0;
  if (light_vertical_component <= 0.0) return 0.0;
  const double column = cloud_vertical_column(receiver_height_m, base_height_m, top_height_m) /
                        light_vertical_component;
  return std::exp(-extinction_per_m * horizontal_density * column);
}

double rain_far_weight(const double distance_m, const double start_m, const double width_m) {
  const double x = std::clamp((distance_m - start_m) / std::max(width_m, 1e-9), 0.0, 1.0);
  return x * x * (3.0 - 2.0 * x);
}

void weather_transition::snap(std::string name, const weather_state& state) {
  current_ = state;
  current_.wind_direction_deg = normalize_weather_direction(current_.wind_direction_deg);
  origin_ = current_;
  target_ = current_;
  source_name_ = name;
  target_name_ = std::move(name);
  elapsed_seconds_ = 0.0;
  duration_seconds_ = 0.0;
}

void weather_transition::set_target(std::string name, const weather_state& state, const double duration_seconds) {
  origin_ = current_;
  target_ = state;
  target_.wind_direction_deg = normalize_weather_direction(target_.wind_direction_deg);
  source_name_ = target_name_;
  target_name_ = std::move(name);
  elapsed_seconds_ = 0.0;
  duration_seconds_ = std::max(0.0, duration_seconds);
  if (duration_seconds_ == 0.0) snap(target_name_, target_);
}

void weather_transition::advance(const double seconds) {
  if (!active()) return;
  elapsed_seconds_ = std::min(duration_seconds_, elapsed_seconds_ + std::max(0.0, seconds));
  current_ = interpolate_weather(origin_, target_, progress());
  if (!active()) source_name_ = target_name_;
}

double weather_transition::progress() const {
  return duration_seconds_ <= 0.0 ? 1.0 : std::clamp(elapsed_seconds_ / duration_seconds_, 0.0, 1.0);
}

} // namespace devils_engine::pf08
