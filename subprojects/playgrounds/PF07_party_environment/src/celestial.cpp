#include "celestial.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>

#include <tavl/deserialize.h>
#include <tavl/parser.h>

namespace devils_engine::pf07 {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double au_km = 149597870.7;
constexpr double sun_radius_km = 695700.0;
constexpr double solar_irradiance_1au_w_m2 = 1361.0;
constexpr double gm_sun_km3_s2 = 1.32712440018e11;
constexpr double gm_earth_km3_s2 = 3.986004418e5;
constexpr double luminous_efficacy_max_lm_per_w = 683.0;
constexpr double stefan_boltzmann_w_m2_k4 = 5.670374419e-8;
constexpr double planck_h_j_s = 6.62607015e-34;
constexpr double light_speed_m_s = 2.99792458e8;
constexpr double boltzmann_j_k = 1.380649e-23;

// Индексы тел в общем списке затмевателей: два светила, планета, затем луны.
constexpr int32_t planet_body_index = 2;
constexpr int32_t first_moon_body_index = 3;

double to_radians(const double degrees) {
  return degrees * pi / 180.0;
}

double to_degrees(const double radians) {
  return radians * 180.0 / pi;
}

double wrap_angle(const double radians) {
  const double turns = 2.0 * pi;
  const double wrapped = std::fmod(radians, turns);
  return wrapped < 0.0 ? wrapped + turns : wrapped;
}

double safe_acos(const double value) {
  return std::acos(std::clamp(value, -1.0, 1.0));
}

double vector_length(const glm::dvec3& value) {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

glm::dvec3 normalized(const glm::dvec3& value) {
  const double length = vector_length(value);
  if (length == 0.0) return glm::dvec3(0.0, 0.0, 0.0);
  return glm::dvec3(value.x / length, value.y / length, value.z / length);
}

double dot_product(const glm::dvec3& a, const glm::dvec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

glm::dvec3 cross_product(const glm::dvec3& a, const glm::dvec3& b) {
  return glm::dvec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

double angle_between(const glm::dvec3& a, const glm::dvec3& b) {
  // Через atan2 от длины векторного произведения: у малых углов между почти совпадающими
  // направлениями это единственная устойчивая форма, а именно там и происходят затмения.
  const double cross_length = vector_length(cross_product(a, b));
  return std::atan2(cross_length, dot_product(a, b));
}

// Период обращения по третьему закону Кеплера, в СЕКУНДАХ. Раньше эта функция сразу делила на сутки,
// но сутки теперь производны от года, и до вычисления года их длина неизвестна.
double orbital_period_seconds(const double semi_major_km, const double gravity_parameter_km3_s2) {
  return 2.0 * pi * std::sqrt(semi_major_km * semi_major_km * semi_major_km / gravity_parameter_km3_s2);
}

glm::dvec3 orbit_position(const orbit_config& orbit, const double semi_major_km, const double period_days,
                          const double time_days) {
  const double mean_anomaly =
    wrap_angle(to_radians(orbit.mean_anomaly_epoch_deg) + 2.0 * pi * time_days / period_days);
  const double eccentricity = orbit.eccentricity;
  const double eccentric_anomaly = solve_kepler(mean_anomaly, eccentricity);

  const double plane_x = semi_major_km * (std::cos(eccentric_anomaly) - eccentricity);
  const double plane_y = semi_major_km * std::sqrt(1.0 - eccentricity * eccentricity) * std::sin(eccentric_anomaly);

  const double periapsis = to_radians(orbit.periapsis_argument_deg);
  const double inclination = to_radians(orbit.inclination_deg);
  const double node = to_radians(orbit.ascending_node_deg);

  const double rotated_x = plane_x * std::cos(periapsis) - plane_y * std::sin(periapsis);
  const double rotated_y = plane_x * std::sin(periapsis) + plane_y * std::cos(periapsis);

  const double inclined_y = rotated_y * std::cos(inclination);
  const double inclined_z = rotated_y * std::sin(inclination);

  return glm::dvec3(rotated_x * std::cos(node) - inclined_y * std::sin(node),
                    rotated_x * std::sin(node) + inclined_y * std::cos(node), inclined_z);
}

// Аппроксимация функций CIE 1931 суммой сдвинутых гауссиан (Wyman, Sloan, Shirley 2013). Точности
// этой аппроксимации достаточно и для цвета, и для световой отдачи: обе величины интегральные.
double gaussian_lobe(const double wavelength_nm, const double center, const double sigma_low, const double sigma_high) {
  const double sigma = wavelength_nm < center ? sigma_low : sigma_high;
  const double offset = (wavelength_nm - center) / sigma;
  return std::exp(-0.5 * offset * offset);
}

glm::dvec3 cie_response(const double wavelength_nm) {
  const double x = 1.056 * gaussian_lobe(wavelength_nm, 599.8, 37.9, 31.0) +
                   0.362 * gaussian_lobe(wavelength_nm, 442.0, 16.0, 26.7) -
                   0.065 * gaussian_lobe(wavelength_nm, 501.1, 20.4, 26.2);
  const double y = 0.821 * gaussian_lobe(wavelength_nm, 568.8, 46.9, 40.5) +
                   0.286 * gaussian_lobe(wavelength_nm, 530.9, 16.3, 31.1);
  const double z = 1.217 * gaussian_lobe(wavelength_nm, 437.0, 11.8, 36.0) +
                   0.681 * gaussian_lobe(wavelength_nm, 459.0, 26.0, 13.8);
  return glm::dvec3(x, y, z);
}

double planck_radiance(const double wavelength_nm, const double temperature_kelvin) {
  const double wavelength_m = wavelength_nm * 1e-9;
  const double numerator = 2.0 * planck_h_j_s * light_speed_m_s * light_speed_m_s;
  const double denominator = std::pow(wavelength_m, 5.0);
  const double exponent = planck_h_j_s * light_speed_m_s / (wavelength_m * boltzmann_j_k * temperature_kelvin);
  return numerator / denominator / std::expm1(exponent);
}

// Один проход интегрирования даёт и трёхстимульные значения, и долю видимой мощности.
glm::dvec3 integrate_tristimulus(const double temperature_kelvin) {
  constexpr double first_nm = 360.0;
  constexpr double last_nm = 830.0;
  constexpr double step_nm = 1.0;
  constexpr double step_m = step_nm * 1e-9;

  auto total = glm::dvec3(0.0, 0.0, 0.0);
  for (double wavelength = first_nm; wavelength <= last_nm; wavelength += step_nm) {
    const double radiance = planck_radiance(wavelength, temperature_kelvin);
    const auto response = cie_response(wavelength);
    total.x += radiance * response.x * step_m;
    total.y += radiance * response.y * step_m;
    total.z += radiance * response.z * step_m;
  }
  return total;
}

double read_orbit_semi_major_km(const orbit_config& orbit, const bool in_astronomical_units) {
  return in_astronomical_units ? orbit.semi_major_axis * au_km : orbit.semi_major_axis;
}

} // namespace

double solve_kepler(const double mean_anomaly_rad, const double eccentricity) {
  // Ньютон с фиксированным числом итераций. Для e < 0.7 восьми шагов заведомо достаточно:
  // сходимость квадратичная, а фиксированное число операций сохраняет побитную повторяемость.
  constexpr int32_t iterations = 8;

  double eccentric_anomaly = mean_anomaly_rad + eccentricity * std::sin(mean_anomaly_rad);
  for (int32_t i = 0; i < iterations; ++i) {
    const double residual = eccentric_anomaly - eccentricity * std::sin(eccentric_anomaly) - mean_anomaly_rad;
    const double derivative = 1.0 - eccentricity * std::cos(eccentric_anomaly);
    eccentric_anomaly -= residual / derivative;
  }
  return eccentric_anomaly;
}

double disk_occluded_fraction(const double separation_rad, const double source_radius_rad,
                              const double occluder_radius_rad) {
  if (source_radius_rad <= 0.0) return 0.0;
  if (occluder_radius_rad <= 0.0) return 0.0;
  if (separation_rad >= source_radius_rad + occluder_radius_rad) return 0.0;

  if (separation_rad <= std::abs(source_radius_rad - occluder_radius_rad)) {
    if (occluder_radius_rad >= source_radius_rad) return 1.0;
    const double ratio = occluder_radius_rad / source_radius_rad;
    return ratio * ratio;
  }

  const double d = separation_rad;
  const double r1 = source_radius_rad;
  const double r2 = occluder_radius_rad;
  const double first = r1 * r1 * safe_acos((d * d + r1 * r1 - r2 * r2) / (2.0 * d * r1));
  const double second = r2 * r2 * safe_acos((d * d + r2 * r2 - r1 * r1) / (2.0 * d * r2));
  const double triangle =
    std::sqrt(std::max(0.0, (-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2)));
  const double lens_area = first + second - 0.5 * triangle;
  return std::clamp(lens_area / (pi * r1 * r1), 0.0, 1.0);
}

double disk_fraction_above_horizon(const double altitude_rad, const double radius_rad) {
  if (radius_rad <= 0.0) return altitude_rad > 0.0 ? 1.0 : 0.0;
  if (altitude_rad >= radius_rad) return 1.0;
  if (altitude_rad <= -radius_rad) return 0.0;

  // Площадь сегмента ниже горизонта для диска, центр которого стоит на высоте altitude_rad.
  // При отрицательной высоте формула автоматически даёт площадь больше половины диска.
  const double below =
    radius_rad * radius_rad * safe_acos(altitude_rad / radius_rad) -
    altitude_rad * std::sqrt(std::max(0.0, radius_rad * radius_rad - altitude_rad * altitude_rad));
  return std::clamp(1.0 - below / (pi * radius_rad * radius_rad), 0.0, 1.0);
}

double blackbody_luminous_efficacy(const double temperature_kelvin) {
  const auto tristimulus = integrate_tristimulus(temperature_kelvin);
  const double total_radiance =
    stefan_boltzmann_w_m2_k4 * std::pow(temperature_kelvin, 4.0) / pi;
  return luminous_efficacy_max_lm_per_w * tristimulus.y / total_radiance;
}

glm::dvec3 blackbody_color_linear(const double temperature_kelvin) {
  const auto tristimulus = integrate_tristimulus(temperature_kelvin);
  const double sum = tristimulus.x + tristimulus.y + tristimulus.z;
  if (sum <= 0.0) return glm::dvec3(1.0, 1.0, 1.0);

  const double chroma_x = tristimulus.x / sum;
  const double chroma_y = tristimulus.y / sum;
  const double big_x = chroma_x / chroma_y;
  const double big_y = 1.0;
  const double big_z = (1.0 - chroma_x - chroma_y) / chroma_y;

  const double red = 3.2406 * big_x - 1.5372 * big_y - 0.4986 * big_z;
  const double green = -0.9689 * big_x + 1.8758 * big_y + 0.0415 * big_z;
  const double blue = 0.0557 * big_x - 0.2040 * big_y + 1.0570 * big_z;

  const double maximum = std::max({red, green, blue});
  if (maximum <= 0.0) return glm::dvec3(1.0, 1.0, 1.0);
  return glm::dvec3(std::max(0.0, red / maximum), std::max(0.0, green / maximum), std::max(0.0, blue / maximum));
}

celestial_system::celestial_system(system_config config) : config_(std::move(config)) {
  const std::array<const star_config*, 2> sources{&config_.primary, &config_.companion};
  for (size_t i = 0; i < stars_.size(); ++i) {
    const auto& source = *sources[i];
    auto& derived = stars_[i];

    const double radius_ratio = source.radius_solar;
    const double temperature_ratio = source.temperature_kelvin / 5772.0;
    derived.luminosity_solar =
      radius_ratio * radius_ratio * temperature_ratio * temperature_ratio * temperature_ratio * temperature_ratio;
    derived.luminous_efficacy_lm_per_w = blackbody_luminous_efficacy(source.temperature_kelvin);
    derived.illuminance_at_1au_lx =
      solar_irradiance_1au_w_m2 * derived.luminosity_solar * derived.luminous_efficacy_lm_per_w;
    derived.color_linear = blackbody_color_linear(source.temperature_kelvin);
  }

  const double total_mass_solar = config_.primary.mass_solar + config_.companion.mass_solar;

  // Порядок здесь обязателен. Год задан орбитой и массами и ни от чего больше не зависит; длина
  // солнечных суток получается делением года на заданное ЦЕЛОЕ их число; и только после этого все
  // прочие периоды можно выразить в сутках.
  const double year_seconds =
    orbital_period_seconds(read_orbit_semi_major_km(config_.planet.orbit, true), gm_sun_km3_s2 * total_mass_solar);
  const double days_per_year = double(std::max(config_.planet.solar_days_per_year, 1u));
  day_seconds_ = year_seconds / days_per_year;
  planet_year_days_ = days_per_year;

  binary_period_days_ =
    orbital_period_seconds(read_orbit_semi_major_km(config_.binary, true), gm_sun_km3_s2 * total_mass_solar) /
    day_seconds_;

  moon_periods_days_.reserve(config_.moons.size());
  for (const auto& moon : config_.moons) {
    moon_periods_days_.push_back(
      orbital_period_seconds(read_orbit_semi_major_km(moon.orbit, false),
                             gm_earth_km3_s2 * config_.planet.mass_earth) /
      day_seconds_);
  }

  // Длина цикла: через сколько ЦЕЛЫХ лет взаимное положение двойной и планеты повторяется. Ищется
  // перебором, а не задаётся числом в конфиге: стоит подвинуть орбиту — и прежнее число станет
  // молча неверным.
  cycle_years_ = 1;
  double best_error = 1.0;
  for (uint32_t years = 1; years <= 24; ++years) {
    const double orbits = double(years) * planet_year_days_ / binary_period_days_;
    const double error = std::abs(orbits - std::round(orbits));
    if (error < best_error - 1e-9) {
      best_error = error;
      cycle_years_ = years;
    }
  }

  // Полночь — нижняя кульминация главного светила. Ищется численно по минимуму его высоты за сутки,
  // а не выводится формулой: высота уже собирает в себе и вращение планеты, и её движение по орбите,
  // и наклон оси, и повторять этот вывод отдельно значило бы завести второй источник правды.
  // Ищется ПОСЛЕДНЯЯ полночь до нуля, а не первая после. Иначе нулевой момент оказывается ещё во
  // вчерашних сутках, и отчёт на старте сцены показывает последний день предыдущего года.
  midnight_epoch_days_ = -1.0;
  double lowest = 1.0e30;
  constexpr int coarse_steps = 240;
  for (int step = 0; step < coarse_steps; ++step) {
    const double time = -1.0 + double(step) / double(coarse_steps);
    const double altitude = evaluate(time).stars[0].altitude_deg;
    if (altitude < lowest) {
      lowest = altitude;
      midnight_epoch_days_ = time;
    }
  }
  double window = 1.0 / double(coarse_steps);
  for (int pass = 0; pass < 6; ++pass) {
    const double centre = midnight_epoch_days_;
    for (int step = -4; step <= 4; ++step) {
      const double time = centre + double(step) * window * 0.25;
      const double altitude = evaluate(time).stars[0].altitude_deg;
      if (altitude < lowest) {
        lowest = altitude;
        midnight_epoch_days_ = time;
      }
    }
    window *= 0.25;
  }
}

double celestial_system::day_seconds() const {
  return day_seconds_;
}

double celestial_system::sidereal_rotation_days() const {
  // За солнечные сутки планета проходит по орбите долю 1/N года, поэтому меридиан обязан повернуться
  // на полный оборот ПЛЮС эту долю. Отсюда звёздный оборот короче солнечных суток.
  return planet_year_days_ / (planet_year_days_ + 1.0);
}

double celestial_system::midnight_epoch_days() const {
  return midnight_epoch_days_;
}

uint32_t celestial_system::cycle_years() const {
  return cycle_years_;
}

celestial_system::calendar_time celestial_system::to_calendar(const double time_days) const {
  // Отсчёт ведётся от ПОЛУНОЧИ, а не от начала сцены. Иначе ноль часов приходится на произвольную
  // фазу вращения планеты, и закат оказывается в 05:52, а восход в 19:23.
  const double civil = time_days - midnight_epoch_days_;
  const double whole = std::floor(civil);
  const double fraction = civil - whole;
  const double days_per_year = std::max(planet_year_days_, 1.0);

  const int64_t absolute_day = int64_t(whole);
  const int64_t year = int64_t(std::floor(double(absolute_day) / days_per_year));
  const int64_t day_of_year = absolute_day - int64_t(std::llround(double(year) * days_per_year));

  calendar_time out{};
  out.year = uint32_t(std::max<int64_t>(year, 0));
  out.cycle_year = uint32_t(out.year % std::max(cycle_years_, 1u)) + 1u;
  out.day = uint32_t(std::max<int64_t>(day_of_year, 0));
  const double hours = fraction * 24.0;
  out.hour = uint32_t(std::floor(hours));
  out.minute = uint32_t(std::floor((hours - std::floor(hours)) * 60.0));
  return out;
}

double celestial_system::from_calendar(const uint32_t cycle_year, const uint32_t day,
                                       const double hour) const {
  const double years = double(cycle_year == 0 ? 0u : cycle_year - 1u);
  return midnight_epoch_days_ + years * planet_year_days_ + double(day) + hour / 24.0;
}

const system_config& celestial_system::config() const {
  return config_;
}

const star_properties& celestial_system::star(const size_t index) const {
  return stars_[index];
}

double celestial_system::binary_period_days() const {
  return binary_period_days_;
}

double celestial_system::planet_year_days() const {
  return planet_year_days_;
}

double celestial_system::moon_period_days(const size_t index) const {
  return moon_periods_days_[index];
}

double holman_wiegert_critical_ratio(const double eccentricity, const double mass_ratio) {
  const double e = eccentricity;
  const double u = mass_ratio;
  return 1.60 + 5.10 * e - 2.22 * e * e + 4.12 * u - 4.27 * e * u - 5.09 * u * u + 4.61 * e * e * u * u;
}

double celestial_system::binary_revolutions_per_year() const {
  return planet_year_days_ / binary_period_days_;
}

double celestial_system::stability_margin() const {
  const double ratio = config_.planet.orbit.semi_major_axis / config_.binary.semi_major_axis;
  const double mass_ratio =
    config_.companion.mass_solar / (config_.primary.mass_solar + config_.companion.mass_solar);
  return ratio / holman_wiegert_critical_ratio(config_.binary.eccentricity, mass_ratio);
}

double celestial_system::maximum_star_separation_deg() const {
  // Апоцентр двойной, наблюдаемый с перицентра планеты, даёт верхнюю границу разделения.
  const double binary_apoapsis = config_.binary.semi_major_axis * (1.0 + config_.binary.eccentricity);
  const double planet_periapsis = config_.planet.orbit.semi_major_axis * (1.0 - config_.planet.orbit.eccentricity);
  return to_degrees(std::atan(binary_apoapsis / planet_periapsis));
}

double celestial_system::star_conjunction_interval_days() const {
  // Соединения происходят дважды за синодический период двойной относительно планеты.
  const double synodic = binary_period_days_ / (1.0 - binary_period_days_ / planet_year_days_);
  return synodic * 0.5;
}

celestial_system::instant celestial_system::sample(const double time_days) const {
  instant now;

  const double primary_mass = config_.primary.mass_solar;
  const double companion_mass = config_.companion.mass_solar;
  const double total_mass = primary_mass + companion_mass;

  const auto relative =
    orbit_position(config_.binary, read_orbit_semi_major_km(config_.binary, true), binary_period_days_, time_days);
  now.stars[0].position = relative * (-companion_mass / total_mass);
  now.stars[0].radius_km = config_.primary.radius_solar * sun_radius_km;
  now.stars[1].position = relative * (primary_mass / total_mass);
  now.stars[1].radius_km = config_.companion.radius_solar * sun_radius_km;

  now.planet.position = orbit_position(config_.planet.orbit, read_orbit_semi_major_km(config_.planet.orbit, true),
                                       planet_year_days_, time_days);
  now.planet.radius_km = config_.planet.radius_km;

  now.moons.resize(config_.moons.size());
  for (size_t i = 0; i < config_.moons.size(); ++i) {
    const auto& moon = config_.moons[i];
    now.moons[i].position =
      now.planet.position +
      orbit_position(moon.orbit, read_orbit_semi_major_km(moon.orbit, false), moon_periods_days_[i], time_days);
    now.moons[i].radius_km = moon.radius_km;
  }

  // Ось вращения планеты задаётся наклоном от нормали к плоскости двойной и азимутом этого наклона.
  const double tilt = to_radians(config_.planet.axial_tilt_deg);
  const double tilt_azimuth = to_radians(config_.planet.axis_azimuth_deg);
  const auto axis = glm::dvec3(std::sin(tilt) * std::cos(tilt_azimuth), std::sin(tilt) * std::sin(tilt_azimuth),
                               std::cos(tilt));

  const auto helper = std::abs(axis.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
  const auto planet_x = normalized(cross_product(helper, axis));
  const auto planet_y = cross_product(axis, planet_x);

  const double rotation_period_days = sidereal_rotation_days();
  const double meridian = wrap_angle(to_radians(config_.planet.prime_meridian_epoch_deg) +
                                     to_radians(config_.observer.longitude_deg) +
                                     2.0 * pi * time_days / rotation_period_days);
  const double latitude = to_radians(config_.observer.latitude_deg);

  now.up = normalized(planet_x * (std::cos(latitude) * std::cos(meridian)) +
                      planet_y * (std::cos(latitude) * std::sin(meridian)) + axis * std::sin(latitude));
  now.observer = now.planet.position + now.up * config_.planet.radius_km;
  now.north = normalized(axis - now.up * dot_product(axis, now.up));
  now.east = cross_product(now.north, now.up);

  return now;
}

celestial_system::illumination_sample celestial_system::star_illuminance_at(const size_t star_index,
                                                                           const glm::dvec3& point, const instant& now,
                                                                           const int32_t ignored_moon) const {
  illumination_sample result;

  const auto to_star = now.stars[star_index].position - point;
  const double distance = vector_length(to_star);
  if (distance <= 0.0) return result;

  const double distance_au = distance / au_km;
  const double unoccluded = stars_[star_index].illuminance_at_1au_lx / (distance_au * distance_au);
  const double source_radius = std::asin(std::clamp(now.stars[star_index].radius_km / distance, 0.0, 1.0));

  // Одновременное перекрытие одного диска двумя телами астрономически маловероятно, поэтому берётся
  // максимум, а не произведение: для вложенных дисков это точный ответ, для раздельных — верхняя оценка.
  const auto accumulate = [&](const body_position& body, const int32_t index) {
    const auto to_body = body.position - point;
    const double body_distance = vector_length(to_body);
    if (body_distance <= 0.0 || body_distance >= distance) return;

    const double body_radius = std::asin(std::clamp(body.radius_km / body_distance, 0.0, 1.0));
    const double separation = angle_between(to_star, to_body);
    const double fraction = disk_occluded_fraction(separation, source_radius, body_radius);
    if (fraction <= result.occluded_fraction) return;

    result.occluded_fraction = fraction;
    result.occluder = index;
  };

  accumulate(now.stars[1 - star_index], static_cast<int32_t>(1 - star_index));
  accumulate(now.planet, planet_body_index);
  for (size_t i = 0; i < now.moons.size(); ++i) {
    if (static_cast<int32_t>(i) == ignored_moon) continue;
    accumulate(now.moons[i], first_moon_body_index + static_cast<int32_t>(i));
  }

  result.unoccluded_lx = unoccluded;
  result.illuminance_lx = unoccluded * (1.0 - result.occluded_fraction);
  return result;
}

celestial_system::body_positions celestial_system::positions(const double time_days) const {
  const auto now = sample(time_days);

  body_positions result;
  result.stars[0] = now.stars[0].position;
  result.stars[1] = now.stars[1].position;
  result.planet = now.planet.position;
  result.observer = now.observer;
  result.moons.reserve(now.moons.size());
  for (const auto& moon : now.moons) {
    result.moons.push_back(moon.position);
  }
  return result;
}

sky_state celestial_system::evaluate(const double time_days) const {
  const auto now = sample(time_days);

  sky_state state;
  state.time_days = time_days;
  state.east_inertial = now.east;
  state.north_inertial = now.north;
  state.up_inertial = now.up;
  state.moons.resize(config_.moons.size());

  const auto to_local = [&](const glm::dvec3& world_direction) {
    return glm::dvec3(dot_product(world_direction, now.east), dot_product(world_direction, now.north),
                      dot_product(world_direction, now.up));
  };

  const auto fill_geometry = [&](body_view& view, const glm::dvec3& world_position, const double radius_km) {
    const auto offset = world_position - now.observer;
    view.distance_km = vector_length(offset);
    view.direction = to_local(normalized(offset));
    view.altitude_deg = to_degrees(std::asin(std::clamp(view.direction.z, -1.0, 1.0)));
    view.azimuth_deg = to_degrees(wrap_angle(std::atan2(view.direction.x, view.direction.y)));
    view.angular_radius_deg = to_degrees(std::asin(std::clamp(radius_km / view.distance_km, 0.0, 1.0)));
    view.horizon_fraction =
      disk_fraction_above_horizon(to_radians(view.altitude_deg), to_radians(view.angular_radius_deg));
  };

  for (size_t i = 0; i < state.stars.size(); ++i) {
    auto& view = state.stars[i];
    const auto& source = i == 0 ? config_.primary : config_.companion;
    view.name = source.name;
    view.kind = body_kind::star;
    view.color_linear = stars_[i].color_linear;
    view.phase = 1.0;
    fill_geometry(view, now.stars[i].position, now.stars[i].radius_km);

    const auto to_star = now.stars[i].position - now.observer;
    const double source_radius = to_radians(view.angular_radius_deg);
    const auto accumulate = [&](const body_position& body, const int32_t index) {
      const auto to_body = body.position - now.observer;
      const double body_distance = vector_length(to_body);
      if (body_distance <= 0.0 || body_distance >= view.distance_km) return;

      const double body_radius = std::asin(std::clamp(body.radius_km / body_distance, 0.0, 1.0));
      const double separation = angle_between(to_star, to_body);
      const double fraction = disk_occluded_fraction(separation, source_radius, body_radius);
      if (fraction <= view.occluded_fraction) return;

      view.occluded_fraction = fraction;
      view.occluder = index;
    };

    // Планета не является затмевателем для наблюдателя на её поверхности: её вклад — это горизонт.
    accumulate(now.stars[1 - i], static_cast<int32_t>(1 - i));
    for (size_t m = 0; m < now.moons.size(); ++m) {
      accumulate(now.moons[m], first_moon_body_index + static_cast<int32_t>(m));
    }

    const double distance_au = view.distance_km / au_km;
    const double unoccluded = stars_[i].illuminance_at_1au_lx / (distance_au * distance_au);
    view.unocculted_lx = unoccluded * view.horizon_fraction;
    view.illuminance_lx = view.unocculted_lx * (1.0 - view.occluded_fraction);
    view.space_illuminance_lx = unoccluded * (1.0 - view.occluded_fraction);
  }

  for (size_t m = 0; m < state.moons.size(); ++m) {
    auto& view = state.moons[m];
    const auto& moon = config_.moons[m];
    view.name = moon.name;
    view.kind = body_kind::moon;
    fill_geometry(view, now.moons[m].position, now.moons[m].radius_km);

    const auto moon_to_observer = now.observer - now.moons[m].position;
    const double observer_distance = vector_length(moon_to_observer);
    const double solid_angle_factor =
      (moon.radius_km / observer_distance) * (moon.radius_km / observer_distance);

    double total_illuminance = 0.0;
    double unocculted_total = 0.0;
    double unoccluded_illumination = 0.0;
    double received_illumination = 0.0;
    double deepest_occlusion = 0.0;
    auto weighted_color = glm::dvec3(0.0, 0.0, 0.0);
    double dominant_illumination = 0.0;
    double dominant_phase_angle = pi;

    for (size_t s = 0; s < state.stars.size(); ++s) {
      const auto illumination = star_illuminance_at(s, now.moons[m].position, now, static_cast<int32_t>(m));
      unoccluded_illumination += illumination.unoccluded_lx;
      received_illumination += illumination.illuminance_lx;
      if (illumination.occluded_fraction > deepest_occlusion) {
        deepest_occlusion = illumination.occluded_fraction;
        view.occluder = illumination.occluder;
      }

      const auto moon_to_star = now.stars[s].position - now.moons[m].position;
      const double phase_angle = angle_between(moon_to_star, moon_to_observer);
      // Диск-интегральная фазовая функция ламбертовой сферы: в полной фазе даёт 2/3.
      const double phase_function =
        (std::sin(phase_angle) + (pi - phase_angle) * std::cos(phase_angle)) / pi;
      const double geometry = moon.albedo * solid_angle_factor * (2.0 / 3.0) * std::max(0.0, phase_function);

      // Обе ветви считаются из своих освещённостей, а не делением одной на другую: при полном
      // затмении фактическая обращается в ноль, и отношение потеряло бы именно тот случай,
      // ради которого эта величина и нужна.
      total_illuminance += illumination.illuminance_lx * geometry;
      unocculted_total += illumination.unoccluded_lx * geometry;
      if (illumination.illuminance_lx <= 0.0) continue;

      weighted_color += stars_[s].color_linear * illumination.illuminance_lx * geometry;
      if (illumination.illuminance_lx > dominant_illumination) {
        dominant_illumination = illumination.illuminance_lx;
        dominant_phase_angle = phase_angle;
      }
    }

    // Для луны «затмение» — это суммарная по обоим светилам потеря освещения, а не перекрытие
    // её собственного диска: именно так лунное затмение читается наблюдателем.
    view.occluded_fraction =
      unoccluded_illumination > 0.0 ? 1.0 - received_illumination / unoccluded_illumination : 0.0;
    view.phase = 0.5 * (1.0 + std::cos(dominant_phase_angle));
    view.illuminance_lx = total_illuminance * view.horizon_fraction;
    view.unocculted_lx = unocculted_total * view.horizon_fraction;
    view.space_illuminance_lx = total_illuminance;

    const double color_maximum = std::max({weighted_color.x, weighted_color.y, weighted_color.z});
    view.color_linear = color_maximum > 0.0 ? weighted_color / color_maximum : glm::dvec3(1.0, 1.0, 1.0);
  }

  const auto add_horizontal = [&](const body_view& view, double& actual, double& unocculted) {
    if (view.altitude_deg <= 0.0) return;
    const double projection = std::sin(to_radians(view.altitude_deg));
    actual += view.illuminance_lx * projection;
    unocculted += view.unocculted_lx * projection;
  };
  for (const auto& view : state.stars) {
    add_horizontal(view, state.star_illuminance_lx, state.unocculted_star_illuminance_lx);
  }
  for (const auto& view : state.moons) {
    add_horizontal(view, state.moon_illuminance_lx, state.unocculted_moon_illuminance_lx);
  }
  state.horizontal_illuminance_lx = state.star_illuminance_lx + state.moon_illuminance_lx;

  return state;
}

bool parse_system_config(const std::string& text, system_config& out, std::string& diagnostics) {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();

  tavl::ct_context context;
  out = system_config{};
  tavl::deserialize(parser, context, out);

  if (context.diagnostics.empty()) return true;

  diagnostics.clear();
  for (const auto& entry : context.diagnostics) {
    diagnostics += std::format("  {} at {}:{} field '{}'\n", tavl::to_string(entry.error.type), entry.error.span.line,
                               entry.error.span.column, entry.field);
  }
  return false;
}

} // namespace devils_engine::pf07
