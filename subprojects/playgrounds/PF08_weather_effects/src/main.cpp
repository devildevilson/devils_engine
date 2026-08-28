// PF08 — weather effects. Небесная механика, календарные инструменты и запуск визуальной сцены
// сходятся здесь в одном executable, чтобы отчёт и рендер всегда вычисляли одно и то же небо.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

#include "celestial.h"
#include "survey.h"
#include "sky_view.h"
#include "weather.h"

using namespace devils_engine;

namespace {

constexpr double default_event_days = 90.0;
constexpr double default_event_step_minutes = 2.0;
constexpr double eclipse_threshold = 1e-4;

enum class action : uint32_t { render, report, events, verify, survey };

struct options {
  action requested = action::render;
  pf08::view_options view;
  pf08::survey_options survey;
  std::string config_path = std::string(PF08_RESOURCE_ROOT) + "/celestial/system.tavl";
  double time_days = 0.0;
  double span_days = default_event_days;
  double step_minutes = default_event_step_minutes;
};

bool read_prefixed(const std::string_view argument, const std::string_view prefix, std::string& out) {
  if (!argument.starts_with(prefix)) return false;
  out = std::string(argument.substr(prefix.size()));
  return true;
}

void print_usage() {
  std::cout << "PF08 weather effects\n"
               "  --render            окно с физическим небом (по умолчанию)\n"
               "  --report            эфемериды на момент --time\n"
               "  --events            таблица событий на --days вперёд\n"
               "  --verify            численные инварианты, ненулевой код возврата при провале\n"
               "  --survey            сезоны, суточная структура, парады, двойные затмения, луны днём\n"
               "  --time=DAYS         игровое время в сутках от эпохи\n"
               "  --days=DAYS         длина окна сканирования событий\n"
               "  --step=MINUTES      шаг сканирования событий\n"
               "  --config=PATH       путь к tavl-описанию системы\n"
               "  --budget-days=N     окно бюджета событий\n"
               "  --lunar-threshold=F доля потерянного света луны, начиная с которой это событие\n"
               "  --star-threshold=F  доля перекрытия диска светила, начиная с которой это событие\n"
               "  --game-minutes=F    игровых минут за реальную секунду\n"
               "  --small-dimming=F   доля потери света, с которой явление считается малым\n"
               "  --large-dimming=F   доля потери света, с которой явление считается крупным\n"
               "  --validation        включить слои валидации Vulkan\n"
               "  --frames=N          закрыть окно после N кадров\n"
               "  --dump=PATH         сохранить последний кадр в ppm\n"
               "  --no-overlay        не рисовать диагностический текст (для эталонных кадров)\n"
               "  --width=N --height=N  размер окна\n"
               "  --time-scale=F      игровых суток за реальную секунду\n"
               "  --ev=F              зафиксировать EV100 вместо адаптации\n"
               "  --ev-bias=F         экспокоррекция в стопах\n"
               "  --preset=NAME       именованное состояние: noon, double_sunset, night, eclipse\n"
               "  --weather=NAME      состояние: clear, haze, windy, fog, cloudy, overcast, rain\n"
               "  --weather-transition=S  длительность runtime-перехода по T, реальные секунды\n"
               "  --night-vision=F    сила ночного зрения, 0 отключает\n"
               "  --turbidity=F       множитель аэрозоля\n"
               "  --fog-extinction=F  extinction локального тумана, 1/м (0 = точный clear)\n"
               "  --fog-albedo=F      альбедо рассеяния локального тумана, 0..1\n"
               "  --fog-anisotropy=F  HG-анизотропия локального тумана, -0.85..0.85\n"
               "  --fog-base=M        высота начала экспоненциального спада плотности\n"
               "  --fog-height=M      scale height туманного слоя\n"
               "  --fog-variation=F   амплитуда world-space неоднородности, 0..0.95\n"
               "  --fog-cell=M        размер низкочастотной ячейки плотности\n"
               "  --fog-speed=MPS     скорость advection ячеек по направлению ветра\n"
               "  --fog-range=M       дальность froxel-объёма в метрах\n"
               "  --cloud-coverage=F  доля покрытия облачного слоя, 0..1\n"
               "  --cloud-extinction=F extinction облака, 1/м\n"
               "  --cloud-albedo=F    альбедо рассеяния облака, 0..1\n"
               "  --cloud-anisotropy=F HG-анизотропия облака, -0.85..0.85\n"
               "  --cloud-base=M --cloud-top=M  границы облачного слоя\n"
               "  --cloud-cell=M      размер world-space облачной ячейки\n"
               "  --cloud-speed=MPS   скорость переноса облаков ветром\n"
               "  --cloud-range=M     дальность облачного froxel-интеграла\n"
               "  --rain-rate=MMH     интенсивность дождя в мм/ч (0 = точный bypass)\n"
               "  --rain-speed=MPS    вертикальная скорость падения капель\n"
               "  --rain-wind=MPS     горизонтальный перенос капель общим ветром\n"
               "  --rain-length=M     длина ближнего streak в мировых метрах\n"
               "  --rain-radius=M     радиус ближнего particle-объёма\n"
               "  --rain-extinction=F дальнее extinction дождя, 1/м\n"
               "  --rain-range=M      дальность froxel-представления дождя\n"
               "  --no-rain-particles отключить ближние streaks/impacts для A/B\n"
               "  --no-rain-collision отключить depth contacts для A/B\n"
               "  --debug=8|9|10      transmittance объёма | cloud density | cloud shadow\n"
               "  --march-steps=N     шагов основного марша неба\n"
               "  --aerial-range=KM   дальность таблицы воздушной перспективы\n"
               "  --camera-height=KM  высота наблюдателя над поверхностью\n"
               "  --look-azimuth=F --look-altitude=F  фиксированное наведение камеры, градусы\n"
               "  --disc-scale=F      преувеличение размера дисков светил и лун\n"
               "  --star-density=F --star-brightness=F  звёздное поле\n"
               "  --adaptation=F      полнота адаптации экспозиции (1 = всё средне-серое, 0 = фикс)\n"
               "  --trace-exposure    покадровая печать экспозиции: цель, текущая, отставание\n"
               "  --shadow-far=M      дальность каскадов теней в метрах\n"
               "  --shadow-sources=N  сколько светил отбрасывают тени (0..2), для сравнения кадров\n"
               "  --wind=M            сила ветра: отклонение верхушки куста в метрах\n"
               "  --wind-direction=D  направление ветра в градусах от севера через восток\n"
               "  --foliage=N         число кустов\n"
               "  --foliage-range=M   дальность отрисовки зарослей в метрах\n"
               "  --foliage-lod=M     граница между полным и упрощённым мешем куста\n"
               "  --foliage-shadow-cascades=N  в скольких ближних каскадах трава даёт тень\n"
               "  --cascade-split=F   доля логарифмического разбиения каскадов (1 лог, 0 равномерное)\n"
               "  --year=N            абсолютный календарный год, от единицы\n"
               "  --day=D             сутки внутри года, от нуля\n"
               "  --hour=H            час внутри суток\n"
               "  --corona=F          яркость короны светил в долях физической\n"
               "  --galaxy=F          сила сгущения звёзд в галактической полосе\n"
               "  --star-rotation=F   доля физической скорости вращения неба (1 — честная, 0 — статика)\n";
}

bool parse_options(const int argc, char** argv, options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::string value;

    if (argument == "--render") {
      out.requested = action::render;
    } else if (argument == "--report") {
      out.requested = action::report;
    } else if (argument == "--events") {
      out.requested = action::events;
    } else if (argument == "--verify") {
      out.requested = action::verify;
    } else if (argument == "--survey") {
      out.requested = action::survey;
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      return false;
    } else if (read_prefixed(argument, "--config=", value)) {
      out.config_path = value;
    } else if (read_prefixed(argument, "--time=", value)) {
      out.time_days = std::stod(value);
    } else if (read_prefixed(argument, "--days=", value)) {
      out.span_days = std::stod(value);
    } else if (read_prefixed(argument, "--step=", value)) {
      out.step_minutes = std::stod(value);
    } else if (read_prefixed(argument, "--budget-days=", value)) {
      out.survey.budget_span_days = std::stod(value);
    } else if (read_prefixed(argument, "--lunar-threshold=", value)) {
      out.survey.min_lunar_eclipse = std::stod(value);
    } else if (read_prefixed(argument, "--star-threshold=", value)) {
      out.survey.min_star_occultation = std::stod(value);
    } else if (read_prefixed(argument, "--game-minutes=", value)) {
      out.survey.game_minutes_per_real_second = std::stod(value);
    } else if (read_prefixed(argument, "--small-dimming=", value)) {
      out.survey.small_effect_dimming = std::stod(value);
    } else if (read_prefixed(argument, "--large-dimming=", value)) {
      out.survey.large_effect_dimming = std::stod(value);
    } else if (argument == "--validation") {
      out.view.validation = true;
    } else if (argument == "--uncapped") {
      out.view.uncapped = true;
    } else if (argument == "--pause") {
      out.view.paused = true;
    } else if (read_prefixed(argument, "--frames=", value)) {
      out.view.frames = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--dump=", value)) {
      out.view.dump_path = value;
    } else if (read_prefixed(argument, "--width=", value)) {
      out.view.width = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--height=", value)) {
      out.view.height = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--time-scale=", value)) {
      out.view.time_scale = std::stod(value);
    } else if (read_prefixed(argument, "--ev=", value)) {
      out.view.exposure.manual_ev100 = std::stod(value);
      out.view.exposure.manual = true;
    } else if (read_prefixed(argument, "--wind=", value)) {
      out.view.wind_strength_m = std::stod(value);
      out.view.wind_strength_overridden = true;
    } else if (read_prefixed(argument, "--wind-direction=", value)) {
      out.view.wind_direction_deg = std::stod(value);
      out.view.wind_direction_overridden = true;
    } else if (read_prefixed(argument, "--foliage=", value)) {
      out.view.foliage_count = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--foliage-range=", value)) {
      out.view.foliage_range_m = std::stod(value);
    } else if (read_prefixed(argument, "--foliage-lod=", value)) {
      out.view.foliage_lod_m = std::stod(value);
    } else if (read_prefixed(argument, "--foliage-shadow-cascades=", value)) {
      out.view.foliage_shadow_cascades = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--year=", value)) {
      out.view.start_year = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--day=", value)) {
      out.view.start_day_of_year = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--hour=", value)) {
      out.view.start_hour = std::stod(value);
    } else if (read_prefixed(argument, "--cascade-split=", value)) {
      out.view.cascade_split_lambda = std::stod(value);
    } else if (read_prefixed(argument, "--shadow-sources=", value)) {
      out.view.shadow_sources = uint32_t(std::stoul(value));
    } else if (read_prefixed(argument, "--shadow-far=", value)) {
      out.view.shadow_far_m = std::stod(value);
    } else if (argument == "--trace-exposure") {
      out.view.trace_exposure = true;
    } else if (argument == "--no-overlay") {
      out.view.show_overlay = false;
    } else if (read_prefixed(argument, "--adaptation=", value)) {
      out.view.exposure.adaptation_strength = std::stod(value);
    } else if (read_prefixed(argument, "--ev-bias=", value)) {
      out.view.exposure.bias_stops = std::stod(value);
    } else if (read_prefixed(argument, "--night-vision=", value)) {
      out.view.output.scotopic_strength = std::stod(value);
    } else if (read_prefixed(argument, "--preset=", value)) {
      out.view.preset = value;
    } else if (read_prefixed(argument, "--weather=", value)) {
      out.view.weather_preset = value;
    } else if (read_prefixed(argument, "--weather-transition=", value)) {
      out.view.weather_transition_seconds = std::stod(value);
      out.view.weather_transition_overridden = true;
    } else if (read_prefixed(argument, "--turbidity=", value)) {
      out.view.atmosphere.turbidity = std::stod(value);
      out.view.turbidity_overridden = true;
    } else if (read_prefixed(argument, "--fog-extinction=", value)) {
      out.view.fog_extinction_per_m = std::stod(value);
      out.view.fog_extinction_overridden = true;
    } else if (read_prefixed(argument, "--fog-albedo=", value)) {
      out.view.fog_scattering_albedo = std::stod(value);
      out.view.fog_albedo_overridden = true;
    } else if (read_prefixed(argument, "--fog-anisotropy=", value)) {
      out.view.fog_anisotropy = std::stod(value);
      out.view.fog_anisotropy_overridden = true;
    } else if (read_prefixed(argument, "--fog-base=", value)) {
      out.view.fog_base_height_m = std::stod(value);
      out.view.fog_base_overridden = true;
    } else if (read_prefixed(argument, "--fog-height=", value)) {
      out.view.fog_scale_height_m = std::stod(value);
      out.view.fog_height_overridden = true;
    } else if (read_prefixed(argument, "--fog-variation=", value)) {
      out.view.fog_density_variation = std::stod(value);
      out.view.fog_variation_overridden = true;
    } else if (read_prefixed(argument, "--fog-cell=", value)) {
      out.view.fog_cell_size_m = std::stod(value);
      out.view.fog_cell_overridden = true;
    } else if (read_prefixed(argument, "--fog-speed=", value)) {
      out.view.fog_advection_speed_m_s = std::stod(value);
      out.view.fog_speed_overridden = true;
    } else if (read_prefixed(argument, "--fog-range=", value)) {
      out.view.fog_range_m = std::stod(value);
    } else if (read_prefixed(argument, "--cloud-coverage=", value)) {
      out.view.cloud_coverage = std::stod(value);
      out.view.cloud_coverage_overridden = true;
    } else if (read_prefixed(argument, "--cloud-extinction=", value)) {
      out.view.cloud_extinction_per_m = std::stod(value);
      out.view.cloud_extinction_overridden = true;
    } else if (read_prefixed(argument, "--cloud-albedo=", value)) {
      out.view.cloud_scattering_albedo = std::stod(value);
      out.view.cloud_albedo_overridden = true;
    } else if (read_prefixed(argument, "--cloud-anisotropy=", value)) {
      out.view.cloud_anisotropy = std::stod(value);
      out.view.cloud_anisotropy_overridden = true;
    } else if (read_prefixed(argument, "--cloud-base=", value)) {
      out.view.cloud_base_height_m = std::stod(value);
      out.view.cloud_base_overridden = true;
    } else if (read_prefixed(argument, "--cloud-top=", value)) {
      out.view.cloud_top_height_m = std::stod(value);
      out.view.cloud_top_overridden = true;
    } else if (read_prefixed(argument, "--cloud-cell=", value)) {
      out.view.cloud_cell_size_m = std::stod(value);
      out.view.cloud_cell_overridden = true;
    } else if (read_prefixed(argument, "--cloud-speed=", value)) {
      out.view.cloud_advection_speed_m_s = std::stod(value);
      out.view.cloud_speed_overridden = true;
    } else if (read_prefixed(argument, "--cloud-range=", value)) {
      out.view.cloud_range_m = std::stod(value);
    } else if (read_prefixed(argument, "--rain-rate=", value)) {
      out.view.rain_rate_mm_h = std::stod(value);
      out.view.rain_rate_overridden = true;
    } else if (read_prefixed(argument, "--rain-speed=", value)) {
      out.view.rain_fall_speed_m_s = std::stod(value);
      out.view.rain_fall_speed_overridden = true;
    } else if (read_prefixed(argument, "--rain-wind=", value)) {
      out.view.rain_wind_speed_m_s = std::stod(value);
      out.view.rain_wind_speed_overridden = true;
    } else if (read_prefixed(argument, "--rain-length=", value)) {
      out.view.rain_drop_length_m = std::stod(value);
      out.view.rain_drop_length_overridden = true;
    } else if (read_prefixed(argument, "--rain-radius=", value)) {
      out.view.rain_near_radius_m = std::stod(value);
      out.view.rain_near_radius_overridden = true;
    } else if (read_prefixed(argument, "--rain-extinction=", value)) {
      out.view.rain_far_extinction_per_m = std::stod(value);
      out.view.rain_far_extinction_overridden = true;
    } else if (read_prefixed(argument, "--rain-range=", value)) {
      out.view.rain_range_m = std::stod(value);
    } else if (argument == "--no-rain-particles") {
      out.view.rain_particles = false;
    } else if (argument == "--no-rain-collision") {
      out.view.rain_collision = false;
    } else if (read_prefixed(argument, "--march-steps=", value)) {
      out.view.march.primary_steps = int32_t(std::stol(value));
    } else if (read_prefixed(argument, "--camera-height=", value)) {
      out.view.march.camera_height_km = std::stod(value);
    } else if (read_prefixed(argument, "--aerial-range=", value)) {
      out.view.march.aerial_range_km = std::stod(value);
    } else if (read_prefixed(argument, "--debug=", value)) {
      out.view.output.debug_mode = std::stod(value);
    } else if (read_prefixed(argument, "--look-azimuth=", value)) {
      out.view.look_azimuth_deg = std::stod(value);
      out.view.fixed_look = true;
    } else if (read_prefixed(argument, "--disc-scale=", value)) {
      out.view.output.disc_scale = std::stod(value);
    } else if (read_prefixed(argument, "--star-density=", value)) {
      out.view.output.star_density = std::stod(value);
    } else if (read_prefixed(argument, "--star-brightness=", value)) {
      out.view.output.star_brightness = std::stod(value);
    } else if (read_prefixed(argument, "--corona=", value)) {
      out.view.output.corona_strength = std::stod(value);
    } else if (read_prefixed(argument, "--galaxy=", value)) {
      out.view.output.galaxy_concentration = std::stod(value);
    } else if (read_prefixed(argument, "--star-rotation=", value)) {
      out.view.output.star_rotation_scale = std::stod(value);
    } else if (read_prefixed(argument, "--look-altitude=", value)) {
      out.view.look_altitude_deg = std::stod(value);
      out.view.fixed_look = true;
    } else {
      utils::warn("PF08 unknown argument '{}'", argument);
      print_usage();
      return false;
    }
  }
  return true;
}

