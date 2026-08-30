#include "lightning.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/geometric.hpp>

namespace devils_engine::pf08 {
namespace {

double hash01(uint32_t value) {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return double(value) / double(UINT32_MAX);
}

double stroke_offset(const lightning_event& event, const uint32_t index) {
  if (index == 0 || event.stroke_count <= 1) return 0.0;
  const double t = double(index) / double(event.stroke_count - 1u);
  const double jitter = (hash01(event.seed + index * 0x9e3779b9u) - 0.5) * 0.08;
  return event.duration_seconds * std::clamp(0.12 + t * 0.62 + jitter, 0.04, 0.82);
}

} // namespace

lightning_event make_lightning_event(const lightning_profile profile, const double start_seconds,
                                     const uint32_t seed) {
  lightning_event event;
  event.start_seconds = start_seconds;
  event.seed = seed;
  if (profile == lightning_profile::close) {
    event.start_m = {12.0, 170.0, -58.0};
    event.end_m = {18.0, 1.0, -43.0};
    event.cloud_glow_radius_m = 180.0;
    event.luminous_intensity_cd = 2.5e8;
  } else if (profile == lightning_profile::magic) {
    event.start_m = {-2.0, 24.0, -32.0};
    event.end_m = {3.0, 1.2, -27.0};
    event.color_linear = {0.62, 0.34, 1.0};
    event.channel_radius_m = 0.12;
    event.channel_luminance_nits = 90000.0;
    event.luminous_intensity_cd = 4.0e7;
    event.cloud_glow_radius_m = 45.0;
    event.duration_seconds = 0.42;
    event.stroke_count = 1;
    event.author = lightning_author::magic;
  }
  return event;
}

bool valid_lightning_event(const lightning_event& e) {
  const auto finite3 = [](const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  };
  return finite3(e.start_m) && finite3(e.end_m) && glm::length(e.end_m - e.start_m) > 0.01 &&
         finite3(e.color_linear) && e.color_linear.x >= 0.0 && e.color_linear.y >= 0.0 &&
         e.color_linear.z >= 0.0 && std::isfinite(e.channel_radius_m) && e.channel_radius_m > 0.0 &&
         std::isfinite(e.channel_luminance_nits) && e.channel_luminance_nits >= 0.0 &&
         std::isfinite(e.luminous_intensity_cd) && e.luminous_intensity_cd >= 0.0 &&
         std::isfinite(e.cloud_glow_radius_m) && e.cloud_glow_radius_m > 0.0 &&
         std::isfinite(e.start_seconds) && std::isfinite(e.duration_seconds) &&
         e.duration_seconds > 0.0 && e.stroke_count >= 1 && e.stroke_count <= 8;
}

lightning_sample sample_lightning(const lightning_event& event, const double now_seconds) {
  const double age = now_seconds - event.start_seconds;
  if (age < 0.0 || age > event.duration_seconds || !valid_lightning_event(event)) return {};

  double channel = 0.0;
  double flash = 0.0;
  for (uint32_t i = 0; i < event.stroke_count; ++i) {
    const double local = age - stroke_offset(event, i);
    if (local < 0.0) continue;
    const double attack = std::min(0.0025, event.duration_seconds * 0.08);
    const double channel_decay = event.duration_seconds * (0.055 + hash01(event.seed + i * 17u) * 0.045);
    const double flash_decay = event.duration_seconds * (0.18 + hash01(event.seed + i * 29u) * 0.12);
    const double onset = 1.0 - std::exp(-local / std::max(attack, 1e-5));
    channel = std::max(channel, onset * std::exp(-local / std::max(channel_decay, 1e-5)));
    flash = std::max(flash, onset * std::exp(-local / std::max(flash_decay, 1e-5)));
  }

  return lightning_sample{channel, flash, channel > 1e-4 || flash > 1e-4};
}

double lightning_distance_to_channel_m(const lightning_event& event, const glm::dvec3& point_m) {
  const glm::dvec3 segment = event.end_m - event.start_m;
  const double length_squared = glm::dot(segment, segment);
  if (length_squared <= 1e-12) return glm::length(point_m - event.start_m);
  const double t = std::clamp(glm::dot(point_m - event.start_m, segment) / length_squared, 0.0, 1.0);
  return glm::length(point_m - (event.start_m + segment * t));
}

double lightning_illuminance_lx(const lightning_event& event, const glm::dvec3& point_m,
                                const double now_seconds) {
  const auto sample = sample_lightning(event, now_seconds);
  if (!sample.active || event.luminous_intensity_cd <= 0.0) return 0.0;
  const double distance = lightning_distance_to_channel_m(event, point_m);
  const double softened_distance_squared = distance * distance + event.channel_radius_m * event.channel_radius_m;
  return event.luminous_intensity_cd * sample.flash / std::max(softened_distance_squared, 1e-6);
}

double lightning_channel_width_pixels(const lightning_event& event, const glm::dvec3& observer_m,
                                      const double vertical_fov_radians, const uint32_t viewport_height) {
  if (!valid_lightning_event(event) || !std::isfinite(vertical_fov_radians) ||
      vertical_fov_radians <= 0.0 || viewport_height == 0) return 0.0;
  const glm::dvec3 midpoint = (event.start_m + event.end_m) * 0.5;
  const double distance = glm::length(midpoint - observer_m);
  const double angular_width = 2.0 * std::atan(event.channel_radius_m / std::max(distance, 1e-6));
  return angular_width / vertical_fov_radians * double(viewport_height);
}

bool lightning_channel_resolvable(const lightning_event& event, const glm::dvec3& observer_m,
                                  const double vertical_fov_radians, const uint32_t viewport_height,
                                  const double minimum_pixels) {
  return std::isfinite(minimum_pixels) && minimum_pixels >= 0.0 &&
         lightning_channel_width_pixels(event, observer_m, vertical_fov_radians, viewport_height) >= minimum_pixels;
}

double thunder_delay_seconds(const double distance_m, const double speed_of_sound_m_s) {
  if (!std::isfinite(distance_m) || distance_m <= 0.0 || !std::isfinite(speed_of_sound_m_s) ||
      speed_of_sound_m_s <= 0.0) return 0.0;
  return distance_m / speed_of_sound_m_s;
}

} // namespace devils_engine::pf08
