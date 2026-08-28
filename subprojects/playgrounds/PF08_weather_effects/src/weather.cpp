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
                       preset.wind_strength_m};
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
    std::lerp(from.wind_strength_m, to.wind_strength_m, t)};
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