// Календарь берётся у небесной механики: эпоха полуночи и длина года принадлежат ей, и повторять их
// здесь значило бы развести часы отчёта с часами окна.
std::string format_clock(const pf08::celestial_system& system, const double time_days) {
  const auto now = system.to_calendar(time_days);
  return std::format("г{} д{:>3} {:02}:{:02}", now.year, now.day, now.hour, now.minute);
}

std::string body_name(const pf08::celestial_system& system, const int32_t index) {
  if (index < 0) return "-";
  if (index == 0) return system.config().primary.name;
  if (index == 1) return system.config().companion.name;
  if (index == 2) return "planet";
  const size_t moon = static_cast<size_t>(index - 3);
  if (moon < system.config().moons.size()) return system.config().moons[moon].name;
  return "?";
}

void print_system(const pf08::celestial_system& system) {
  const auto& config = system.config();
  std::cout << "== система ==\n";

  for (size_t i = 0; i < 2; ++i) {
    const auto& source = i == 0 ? config.primary : config.companion;
    const auto& derived = system.star(i);
    std::cout << std::format(
      "  {:<8} M={:.2f} R={:.2f} T={:.0f}K  L={:.4f} L_sun  отдача={:.1f} лм/Вт  E(1 а.е.)={:.0f} лк"
      "  цвет=({:.3f} {:.3f} {:.3f})\n",
      source.name, source.mass_solar, source.radius_solar, source.temperature_kelvin, derived.luminosity_solar,
      derived.luminous_efficacy_lm_per_w, derived.illuminance_at_1au_lx, derived.color_linear.x, derived.color_linear.y,
      derived.color_linear.z);
  }

  const double visible_ratio =
    system.star(1).illuminance_at_1au_lx / system.star(0).illuminance_at_1au_lx;
  std::cout << std::format("  видимое отношение освещённостей компаньона к главной звезде: {:.3f}\n", visible_ratio);
  std::cout << std::format("  период двойной {:.2f} сут, год планеты {:.2f} сут, соединения каждые {:.2f} сут\n",
                           system.binary_period_days(), system.planet_year_days(),
                           system.star_conjunction_interval_days());
  std::cout << std::format("  beat двойной: {} лет, остаточная ошибка {:.2f} сут; луны в beat не входят\n",
                           system.binary_beat_years(), system.binary_beat_error_days());
  std::cout << std::format("  устойчивость: a_планеты / a_двойной = {:.2f} (критерий Холмана-Вигерта > ~3)\n",
                           config.planet.orbit.semi_major_axis / config.binary.semi_major_axis);

  for (size_t i = 0; i < config.moons.size(); ++i) {
    std::cout << std::format("  луна {:<8} a={:.0f} тыс. км  период {:.3f} сут  R={:.0f} км  альбедо {:.2f}\n",
                             config.moons[i].name, config.moons[i].orbit.semi_major_axis / 1000.0,
                             system.moon_period_days(i), config.moons[i].radius_km, config.moons[i].albedo);
  }
}

