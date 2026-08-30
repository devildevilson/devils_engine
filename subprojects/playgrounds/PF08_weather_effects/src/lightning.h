#ifndef DEVILS_ENGINE_PF08_LIGHTNING_H
#define DEVILS_ENGINE_PF08_LIGHTNING_H

#include <cstdint>

#include <glm/vec3.hpp>

namespace devils_engine::pf08 {

// Молния не является weather preset. Это короткоживущее событие, которое может быть создано грозой,
// заклинанием или сценарием. Автор меняется, а геометрия канала, световая энергия и temporal envelope — нет.
enum class lightning_author : uint32_t { weather, magic, scripted };
enum class lightning_profile : uint32_t { distant, close, magic };

struct lightning_event {
  glm::dvec3 start_m{0.0, 700.0, -900.0};
  glm::dvec3 end_m{0.0, 0.0, -900.0};
  glm::dvec3 color_linear{0.72, 0.84, 1.0};
  double channel_radius_m = 0.045;
  double channel_luminance_nits = 50000.0;
  // Point/short-line approximation used by surface and volume consumers. Keeping it separate from
  // channel luminance lets a subpixel distant bolt illuminate a cloud without growing a fat line.
  double luminous_intensity_cd = 2.0e9;
  double cloud_glow_radius_m = 650.0;
  double start_seconds = 0.0;
  double duration_seconds = 0.24;
  uint32_t stroke_count = 3;
  uint32_t seed = 1;
  lightning_author author = lightning_author::weather;
};

struct lightning_sample {
  double channel = 0.0;
  double flash = 0.0;
  bool active = false;
};

lightning_event make_lightning_event(lightning_profile profile, double start_seconds = 0.0,
                                     uint32_t seed = 1);
bool valid_lightning_event(const lightning_event& event);
lightning_sample sample_lightning(const lightning_event& event, double now_seconds);
double lightning_distance_to_channel_m(const lightning_event& event, const glm::dvec3& point_m);
double lightning_illuminance_lx(const lightning_event& event, const glm::dvec3& point_m,
                                double now_seconds);
double lightning_channel_width_pixels(const lightning_event& event, const glm::dvec3& observer_m,
                                      double vertical_fov_radians, uint32_t viewport_height);
bool lightning_channel_resolvable(const lightning_event& event, const glm::dvec3& observer_m,
                                  double vertical_fov_radians, uint32_t viewport_height,
                                  double minimum_pixels = 0.75);
double thunder_delay_seconds(double distance_m, double speed_of_sound_m_s = 343.0);

} // namespace devils_engine::pf08

#endif
