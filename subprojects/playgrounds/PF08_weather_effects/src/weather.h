#ifndef DEVILS_ENGINE_PF08_WEATHER_H
#define DEVILS_ENGINE_PF08_WEATHER_H

// Погодное состояние PF08. Здесь живут только величины с реальными consumer'ами текущего среза:
// аэрозоль читает атмосферная модель, направление и сила ветра — главный и теневой проходы листвы,
// локальную взвесь — froxel-объём. Облачность, осадки и мокрота появятся вместе со своими
// объёмными/частичными/surface consumer'ами.

#include <string>
#include <string_view>
#include <vector>

namespace devils_engine::pf08 {

struct weather_state {
  double aerosol_turbidity = 1.0;
  double wind_direction_deg = 250.0;
  double wind_strength_m = 0.22;
  // Однородная локальная среда. Extinction размерен в 1/м; ноль означает точный clear-bypass.
  double fog_extinction_per_m = 0.0;
  double fog_scattering_albedo = 0.92;
  double fog_anisotropy = 0.35;
  double fog_base_height_m = 0.0;
  double fog_scale_height_m = 80.0;
  // Низкочастотные world-space столбы плотности. Variation 0 возвращает точный высотный профиль.
  double fog_density_variation = 0.0;
  double fog_cell_size_m = 70.0;
  double fog_advection_speed_m_s = 0.0;
};

struct weather_preset {
  std::string name;
  double aerosol_turbidity = 1.0;
  double wind_direction_deg = 250.0;
  double wind_strength_m = 0.22;
  double fog_extinction_per_m = 0.0;
  double fog_scattering_albedo = 0.92;
  double fog_anisotropy = 0.35;
  double fog_base_height_m = 0.0;
  double fog_scale_height_m = 80.0;
  double fog_density_variation = 0.0;
  double fog_cell_size_m = 70.0;
  double fog_advection_speed_m_s = 0.0;
};

struct weather_preset_list {
  double transition_seconds = 4.0;
  std::vector<weather_preset> presets;
};

bool parse_weather_presets(const std::string& text, weather_preset_list& out, std::string& diagnostics);
weather_state state_from_preset(const weather_preset& preset);
const weather_preset* find_weather_preset(const weather_preset_list& list, std::string_view name);

double normalize_weather_direction(double degrees);
weather_state interpolate_weather(const weather_state& from, const weather_state& to, double fraction);

struct homogeneous_fog_integral {
  double transmittance = 1.0;
  // Множитель постоянной входящей яркости: albedo * (1 - T). Это независимый CPU-эталон для GPU.
  double in_scattering_fraction = 0.0;
};

homogeneous_fog_integral integrate_homogeneous_fog(double extinction_per_m,
                                                   double scattering_albedo,
                                                   double distance_m);
double fog_density_at_height(double height_m, double base_height_m, double scale_height_m);
double fog_light_transmittance(double extinction_per_m, double receiver_height_m,
                               double light_vertical_component, double base_height_m,
                               double scale_height_m);

// Переход хранит исходный snapshot, а не имя пресета: если T нажата посреди предыдущего перехода,
// новый начинается ровно из показанного кадра и не щёлкает обратно к прежней authored-точке.
class weather_transition {
public:
  void snap(std::string name, const weather_state& state);
  void set_target(std::string name, const weather_state& state, double duration_seconds);
  void advance(double seconds);

  const weather_state& state() const { return current_; }
  std::string_view source_name() const { return source_name_; }
  std::string_view target_name() const { return target_name_; }
  double progress() const;
  bool active() const { return elapsed_seconds_ < duration_seconds_; }

private:
  weather_state current_{};
  weather_state origin_{};
  weather_state target_{};
  std::string source_name_ = "clear";
  std::string target_name_ = "clear";
  double elapsed_seconds_ = 0.0;
  double duration_seconds_ = 0.0;
};

} // namespace devils_engine::pf08

#endif