void print_state(const pf08::celestial_system& system, const pf08::sky_state& state) {
  std::cout << std::format("\n== небо на {} (t = {:.4f} сут) ==\n", format_clock(system, state.time_days), state.time_days);
  std::cout << "  тело      высота  азимут  угл.радиус  расстояние      освещ., лк  фаза   затмение\n";

  const auto print_body = [&](const pf08::body_view& view) {
    const std::string distance = view.kind == pf08::body_kind::star
                                   ? std::format("{:10.4f} а.е.", view.distance_km / 149597870.7)
                                   : std::format("{:10.0f} тыс.км", view.distance_km / 1000.0);
    const std::string eclipse =
      view.occluded_fraction > eclipse_threshold
        ? std::format("{:.3f} ({})", view.occluded_fraction, body_name(system, view.occluder))
        : std::string("-");
    std::cout << std::format("  {:<9} {:6.2f}  {:6.2f}  {:9.4f}  {}  {:14.5g}  {:5.3f}  {}\n", view.name,
                             view.altitude_deg, view.azimuth_deg, view.angular_radius_deg, distance,
                             view.illuminance_lx, view.phase, eclipse);
  };

  for (const auto& view : state.stars) {
    print_body(view);
  }
  for (const auto& view : state.moons) {
    print_body(view);
  }

  std::cout << std::format("  горизонтальная освещённость вне атмосферы: {:.6g} лк\n",
                           state.horizontal_illuminance_lx);

  const double star_sum = state.stars[0].illuminance_lx + state.stars[1].illuminance_lx;
  if (star_sum > 0.0) {
    // Контраст второй тени: доля, которую компаньон вносит в полное освещение площадки. Небо
    // здесь ещё не участвует, поэтому это верхняя оценка до появления атмосферы в срезе 2.
    std::cout << std::format("  вклад компаньона в прямой свет: {:.1f}% (контраст второй тени без неба)\n",
                             100.0 * state.stars[1].illuminance_lx / star_sum);
  }
}

// --- сканер событий ---

struct event_track {
  bool active = false;
  double begin_days = 0.0;
  double peak_days = 0.0;
  double peak_value = 0.0;
  int32_t peak_occluder = -1;
};

// Обход событий начинается с ЗАДАННОГО момента, а не с нуля. Иначе «покажи третий год»
// невозможно спросить вовсе: календарь на выводе показывал бы третий год, а сканировался первый.
void scan_events(const pf08::celestial_system& system, const double start_days, const double span_days,
                 const double step_minutes, const pf08::survey_options& options) {
  const double step_days = step_minutes / (24.0 * 60.0);
  const size_t moon_count = system.config().moons.size();

  std::vector<event_track> star_eclipse(2);
  std::vector<event_track> moon_eclipse(moon_count);
  std::vector<event_track> moon_occultation(moon_count);
  event_track parade;
  std::vector<double> previous_star_altitude(2, 0.0);
  bool has_previous = false;
  bool unique_active = false;
  uint32_t unique_count = 0;
  pf08::observable_event_state observed;

  const auto start_calendar = system.to_calendar(start_days);
  std::cout << std::format("\n== события на {:.1f} сут от г{} д{}, шаг {:.1f} мин ==\n", span_days,
                           start_calendar.year, start_calendar.day, step_minutes);

  const auto close_track = [&](event_track& track, const double now, const std::string_view kind,
                               const std::string_view subject) {
    if (!track.active) return;
    std::cout << std::format("  {}  {}: {:<8} максимум {:.3f} ({}) в {}, длительность {:.2f} ч\n",
                             format_clock(system, track.begin_days), kind, subject, track.peak_value,
                             body_name(system, track.peak_occluder), format_clock(system, track.peak_days),
                             (now - track.begin_days) * 24.0);
    track = event_track{};
  };

  const auto update_track = [&](event_track& track, const double now, const double value, const int32_t occluder,
                                const std::string_view kind, const std::string_view subject) {
    if (value > eclipse_threshold) {
      if (!track.active) {
        track.active = true;
        track.begin_days = now;
        track.peak_value = 0.0;
      }
      if (value > track.peak_value) {
        track.peak_value = value;
        track.peak_days = now;
        track.peak_occluder = occluder;
      }
      return;
    }
    close_track(track, now, kind, subject);
  };

  const auto close_parade = [&](const double now) {
    if (!parade.active) return;
    std::cout << std::format("  {}  парад: все тела в пределах {:.2f}° в {}, длительность {:.2f} ч\n",
                             format_clock(system, parade.begin_days), parade.peak_value,
                             format_clock(system, parade.peak_days), (now - parade.begin_days) * 24.0);
    parade = event_track{};
  };

  for (double now = start_days; now <= start_days + span_days; now += step_days) {
    const auto state = system.evaluate(now);
    pf08::observe_events(state, options, observed);

    for (size_t i = 0; i < state.stars.size(); ++i) {
      const auto& view = state.stars[i];
      if (has_previous && previous_star_altitude[i] < 0.0 && view.altitude_deg >= 0.0) {
        std::cout << std::format("  {}  восход  {}\n", format_clock(system, now), view.name);
      }
      if (has_previous && previous_star_altitude[i] >= 0.0 && view.altitude_deg < 0.0) {
        std::cout << std::format("  {}  закат   {}\n", format_clock(system, now), view.name);
      }
      previous_star_altitude[i] = view.altitude_deg;

      const double visible_eclipse = observed.star_occultation[i];
      update_track(star_eclipse[i], now, visible_eclipse, view.occluder, "затмение", view.name);
    }

    for (size_t m = 0; m < moon_count; ++m) {
      const auto& view = state.moons[m];
      update_track(moon_eclipse[m], now, observed.lunar_eclipse[m], view.occluder, "лунное  ", view.name);
    }

    for (size_t far_index = 0; far_index < moon_count; ++far_index) {
      const auto& far = state.moons[far_index];
      update_track(moon_occultation[far_index], now, observed.moon_occultation[far_index],
                   observed.moon_occulting_body[far_index], "покрытие ", far.name);
    }
    if (observed.parade) {
      if (!parade.active) {
        parade.active = true;
        parade.begin_days = now;
        parade.peak_value = observed.parade_spread_deg;
        parade.peak_days = now;
      } else if (observed.parade_spread_deg < parade.peak_value) {
        parade.peak_value = observed.parade_spread_deg;
        parade.peak_days = now;
      }
    } else {
      close_parade(now);
    }

    if (observed.any() && !unique_active) ++unique_count;
    unique_active = observed.any();

    has_previous = true;
  }

  for (size_t i = 0; i < star_eclipse.size(); ++i) {
    close_track(star_eclipse[i], start_days + span_days, "затмение",
                i == 0 ? system.config().primary.name : system.config().companion.name);
  }
  for (size_t m = 0; m < moon_count; ++m) {
    close_track(moon_eclipse[m], start_days + span_days, "лунное  ", system.config().moons[m].name);
    close_track(moon_occultation[m], start_days + span_days, "покрытие ", system.config().moons[m].name);
  }
  close_parade(start_days + span_days);
  std::cout << std::format("  итого уникальных наблюдаемых небесных эпизодов: {}\n", unique_count);
}

// --- численные проверки ---

class checker {
public:
  void expect(const bool condition, const std::string_view name, const std::string& detail = {}) {
    ++total_;
    if (condition) return;
    ++failed_;
    std::cout << std::format("  ПРОВАЛ  {}{}{}\n", name, detail.empty() ? "" : ": ", detail);
  }

  void expect_near(const double value, const double reference, const double tolerance, const std::string_view name) {
    const double difference = std::abs(value - reference);
    expect(difference <= tolerance, name, std::format("{:.17g} против {:.17g}, отклонение {:.3g}", value, reference,
                                                      difference));
  }

  bool passed() const { return failed_ == 0; }
  size_t total() const { return total_; }
  size_t failed() const { return failed_; }

private:
  size_t total_ = 0;
  size_t failed_ = 0;
};

bool identical_bits(const double a, const double b) {
  return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}

bool identical_state(const pf08::sky_state& a, const pf08::sky_state& b) {
  if (a.moons.size() != b.moons.size()) return false;
  if (!identical_bits(a.horizontal_illuminance_lx, b.horizontal_illuminance_lx)) return false;

  const auto same_body = [](const pf08::body_view& x, const pf08::body_view& y) {
    return identical_bits(x.altitude_deg, y.altitude_deg) && identical_bits(x.azimuth_deg, y.azimuth_deg) &&
           identical_bits(x.angular_radius_deg, y.angular_radius_deg) &&
           identical_bits(x.illuminance_lx, y.illuminance_lx) &&
           identical_bits(x.unocculted_lx, y.unocculted_lx) &&
           identical_bits(x.space_unocculted_lx, y.space_unocculted_lx) &&
           identical_bits(x.space_illuminance_lx, y.space_illuminance_lx) &&
           identical_bits(x.occluded_fraction, y.occluded_fraction) && identical_bits(x.phase, y.phase) &&
           identical_bits(x.star_visibility[0], y.star_visibility[0]) &&
           identical_bits(x.star_visibility[1], y.star_visibility[1]);
  };

  for (size_t i = 0; i < a.stars.size(); ++i) {
    if (!same_body(a.stars[i], b.stars[i])) return false;
  }
  for (size_t i = 0; i < a.moons.size(); ++i) {
    if (!same_body(a.moons[i], b.moons[i])) return false;
  }
  return true;
}

void verify_kepler(checker& check) {
  double worst = 0.0;
  for (int32_t e_step = 0; e_step <= 60; ++e_step) {
    const double eccentricity = e_step / 100.0;
    for (int32_t m_step = 0; m_step < 360; ++m_step) {
      const double mean_anomaly = m_step * std::numbers::pi / 180.0;
      const double eccentric = pf08::solve_kepler(mean_anomaly, eccentricity);
      const double residual = std::abs(eccentric - eccentricity * std::sin(eccentric) - mean_anomaly);
      worst = std::max(worst, residual);
    }
  }
  check.expect(worst < 1e-12, "уравнение Кеплера решено", std::format("худшая невязка {:.3g}", worst));
}

void verify_occlusion(checker& check) {
  constexpr double source = 0.0045;
  constexpr double smaller = 0.0030;
  constexpr double larger = 0.0080;

  check.expect_near(pf08::disk_occluded_fraction(0.0, source, larger), 1.0, 0.0,
                    "полное перекрытие большим диском");
  check.expect_near(pf08::disk_occluded_fraction(0.0, source, smaller), (smaller / source) * (smaller / source), 1e-15,
                    "кольцевое перекрытие даёт отношение площадей");
  check.expect_near(pf08::disk_occluded_fraction(source + larger, source, larger), 0.0, 0.0,
                    "нет перекрытия при касании");
  check.expect_near(pf08::disk_occluded_fraction(0.5 * (source + smaller), source, smaller),
                    pf08::disk_occluded_fraction(0.5 * (source + smaller), source, smaller), 0.0,
                    "функция перекрытия детерминирована");

  double previous = 1.1;
  bool monotone = true;
  for (int32_t i = 0; i <= 200; ++i) {
    const double separation = (source + larger) * i / 200.0;
    const double value = pf08::disk_occluded_fraction(separation, source, larger);
    if (value > previous + 1e-15) monotone = false;
    previous = value;
  }
  check.expect(monotone, "перекрытие монотонно убывает с расстоянием между дисками");

  check.expect_near(pf08::disk_fraction_above_horizon(0.0, source), 0.5, 1e-15, "половина диска на горизонте");
  check.expect_near(pf08::disk_fraction_above_horizon(source, source), 1.0, 0.0, "диск целиком взошёл");
  check.expect_near(pf08::disk_fraction_above_horizon(-source, source), 0.0, 0.0, "диск целиком зашёл");
}

void verify_stability(checker& check, const pf08::celestial_system& system) {
  // Опорные значения Холмана-Вигерта: круговая двойная равных масс требует примерно 2.3-2.4 своих
  // больших полуосей, и требование растёт с эксцентриситетом.
  check.expect_near(pf08::holman_wiegert_critical_ratio(0.0, 0.3), 2.378, 0.02, "критерий устойчивости, e=0");
  check.expect(pf08::holman_wiegert_critical_ratio(0.3, 0.3) > pf08::holman_wiegert_critical_ratio(0.0, 0.3),
               "эксцентричная двойная требует более далёкой планеты");
  check.expect(system.stability_margin() > 1.0, "сконфигурированная орбита планеты устойчива",
               std::format("запас {:.3f}", system.stability_margin()));

  // Медленная двойная и широко разнесённые солнца — одно требование: и то и другое упирается в тот же
  // критический радиус, поэтому число оборотов за год снизу ограничено.
  const double mass_ratio =
    system.config().companion.mass_solar / (system.config().primary.mass_solar + system.config().companion.mass_solar);
  const double floor_revolutions =
    std::pow(pf08::holman_wiegert_critical_ratio(system.config().binary.eccentricity, mass_ratio), 1.5);
  check.expect(system.binary_revolutions_per_year() > floor_revolutions,
               "двойная делает больше оборотов за год, чем позволяет предел устойчивости",
               std::format("{:.2f} против предела {:.2f}", system.binary_revolutions_per_year(), floor_revolutions));
}

void verify_photometry(checker& check, const pf08::celestial_system& system) {
  const double efficacy_primary = system.star(0).luminous_efficacy_lm_per_w;
  const double efficacy_companion = system.star(1).luminous_efficacy_lm_per_w;
  check.expect(efficacy_primary > 90.0 && efficacy_primary < 95.0, "световая отдача солнечного спектра",
               std::format("{:.2f} лм/Вт", efficacy_primary));
  check.expect(efficacy_companion < efficacy_primary, "у более холодной звезды отдача ниже",
               std::format("{:.2f} против {:.2f}", efficacy_companion, efficacy_primary));

  const auto hot = pf08::blackbody_color_linear(9000.0);
  const auto cold = pf08::blackbody_color_linear(3500.0);
  check.expect(hot.z / hot.x > cold.z / cold.x, "горячая звезда синее холодной",
               std::format("{:.3f} против {:.3f}", hot.z / hot.x, cold.z / cold.x));
}

void verify_dynamics(checker& check, const pf08::celestial_system& system) {
  const auto& config = system.config();
  const double mass_ratio = config.companion.mass_solar / config.primary.mass_solar;

  double worst_barycenter = 0.0;
  double worst_period = 0.0;
  for (int32_t i = 0; i < 64; ++i) {
    const double now = i * 0.7331;
    const auto here = system.positions(now);

    const auto length = [](const glm::dvec3& value) {
      return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    };
    const double primary_arm = length(here.stars[0]);
    const double companion_arm = length(here.stars[1]);
    if (companion_arm > 0.0) {
      worst_barycenter = std::max(worst_barycenter, std::abs(primary_arm / companion_arm - mass_ratio));
    }

    const auto later = system.positions(now + system.binary_period_days());
    const double drift = length(later.stars[0] - here.stars[0]) / std::max(1.0, primary_arm);
    worst_period = std::max(worst_period, drift);
  }

  check.expect(worst_barycenter < 1e-12, "барицентр делит расстояние обратно пропорционально массам",
               std::format("худшее отклонение {:.3g}", worst_barycenter));
  check.expect(worst_period < 1e-9, "звёзды возвращаются в ту же точку через период двойной",
               std::format("худший относительный дрейф {:.3g}", worst_period));
}

void verify_determinism(checker& check, const pf08::celestial_system& system) {
  std::vector<double> times;
  for (int32_t i = 0; i < 48; ++i) {
    times.push_back(i * 1.13717);
  }

  std::vector<pf08::sky_state> forward;
  forward.reserve(times.size());
  for (const double now : times) {
    forward.push_back(system.evaluate(now));
  }

  // Обратный проход по тем же моментам: если бы вычисление зависело от предыдущего вызова,
  // порядок изменил бы результат. Это и есть проверка отсутствия скрытого состояния.
  bool stateless = true;
  for (size_t i = times.size(); i-- > 0;) {
    if (!identical_state(forward[i], system.evaluate(times[i]))) stateless = false;
  }
  check.expect(stateless, "результат не зависит от порядка вычисления моментов времени");

  const pf08::celestial_system fresh(system.config());
  bool reproducible = true;
  for (size_t i = 0; i < times.size(); ++i) {
    if (!identical_state(forward[i], fresh.evaluate(times[i]))) reproducible = false;
  }
  check.expect(reproducible, "новая система из того же конфига даёт побитно тот же результат");
}

struct continuity_measure {
  double star_altitude = 0.0;
  double star_eclipse = 0.0;
  double moon_eclipse = 0.0;
};

continuity_measure measure_continuity(const pf08::celestial_system& system, const double span_days,
                                      const double step_days) {
  continuity_measure worst;
  const int32_t steps = static_cast<int32_t>(span_days / step_days);

  auto previous = system.evaluate(0.0);
  for (int32_t i = 1; i <= steps; ++i) {
    const auto current = system.evaluate(i * step_days);
    for (size_t s = 0; s < current.stars.size(); ++s) {
      worst.star_altitude =
        std::max(worst.star_altitude, std::abs(current.stars[s].altitude_deg - previous.stars[s].altitude_deg));
      worst.star_eclipse = std::max(
        worst.star_eclipse, std::abs(current.stars[s].occluded_fraction - previous.stars[s].occluded_fraction));
    }
    for (size_t m = 0; m < current.moons.size(); ++m) {
      worst.moon_eclipse = std::max(
        worst.moon_eclipse, std::abs(current.moons[m].occluded_fraction - previous.moons[m].occluded_fraction));
    }
    previous = current;
  }
  return worst;
}

// Абсолютный порог на скачок здесь бесполезен: близкая луна проходит край тени планеты за шесть
// минут, поэтому физически верная глубина затмения меняется на 0.15 за минуту. Разрыв отличается
// от быстрого перехода не величиной, а поведением при измельчении шага: у непрерывной функции
// максимальный скачок падает пропорционально шагу, у разрыва остаётся прежним. Первая версия
// расчёта лунного затмения теряла глубину при полном перекрытии одного из светил и давала
// именно разрыв, поэтому проверка построена вокруг этого признака.
void verify_continuity(checker& check, const pf08::celestial_system& system) {
  constexpr double span_days = 14.0;
  constexpr double coarse_step = 1.0 / (24.0 * 60.0);
  constexpr double refinement = 4.0;
  constexpr double required_ratio = 2.5;

  const auto coarse = measure_continuity(system, span_days, coarse_step);
  const auto fine = measure_continuity(system, span_days, coarse_step / refinement);

  const auto expect_continuous = [&](const double coarse_jump, const double fine_jump, const std::string_view name) {
    if (coarse_jump <= 0.0) {
      check.expect(true, name);
      return;
    }
    const double ratio = fine_jump > 0.0 ? coarse_jump / fine_jump : refinement;
    check.expect(ratio >= required_ratio, name,
                 std::format("скачок {:.4f} -> {:.4f} при измельчении шага в {:.0f} раз, отношение {:.2f}",
                             coarse_jump, fine_jump, refinement, ratio));
  };

  expect_continuous(coarse.star_altitude, fine.star_altitude, "высота светил непрерывна");
  expect_continuous(coarse.star_eclipse, fine.star_eclipse, "глубина затмения светила непрерывна");
  expect_continuous(coarse.moon_eclipse, fine.moon_eclipse, "глубина лунного затмения непрерывна");
}

// Освещённость каждого тела обязана в точности следовать заявленной доле перекрытия и доле диска
// над горизонтом. Для лун это ещё и проверка того, что затмение считается по энергии обоих светил.
void verify_illuminance_identity(checker& check, const pf08::celestial_system& system) {
  constexpr double step_days = 1.0 / (24.0 * 6.0);
  double worst_star = 0.0;
  double worst_star_space = 0.0;
  double worst_moon_without_light = 0.0;
  double largest_source_difference = 0.0;
  bool moon_visibility_in_range = true;

  for (int32_t i = 0; i <= 12000; ++i) {
    const auto state = system.evaluate(i * step_days);

    for (size_t s = 0; s < state.stars.size(); ++s) {
      const auto& view = state.stars[s];
      const double distance_au = view.distance_km / 149597870.7;
      const double expected = system.star(s).illuminance_at_1au_lx / (distance_au * distance_au) *
                              (1.0 - view.occluded_fraction) * view.horizon_fraction;
      const double expected_space_unocculted = system.star(s).illuminance_at_1au_lx / (distance_au * distance_au);
      const double expected_space = expected_space_unocculted * (1.0 - view.occluded_fraction);
      const double scale = std::max(1.0, std::abs(expected));
      worst_star = std::max(worst_star, std::abs(view.illuminance_lx - expected) / scale);
      worst_star_space = std::max(worst_star_space,
                                  std::abs(view.space_unocculted_lx - expected_space_unocculted) /
                                    std::max(1.0, std::abs(expected_space_unocculted)));
      worst_star_space = std::max(worst_star_space,
                                  std::abs(view.space_illuminance_lx - expected_space) /
                                    std::max(1.0, std::abs(expected_space)));
    }

    for (const auto& view : state.moons) {
      for (const double visibility : view.star_visibility) {
        moon_visibility_in_range = moon_visibility_in_range && visibility >= 0.0 && visibility <= 1.0;
      }
      largest_source_difference =
        std::max(largest_source_difference, std::abs(view.star_visibility[0] - view.star_visibility[1]));
      // Полностью затмённая луна не может светить: обратное означало бы, что доля потери
      // освещения посчитана не по той энергии, которую луна действительно получает.
      if (view.occluded_fraction < 1.0 - 1e-9) continue;
      worst_moon_without_light = std::max(worst_moon_without_light, view.illuminance_lx);
    }
  }

  check.expect(worst_star < 1e-12, "освещённость светил следует перекрытию и горизонту",
               std::format("худшее относительное отклонение {:.3g}", worst_star));
  check.expect(worst_star_space < 1e-12, "незатменённый свет светила не восстанавливается делением на ноль",
               std::format("худшее относительное отклонение {:.3g}", worst_star_space));
  check.expect(worst_moon_without_light <= 0.0, "полностью затмённая луна не светит",
               std::format("остаточная освещённость {:.3g} лк", worst_moon_without_light));
  check.expect(moon_visibility_in_range, "видимость каждого светила с луны лежит в [0, 1]");
  check.expect(largest_source_difference > 0.1, "лунное затмение сохраняет цвет двух светил раздельно",
               std::format("наибольшая разница долей {:.3f}", largest_source_difference));
}

void verify_eclipse_energy(checker& check, const pf08::celestial_system& system) {
  constexpr double step_days = 1.0 / (24.0 * 60.0);
  double deepest = 0.0;
  double deepest_time = 0.0;
  int32_t deepest_star = -1;

  for (int32_t i = 0; i <= 200000; ++i) {
    const auto state = system.evaluate(i * step_days);
    for (size_t s = 0; s < state.stars.size(); ++s) {
      if (state.stars[s].occluded_fraction <= deepest) continue;
      deepest = state.stars[s].occluded_fraction;
      deepest_time = i * step_days;
      deepest_star = static_cast<int32_t>(s);
    }
  }

  check.expect(deepest > 0.0, "затмение светила найдено на интервале сканирования");
  if (deepest_star < 0) return;

  const auto state = system.evaluate(deepest_time);
  const auto& view = state.stars[static_cast<size_t>(deepest_star)];
  const double distance_au = view.distance_km / 149597870.7;
  const double unoccluded = system.star(static_cast<size_t>(deepest_star)).illuminance_at_1au_lx /
                            (distance_au * distance_au);
  const double expected = unoccluded * (1.0 - view.occluded_fraction) * view.horizon_fraction;

  check.expect_near(view.illuminance_lx, expected, std::abs(expected) * 1e-12,
                    "освещённость точно следует доле перекрытия диска");
  check.expect(view.space_unocculted_lx > 0.0, "незатменённая яркость диска переживает полную фазу",
               std::format("{:.3f} лк при перекрытии {:.6f}", view.space_unocculted_lx,
                           view.occluded_fraction));
  std::cout << std::format("  самое глубокое затмение: {} на {}, перекрыто {:.3f} диска телом {}\n", view.name,
                           format_clock(system, deepest_time), view.occluded_fraction, body_name(system, view.occluder));
}

void verify_calendar(checker& check, const pf08::celestial_system& system) {
  const double time = system.from_calendar(8, 100, 12.5);
  const auto calendar = system.to_calendar(time);
  check.expect(calendar.year == 8 && calendar.day == 100 && calendar.hour == 12 && calendar.minute == 30,
               "календарь сохраняет абсолютный год", std::format("получено г{} д{} {:02}:{:02}", calendar.year,
                                                                  calendar.day, calendar.hour, calendar.minute));
  check.expect(calendar.beat_year == 1, "восьмой год начинает новый приблизительный beat двойной",
               std::format("beat {}/{}", calendar.beat_year, system.binary_beat_years()));
  check.expect(system.binary_beat_error_days() > 0.0, "семилетний beat не выдаётся за точный цикл",
               std::format("остаточная ошибка {:.3f} сут", system.binary_beat_error_days()));
}

void verify_weather(checker& check) {
  const std::string path = std::string(PF08_RESOURCE_ROOT) + "/weather/presets.tavl";
  const std::string text = file_io::read(path, file_io::type::text);
  pf08::weather_preset_list presets;
  std::string diagnostics;
  const bool parsed = !text.empty() && pf08::parse_weather_presets(text, presets, diagnostics);
  check.expect(parsed, "погодные пресеты разобраны", diagnostics);
  if (!parsed) return;

  check.expect(presets.presets.size() == 7, "погодные пресеты содержат clear, haze, windy, fog, cloudy, overcast и rain",
               std::format("получено {}", presets.presets.size()));
  const auto* clear = pf08::find_weather_preset(presets, "clear");
  check.expect(clear != nullptr, "clear weather объявлена");
  if (clear != nullptr) {
    const auto state = pf08::state_from_preset(*clear);
    check.expect_near(state.aerosol_turbidity, 1.0, 0.0, "clear сохраняет baseline-аэрозоль");
    check.expect_near(state.wind_direction_deg, 250.0, 0.0, "clear сохраняет baseline-направление ветра");
    check.expect_near(state.wind_strength_m, 0.22, 0.0, "clear сохраняет baseline-силу ветра");
    check.expect_near(state.fog_extinction_per_m, 0.0, 0.0, "clear точно отключает локальную среду");
    check.expect_near(state.fog_density_variation, 0.0, 0.0,
                      "clear точно отключает пространственную модуляцию");
    check.expect_near(state.cloud_coverage, 0.0, 0.0, "clear точно отключает облачный слой");
    check.expect_near(state.rain_rate_mm_h, 0.0, 0.0, "clear точно отключает ближние осадки");
    check.expect_near(state.rain_far_extinction_per_m, 0.0, 0.0,
                      "clear точно отключает дальний дождевой объём");
  }

  const auto* rain = pf08::find_weather_preset(presets, "rain");
  check.expect(rain != nullptr, "rain weather объявлена");
  if (rain != nullptr) {
    const auto state = pf08::state_from_preset(*rain);
    check.expect(state.rain_rate_mm_h > 0.0 && state.rain_far_extinction_per_m > 0.0,
                 "rain связывает near-pool и far extinction одним состоянием");
    check.expect(state.rain_fall_speed_m_s > state.rain_wind_speed_m_s,
                 "authored дождь преимущественно падает, а не летит горизонтально");
    const double start = state.rain_near_radius_m * 0.72;
    const double width = state.rain_near_radius_m * 0.45;
    check.expect_near(pf08::rain_far_weight(start, start, width), 0.0, 0.0,
                      "far-дождь нулевой в начале LOD handover");
    check.expect_near(pf08::rain_far_weight(start + width * 0.5, start, width), 0.5, 1e-14,
                      "середина LOD handover имеет половинный вес");
    check.expect_near(pf08::rain_far_weight(start + width, start, width), 1.0, 0.0,
                      "far-дождь достигает полного веса за границей near-pool");
  }

  const auto* overcast = pf08::find_weather_preset(presets, "overcast");
  check.expect(overcast != nullptr, "overcast weather объявлена");
  if (overcast != nullptr) {
    const auto state = pf08::state_from_preset(*overcast);
    const double thickness = state.cloud_top_height_m - state.cloud_base_height_m;
    check.expect(state.cloud_coverage > 0.0 && state.cloud_coverage <= 1.0,
                 "overcast включает конечное облачное покрытие");
    check.expect_near(pf08::cloud_vertical_column(0.0, state.cloud_base_height_m,
                                                  state.cloud_top_height_m),
                      thickness * 0.5, 1e-12,
                      "полный sin²-профиль имеет половину толщины слоя");
    check.expect_near(pf08::cloud_vertical_column(
                        0.5 * (state.cloud_base_height_m + state.cloud_top_height_m),
                        state.cloud_base_height_m, state.cloud_top_height_m),
                      thickness * 0.25, 1e-12,
                      "над серединой остаётся четверть толщины облачного слоя");
    check.expect_near(pf08::cloud_light_transmittance(
                        state.cloud_extinction_per_m, 1.0, 0.0, 1.0,
                        state.cloud_base_height_m, state.cloud_top_height_m),
                      std::exp(-state.cloud_extinction_per_m * thickness * 0.5), 1e-14,
                      "вертикальная облачная тень совпадает с Beer-Lambert");
  }

  const auto* fog = pf08::find_weather_preset(presets, "fog");
  check.expect(fog != nullptr, "fog weather объявлена");
  if (fog != nullptr) {
    const auto state = pf08::state_from_preset(*fog);
    const auto integral = pf08::integrate_homogeneous_fog(
      state.fog_extinction_per_m, state.fog_scattering_albedo, 100.0);
    check.expect_near(integral.transmittance, std::exp(-1.8), 1e-14,
                      "однородный туман совпадает с аналитическим Beer-Lambert");
    check.expect_near(integral.in_scattering_fraction,
                      state.fog_scattering_albedo * (1.0 - std::exp(-1.8)), 1e-14,
                      "однородное рассеяние интегрируется аналитически");
    check.expect_near(pf08::fog_density_at_height(state.fog_base_height_m + state.fog_scale_height_m,
                                                  state.fog_base_height_m,
                                                  state.fog_scale_height_m), std::exp(-1.0), 1e-14,
                      "высотный профиль теряет e раз на scale height");
    check.expect_near(pf08::fog_light_transmittance(state.fog_extinction_per_m, 0.0, 1.0,
                                                    state.fog_base_height_m, state.fog_scale_height_m),
                      std::exp(-state.fog_extinction_per_m * state.fog_scale_height_m), 1e-14,
                      "вертикальный свет получает аналитическую толщину туманного слоя");
    check.expect(state.fog_density_variation > 0.0 && state.fog_advection_speed_m_s > 0.0,
                 "authored fog имеет пространственные ячейки и advection");
  }

  check.expect_near(pf08::normalize_weather_direction(-10.0), 350.0, 0.0,
                    "направление ветра нормализуется по кругу");
  const pf08::weather_state west{1.0, 350.0, 0.2, 0.0, 0.92, 0.35, 0.0, 80.0, 0.0, 70.0, 0.0};
  const pf08::weather_state east{2.0, 10.0, 0.6, 0.02, 0.80, -0.15, 10.0, 40.0, 0.8, 50.0, 2.0};
  const auto middle = pf08::interpolate_weather(west, east, 0.5);
  check.expect_near(middle.wind_direction_deg, 0.0, 1e-12,
                    "переход ветра идёт по короткой дуге через север");
  check.expect_near(middle.fog_extinction_per_m, 0.01, 1e-12,
                    "локальная среда входит в тот же непрерывный погодный переход");
  check.expect_near(middle.fog_scale_height_m, 60.0, 1e-12,
                    "форма локальной среды интерполируется вместе с её плотностью");
  check.expect_near(middle.fog_density_variation, 0.4, 1e-12,
                    "неоднородность входит в тот же непрерывный weather transition");
  check.expect_near(middle.fog_advection_speed_m_s, 1.0, 1e-12,
                    "скорость advection входит в тот же непрерывный weather transition");
  if (clear != nullptr && overcast != nullptr) {
    const auto cloud_middle = pf08::interpolate_weather(
      pf08::state_from_preset(*clear), pf08::state_from_preset(*overcast), 0.5);
    check.expect_near(cloud_middle.cloud_coverage, overcast->cloud_coverage * 0.5, 1e-12,
                      "coverage входит в непрерывный weather transition");
    check.expect_near(cloud_middle.cloud_advection_speed_m_s,
                      0.5 * (clear->cloud_advection_speed_m_s + overcast->cloud_advection_speed_m_s),
                      1e-12, "скорость облаков интерполируется вместе с погодой");
  }
  if (clear != nullptr && rain != nullptr) {
    const auto rain_middle = pf08::interpolate_weather(
      pf08::state_from_preset(*clear), pf08::state_from_preset(*rain), 0.5);
    check.expect_near(rain_middle.rain_rate_mm_h, rain->rain_rate_mm_h * 0.5, 1e-12,
                      "rain rate входит в непрерывный weather transition");
    check.expect_near(rain_middle.rain_far_extinction_per_m,
                      rain->rain_far_extinction_per_m * 0.5, 1e-12,
                      "far rain extinction интерполируется вместе с near-плотностью");
  }

  pf08::weather_transition transition;
  transition.snap("west", west);
  transition.set_target("east", east, 4.0);
  transition.advance(1.0);
  check.expect_near(transition.progress(), 0.25, 0.0, "погодный переход использует реальные секунды");
  const auto interrupted = transition.state();
  transition.set_target("west", west, 2.0);
  check.expect_near(transition.state().aerosol_turbidity, interrupted.aerosol_turbidity, 0.0,
                    "новый переход не щёлкает посреди предыдущего");
  transition.advance(2.0);
  check.expect(!transition.active() && transition.target_name() == "west",
               "погодный переход точно приходит в authored-состояние");

  pf08::atmosphere_cache_gate cache_gate{2};
  check.expect(cache_gate.try_rebuild(0), "atmosphere cache строится на стартовом кадре");
  check.expect(!cache_gate.try_rebuild(1), "doublebuffer cache запрещает соседний rebuild");
  check.expect(cache_gate.try_rebuild(2), "doublebuffer cache разрешает rebuild через два кадра");
  check.expect(!cache_gate.try_rebuild(3) && cache_gate.try_rebuild(4),
               "плавный weather transition сохраняет безопасный ритм cache rebuild");
}

void verify_event_calendar(checker& check, const pf08::celestial_system& system) {
  pf08::survey_options options;
  options.budget_span_days = 7.0 * system.planet_year_days();
  // Это тот же шаг, которым печатается authored-бюджет. Пять минут уже недостаточно: короткий провал
  // ниже порога может разрезать одно взаимное затмение на два и дать ложные 16 вместо 15.
  options.budget_step_minutes = 2.0;
  const auto budget = pf08::calculate_event_budget(system, options);

  constexpr std::array<int32_t, size_t(pf08::observable_event_kind::count)> expected{15, 12, 32, 5, 2, 0};
  constexpr std::array<std::string_view, size_t(pf08::observable_event_kind::count)> names{
    "взаимные затмения светил", "затмения светил лунами", "лунные затмения",
    "покрытия лун", "тесные парады", "одна луна над обоими светилами"};
  for (size_t i = 0; i < expected.size(); ++i) {
    check.expect(budget.categories[i].count == expected[i], names[i],
                 std::format("{} вместо {} за 2555 сут", budget.categories[i].count, expected[i]));
  }
  check.expect(budget.unique.count == 66, "семилетний календарь содержит 66 уникальных эпизодов",
               std::format("получено {}", budget.unique.count));
}

int run_verification(const pf08::celestial_system& system) {
  std::cout << "\n== проверки ==\n";

  checker check;
  verify_kepler(check);
  verify_occlusion(check);
  verify_photometry(check, system);
  verify_stability(check, system);
  verify_dynamics(check, system);
  verify_determinism(check, system);
  verify_continuity(check, system);
  verify_illuminance_identity(check, system);
  verify_eclipse_energy(check, system);
  verify_calendar(check, system);
  verify_weather(check);
  verify_event_calendar(check, system);

  std::cout << std::format("\n  проверок {}, провалов {}\n", check.total(), check.failed());
  return check.passed() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  options selected;
  if (!parse_options(argc, argv, selected)) return 0;

  const std::string text = file_io::read(selected.config_path, file_io::type::text);
  if (text.empty()) {
    utils::warn("PF08 could not read celestial config '{}'", selected.config_path);
    return 2;
  }

  pf08::system_config config;
  std::string diagnostics;
  if (!pf08::parse_system_config(text, config, diagnostics)) {
    utils::warn("PF08 celestial config '{}' has tavl diagnostics:\n{}", selected.config_path, diagnostics);
    return 2;
  }

  const pf08::celestial_system system(std::move(config));
  print_system(system);

  // Календарный старт перекрывает абсолютный и действует ВО ВСЕХ режимах, а не только в окне:
  // отчёт по третьему календарному году должен спрашиваться теми же словами, что и кадр.
  if (selected.view.start_year != 0) {
    selected.time_days = system.from_calendar(selected.view.start_year, selected.view.start_day_of_year,
                                              selected.view.start_hour);
  }
  selected.view.start_time_days = selected.time_days;

  switch (selected.requested) {
    case action::render: return pf08::run_sky_view(system, selected.view);
    case action::report: print_state(system, system.evaluate(selected.time_days)); return 0;
    case action::events:
      scan_events(system, selected.time_days, selected.span_days, selected.step_minutes, selected.survey);
      return 0;
    case action::verify: return run_verification(system);
    case action::survey: pf08::run_survey(system, selected.survey); return 0;
  }
  return 0;
}
