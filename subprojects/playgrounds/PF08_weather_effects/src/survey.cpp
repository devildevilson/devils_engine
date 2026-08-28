#include "survey.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace devils_engine::pf08 {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double minutes_per_day = 24.0 * 60.0;

double to_radians(const double degrees) {
  return degrees * pi / 180.0;
}

double to_degrees(const double radians) {
  return radians * 180.0 / pi;
}

double angle_between(const glm::dvec3& a, const glm::dvec3& b) {
  const auto cross = glm::dvec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
  const double cross_length = std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
  return to_degrees(std::atan2(cross_length, a.x * b.x + a.y * b.y + a.z * b.z));
}

std::string format_hours(const double fraction_of_day) {
  if (fraction_of_day < 0.0) return "   --";
  const double hours = fraction_of_day * 24.0;
  const int32_t hour = static_cast<int32_t>(std::floor(hours));
  const int32_t minute = static_cast<int32_t>(std::floor((hours - hour) * 60.0 + 0.5));
  return std::format("{:02}:{:02}", minute == 60 ? hour + 1 : hour, minute == 60 ? 0 : minute);
}

// --- разбор одних суток ---

struct day_summary {
  double primary_rise = -1.0;      // доля суток от их начала
  double primary_set = -1.0;
  double companion_rise = -1.0;
  double companion_set = -1.0;
  double daylight_hours = 0.0;     // хотя бы одно светило над горизонтом
  double both_hours = 0.0;
  double single_hours = 0.0;       // освещено ровно одним светилом
  double insolation_lx_hours = 0.0;
  double noon_time = 0.0;
  double noon_altitude_primary = -90.0;
  double noon_altitude_companion = -90.0;
  double noon_companion_share = 0.0;
  double noon_separation_deg = 0.0;
  double moon_daylight_hours[8]{};
  double moon_best_daylight_contrast[8]{};
  double moon_phase_at_best[8]{};
};

double interpolate_crossing(const double previous_time, const double previous_value, const double current_time,
                            const double current_value) {
  const double span = current_value - previous_value;
  if (span == 0.0) return current_time;
  return previous_time + (current_time - previous_time) * (-previous_value / span);
}

// `day_index` — номер СУТОК, а не абсолютное время: сутки начинаются в полночь, и её эпоху знает
// небесная механика. Раньше сюда приходило просто целое время в сутках, и разбор «одного дня» брал
// произвольный отрезок вращения, из-за чего восход мог оказаться позже заката.
day_summary summarize_day(const celestial_system& system, const double day_index, const double step_minutes) {
  const double day_start = system.from_calendar(1, uint32_t(day_index), 0.0);
  day_summary summary;
  const double step_days = step_minutes / minutes_per_day;
  const int32_t steps = static_cast<int32_t>(minutes_per_day / step_minutes);
  const size_t moon_count = system.config().moons.size();

  auto previous = system.evaluate(day_start);
  for (int32_t i = 1; i <= steps; ++i) {
    const double now = day_start + i * step_days;
    const auto current = system.evaluate(now);

    const auto track_crossing = [&](const double before, const double after, double& rise, double& set) {
      if (before < 0.0 && after >= 0.0 && rise < 0.0) {
        rise = interpolate_crossing(i - 1.0, before, static_cast<double>(i), after) / steps;
      }
      if (before >= 0.0 && after < 0.0 && set < 0.0) {
        set = interpolate_crossing(i - 1.0, before, static_cast<double>(i), after) / steps;
      }
    };
    track_crossing(previous.stars[0].altitude_deg, current.stars[0].altitude_deg, summary.primary_rise,
                   summary.primary_set);
    track_crossing(previous.stars[1].altitude_deg, current.stars[1].altitude_deg, summary.companion_rise,
                   summary.companion_set);

    const bool primary_up = current.stars[0].altitude_deg > 0.0;
    const bool companion_up = current.stars[1].altitude_deg > 0.0;
    const double hours = step_minutes / 60.0;
    if (primary_up || companion_up) summary.daylight_hours += hours;
    if (primary_up && companion_up) summary.both_hours += hours;
    if (primary_up != companion_up) summary.single_hours += hours;
    summary.insolation_lx_hours += current.horizontal_illuminance_lx * hours;

    if (current.stars[0].altitude_deg > summary.noon_altitude_primary) {
      summary.noon_altitude_primary = current.stars[0].altitude_deg;
      summary.noon_altitude_companion = current.stars[1].altitude_deg;
      summary.noon_time = static_cast<double>(i) / steps;
      const double total = current.stars[0].illuminance_lx + current.stars[1].illuminance_lx;
      summary.noon_companion_share = total > 0.0 ? current.stars[1].illuminance_lx / total : 0.0;
      summary.noon_separation_deg = angle_between(current.stars[0].direction, current.stars[1].direction);
    }

    // Дневная видимость луны: её средняя яркость диска против грубой оценки яркости дневного неба.
    // Точная яркость неба принадлежит срезу 2; здесь неба ещё нет, поэтому берётся стандартная доля
    // рассеянного света в 15% от прямого, распределённая по полусфере.
    //
    // Сравнение имеет смысл только при уверенно светлом небе. У горизонта светило само по себе даёт
    // мало горизонтальной освещённости, знаменатель проваливается, и любая луна формально оказывается
    // ярче неба — верно по существу (на закате луна и правда проступает), но к вопросу «видно ли луну
    // в разгар дня» отношения не имеет. Поэтому берётся уверенно высокое светило.
    constexpr double daylight_altitude_deg = 30.0;
    const bool full_daylight = current.stars[0].altitude_deg > daylight_altitude_deg ||
                               current.stars[1].altitude_deg > daylight_altitude_deg;
    if (full_daylight) {
      const double sky_luminance = 0.15 * current.horizontal_illuminance_lx / pi;
      for (size_t m = 0; m < moon_count && m < 8; ++m) {
        const auto& moon = current.moons[m];
        if (moon.altitude_deg <= 0.0) continue;
        summary.moon_daylight_hours[m] += hours;
        if (sky_luminance <= 0.0) continue;

        const double disk_solid_angle = pi * std::pow(std::sin(to_radians(moon.angular_radius_deg)), 2.0);
        const double moon_luminance = moon.illuminance_lx / disk_solid_angle;
        const double contrast = moon_luminance / sky_luminance;
        if (contrast <= summary.moon_best_daylight_contrast[m]) continue;
        summary.moon_best_daylight_contrast[m] = contrast;
        summary.moon_phase_at_best[m] = moon.phase;
      }
    }

    previous = current;
  }
  return summary;
}

// --- разделы отчёта ---

void print_structure(const celestial_system& system) {
  std::cout << "\n== устройство системы ==\n";
  std::cout << std::format("  двойная делает {:.2f} оборота за год планеты\n", system.binary_revolutions_per_year());
  std::cout << std::format("  запас устойчивости по Холману-Вигерту: {:.2f}x (меньше 1.0 — орбита разрушается)\n",
                           system.stability_margin());
  std::cout << std::format("  максимальное разделение светил на небе: {:.1f}°\n",
                           system.maximum_star_separation_deg());

  // Медленная двойная и широко разнесённые солнца — одно и то же требование, и упирается оно в
  // устойчивость. Полезно показать границу прямо для сконфигурированных масс и эксцентриситета.
  const double mass_ratio =
    system.config().companion.mass_solar / (system.config().primary.mass_solar + system.config().companion.mass_solar);
  const double critical = holman_wiegert_critical_ratio(system.config().binary.eccentricity, mass_ratio);
  std::cout << std::format("  предел для этих масс: не меньше {:.2f} оборота в год и не больше {:.1f}° разделения\n",
                           std::pow(critical, 1.5), to_degrees(std::atan(1.0 / critical)));
}

void print_seasons(const celestial_system& system, const survey_options& options) {
  std::cout << "\n== год: сезоны ==\n";
  std::cout << "  день   полдень A  полдень B  светло, ч  оба, ч  одно, ч   инсоляция, клк·ч  вклад B  разделение\n";

  const double year = system.planet_year_days();
  const int32_t samples = static_cast<int32_t>(options.year_samples);
  for (int32_t i = 0; i < samples; ++i) {
    const double day = std::floor(year * i / samples);
    const auto summary = summarize_day(system, day, options.day_step_minutes);
    std::cout << std::format("  {:>5.0f}  {:9.2f}° {:9.2f}° {:10.2f} {:7.2f} {:8.2f} {:17.1f} {:7.1f}% {:9.2f}°\n", day,
                             summary.noon_altitude_primary, summary.noon_altitude_companion, summary.daylight_hours,
                             summary.both_hours, summary.single_hours, summary.insolation_lx_hours / 1000.0,
                             100.0 * summary.noon_companion_share, summary.noon_separation_deg);
  }
}

void print_binary_cycle(const celestial_system& system, const survey_options& options) {
  std::cout << "\n== оборот двойной: как меняется день внутри одного оборота ==\n";
  std::cout << "  день   разделение   вклад B   восход A  восход B  разрыв   закат A  закат B  разрыв   одно светило, ч\n";

  const double period = system.binary_period_days();
  constexpr int32_t samples = 10;
  for (int32_t i = 0; i < samples; ++i) {
    const double day = std::floor(period * i / samples);
    const auto summary = summarize_day(system, day, options.day_step_minutes);

    const auto gap_minutes = [](const double first, const double second) {
      if (first < 0.0 || second < 0.0) return std::string("    --");
      return std::format("{:5.0f}м", std::abs(first - second) * minutes_per_day);
    };

    std::cout << std::format("  {:>5.0f} {:11.2f}° {:8.1f}% {:>10} {:>9} {:>7} {:>9} {:>8} {:>7} {:15.2f}\n", day,
                             summary.noon_separation_deg, 100.0 * summary.noon_companion_share,
                             format_hours(summary.primary_rise), format_hours(summary.companion_rise),
                             gap_minutes(summary.primary_rise, summary.companion_rise),
                             format_hours(summary.primary_set), format_hours(summary.companion_set),
                             gap_minutes(summary.primary_set, summary.companion_set), summary.single_hours);
  }
}

void print_parades(const celestial_system& system, const survey_options& options) {
  std::cout << "\n== парады: все тела над горизонтом и рядом друг с другом ==\n";

  const double step_days = options.parade_step_minutes / minutes_per_day;
  const int32_t steps = static_cast<int32_t>(options.parade_span_days / step_days);

  constexpr std::array<double, 4> thresholds{20.0, 30.0, 45.0, 60.0};
  int64_t all_visible = 0;
  std::array<int64_t, thresholds.size()> under_threshold{};
  std::array<int32_t, thresholds.size()> events{};
  std::array<bool, thresholds.size()> inside{};
  double tightest_spread = 360.0;
  double tightest_time = 0.0;

  for (int32_t i = 0; i <= steps; ++i) {
    const double now = i * step_days;
    const auto state = system.evaluate(now);

    std::vector<glm::dvec3> visible;
    visible.reserve(2 + state.moons.size());
    for (const auto& view : state.stars) {
      if (view.altitude_deg > 0.0) visible.push_back(view.direction);
    }
    for (const auto& view : state.moons) {
      if (view.altitude_deg > 0.0) visible.push_back(view.direction);
    }
    if (visible.size() != 2 + state.moons.size()) {
      inside.fill(false);
      continue;
    }
    ++all_visible;

    double spread = 0.0;
    for (size_t a = 0; a < visible.size(); ++a) {
      for (size_t b = a + 1; b < visible.size(); ++b) {
        spread = std::max(spread, angle_between(visible[a], visible[b]));
      }
    }
    if (spread < tightest_spread) {
      tightest_spread = spread;
      tightest_time = now;
    }
    for (size_t t = 0; t < thresholds.size(); ++t) {
      if (spread <= thresholds[t]) {
        ++under_threshold[t];
        if (!inside[t]) {
          inside[t] = true;
          ++events[t];
        }
      } else {
        inside[t] = false;
      }
    }
  }

  const double observed_days = options.parade_span_days;
  std::cout << std::format("  окно наблюдения {:.0f} сут, шаг {:.0f} мин\n", observed_days,
                           options.parade_step_minutes);
  std::cout << std::format("  все {} тел одновременно над горизонтом: {:.1f}% времени\n",
                           2 + system.config().moons.size(), 100.0 * all_visible / (steps + 1));
  for (size_t t = 0; t < thresholds.size(); ++t) {
    if (events[t] == 0) {
      std::cout << std::format("  в пределах {:2.0f}°: ни разу\n", thresholds[t]);
      continue;
    }
    std::cout << std::format("  в пределах {:2.0f}°: {} событий, примерно раз в {:.0f} сут, суммарно {:.2f}% времени\n",
                             thresholds[t], events[t], observed_days / events[t],
                             100.0 * under_threshold[t] / (steps + 1));
  }
  std::cout << std::format("  самое тесное схождение: {:.2f}° на сутки {:.3f}\n", tightest_spread, tightest_time);
}

void print_double_eclipses(const celestial_system& system, const survey_options& options) {
  std::cout << "\n== одна луна закрывает оба светила ==\n";

  const double step_days = options.eclipse_step_minutes / minutes_per_day;
  const int32_t steps = static_cast<int32_t>(options.eclipse_span_days / step_days);
  const size_t moon_count = system.config().moons.size();

  std::vector<int64_t> simultaneous_any(moon_count, 0);
  std::vector<int64_t> simultaneous_total(moon_count, 0);
  std::vector<int32_t> simultaneous_events(moon_count, 0);
  std::vector<int32_t> same_day_events(moon_count, 0);
  std::vector<bool> inside(moon_count, false);
  std::vector<double> deepest(moon_count, 0.0);
  std::vector<double> deepest_time(moon_count, 0.0);

  std::vector<double> last_primary_cover(moon_count, -1000.0);
  std::vector<double> last_companion_cover(moon_count, -1000.0);
  std::vector<double> last_same_day(moon_count, -1000.0);

  for (int32_t i = 0; i <= steps; ++i) {
    const double now = i * step_days;
    const auto state = system.evaluate(now);

    for (size_t m = 0; m < moon_count; ++m) {
      const int32_t body = 3 + static_cast<int32_t>(m);
      const bool covers_primary = state.stars[0].occluder == body && state.stars[0].occluded_fraction > 0.0;
      const bool covers_companion = state.stars[1].occluder == body && state.stars[1].occluded_fraction > 0.0;

      if (covers_primary) last_primary_cover[m] = now;
      if (covers_companion) last_companion_cover[m] = now;

      // «В один день» считается по обоим порядкам следования и не засчитывается дважды.
      const double pair_gap = std::abs(last_primary_cover[m] - last_companion_cover[m]);
      if (covers_primary || covers_companion) {
        if (pair_gap <= 1.0 && now - last_same_day[m] > 1.0) {
          ++same_day_events[m];
          last_same_day[m] = now;
        }
      }

      if (!covers_primary || !covers_companion) {
        inside[m] = false;
        continue;
      }

      ++simultaneous_any[m];
      const double weakest = std::min(state.stars[0].occluded_fraction, state.stars[1].occluded_fraction);
      if (weakest >= 1.0) ++simultaneous_total[m];
      if (weakest > deepest[m]) {
        deepest[m] = weakest;
        deepest_time[m] = now;
      }
      if (!inside[m]) {
        inside[m] = true;
        ++simultaneous_events[m];
      }
    }
  }

  std::cout << std::format("  окно наблюдения {:.0f} сут ({:.1f} года планеты), шаг {:.0f} мин\n",
                           options.eclipse_span_days, options.eclipse_span_days / system.planet_year_days(),
                           options.eclipse_step_minutes);
  for (size_t m = 0; m < moon_count; ++m) {
    const auto& moon = system.config().moons[m];
    const double per_event_days =
      simultaneous_events[m] > 0 ? options.eclipse_span_days / simultaneous_events[m] : 0.0;
    std::cout << std::format(
      "  {:<8} оба светила разом: {} событий (раз в {:.0f} сут), из них полных {:.2f}% времени; "
      "глубочайшее {:.3f} на сутки {:.2f}; в один день (по очереди): {} раз, то есть раз в {:.0f} сут\n",
      moon.name, simultaneous_events[m], per_event_days,
      simultaneous_any[m] > 0 ? 100.0 * simultaneous_total[m] / simultaneous_any[m] : 0.0, deepest[m], deepest_time[m],
      same_day_events[m], same_day_events[m] > 0 ? options.eclipse_span_days / same_day_events[m] : 0.0);
  }
}

void print_daytime_moons(const celestial_system& system, const survey_options& options) {
  std::cout << "\n== луны днём ==\n";
  std::cout << "  замер при светиле выше 30°, то есть в разгар дня, а не на закате\n";

  const size_t moon_count = system.config().moons.size();
  std::vector<double> daylight_hours(moon_count, 0.0);
  std::vector<double> best_contrast(moon_count, 0.0);
  std::vector<double> phase_at_best(moon_count, 0.0);
  double total_daylight = 0.0;

  const double year = system.planet_year_days();
  constexpr int32_t samples = 24;
  for (int32_t i = 0; i < samples; ++i) {
    const double day = std::floor(year * i / samples);
    const auto summary = summarize_day(system, day, options.day_step_minutes);
    total_daylight += summary.daylight_hours;
    for (size_t m = 0; m < moon_count && m < 8; ++m) {
      daylight_hours[m] += summary.moon_daylight_hours[m];
      if (summary.moon_best_daylight_contrast[m] <= best_contrast[m]) continue;
      best_contrast[m] = summary.moon_best_daylight_contrast[m];
      phase_at_best[m] = summary.moon_phase_at_best[m];
    }
  }

  for (size_t m = 0; m < moon_count; ++m) {
    const auto& moon = system.config().moons[m];
    std::cout << std::format(
      "  {:<8} диск {:.2f}°, над горизонтом {:.0f}% светлого времени, лучшая яркость {:.2f} от яркости неба "
      "(при фазе {:.2f})\n",
      moon.name, 2.0 * to_degrees(std::atan(moon.radius_km / moon.orbit.semi_major_axis)),
      100.0 * daylight_hours[m] / std::max(1.0, total_daylight), best_contrast[m], phase_at_best[m]);
  }
  std::cout << "  ориентир: земная Луна днём даёт примерно 0.3-0.7 и уверенно видна\n";
}


// --- бюджет событий ---

struct event_counter {
  std::string name;
  int32_t count = 0;
  double strongest = 0.0;
  bool active = false;

  void update(const bool condition, const double magnitude) {
    if (!condition) {
      active = false;
      return;
    }
    strongest = std::max(strongest, magnitude);
    if (active) return;
    active = true;
    ++count;
  }
};

void print_event_budget(const celestial_system& system, const survey_options& options) {
  std::cout << "\n== бюджет событий ==\n";
  const auto budget = calculate_event_budget(system, options);
  constexpr std::array<std::string_view, size_t(observable_event_kind::count)> names{
    "взаимное затмение солнц", "затмение солнца луной", "лунное затмение",
    "покрытие луны луной", "парад всех тел", "луна закрывает оба солнца"};

  const double real_hours_per_game_day =
    minutes_per_day / options.game_minutes_per_real_second / 3600.0;
  std::cout << std::format("  окно {:.0f} игровых суток ({:.1f} года) от г1 д0 00:00; только видимые события\n",
                           options.budget_span_days, options.budget_span_days / system.planet_year_days());
  std::cout << std::format("  темп времени: {:.0f} игровая минута за реальную секунду, то есть сутки за {:.1f} мин реального\n",
                           options.game_minutes_per_real_second, real_hours_per_game_day * 60.0);
  std::cout << "  событие                       число  раз в, сут   реальных часов   сильнейшее\n";

  for (size_t i = 0; i < budget.categories.size(); ++i) {
    const auto& event = budget.categories[i];
    if (event.count == 0) {
      std::cout << std::format("  {:<30} {:>5}  {:>10}   {:>14}   {:>10}\n", names[i], 0, "-", "-", "-");
      continue;
    }
    const double interval = options.budget_span_days / event.count;
    std::cout << std::format("  {:<30} {:5}  {:10.0f}   {:14.1f}   {:10.3f}\n", names[i], event.count, interval,
                             interval * real_hours_per_game_day, event.strongest);
  }

  if (budget.unique.count > 0) {
    const double interval = options.budget_span_days / budget.unique.count;
    std::cout << std::format("  {:<30} {:5}  {:10.0f}   {:14.1f}\n", "уникальные наблюдаемые эпизоды",
                             budget.unique.count,
                             interval, interval * real_hours_per_game_day);
  }
}


// --- деление на малые и крупные эффекты ---

// Событие для игрока — это изменение света, а не геометрическое совпадение. Поэтому классификация
// строится на доле потерянной освещённости: днём от светил, ночью от лун. Отдельным признаком
// крупного явления служит почти полное угасание одного из двух светил: пропажа источника теней
// меняет кадр сильнее, чем следует из его доли в общей освещённости.
void print_effect_scale(const celestial_system& system, const survey_options& options) {
  std::cout << "\n== малые и крупные эффекты ==\n";

  const double step_days = options.budget_step_minutes / minutes_per_day;
  const int32_t steps = static_cast<int32_t>(options.budget_span_days / step_days);
  const double start_days = system.from_calendar(1, 0, 0.0);

  event_counter small_day{"малый дневной"};
  event_counter large_day{"крупный дневной"};
  event_counter small_night{"малое ночное"};
  event_counter large_night{"крупное ночное"};
  double deepest_day = 0.0;
  double deepest_day_time = 0.0;
  double deepest_night = 0.0;

  for (int32_t i = 0; i <= steps; ++i) {
    const double elapsed = i * step_days;
    const auto state = system.evaluate(start_days + elapsed);

    const double day_dimming =
      state.unocculted_star_illuminance_lx > 0.0
        ? 1.0 - state.star_illuminance_lx / state.unocculted_star_illuminance_lx
        : 0.0;
    const double night_dimming =
      state.unocculted_moon_illuminance_lx > 0.0
        ? 1.0 - state.moon_illuminance_lx / state.unocculted_moon_illuminance_lx
        : 0.0;

    bool star_extinguished = false;
    for (const auto& star : state.stars) {
      if (star.altitude_deg > 0.0 && star.occluded_fraction >= options.large_effect_star_occultation) {
        star_extinguished = true;
      }
    }

    const bool day_large = day_dimming >= options.large_effect_dimming || star_extinguished;
    const bool day_small = !day_large && day_dimming >= options.small_effect_dimming;
    // Ночное событие считается только когда светил нет над горизонтом: затмение луны в дневном
    // небе игрок не увидит, каким бы глубоким оно ни было.
    const bool night_visible = state.star_illuminance_lx <= 0.0 && state.moon_illuminance_lx >= 0.0;
    const bool night_large = night_visible && night_dimming >= options.large_effect_dimming;
    const bool night_small = night_visible && !night_large && night_dimming >= options.small_effect_dimming;

    if (day_dimming > deepest_day) {
      deepest_day = day_dimming;
      deepest_day_time = elapsed;
    }
    deepest_night = std::max(deepest_night, night_visible ? night_dimming : 0.0);

    small_day.update(day_small, day_dimming);
    large_day.update(day_large, day_dimming);
    small_night.update(night_small, night_dimming);
    large_night.update(night_large, night_dimming);
  }

  const double real_hours_per_game_day = minutes_per_day / options.game_minutes_per_real_second / 3600.0;
  std::cout << std::format("  порог малого {:.0f}% потери света, крупного {:.0f}% либо угасание светила на {:.0f}%\n",
                           100.0 * options.small_effect_dimming, 100.0 * options.large_effect_dimming,
                           100.0 * options.large_effect_star_occultation);
  std::cout << "  класс                         число  раз в, сут   реальных часов   сильнейшее\n";

  const auto line = [&](const event_counter& event) {
    if (event.count == 0) {
      std::cout << std::format("  {:<30} {:>5}  {:>10}   {:>14}   {:>10}\n", event.name, 0, "-", "-", "-");
      return 0.0;
    }
    const double interval = options.budget_span_days / event.count;
    std::cout << std::format("  {:<30} {:5}  {:10.0f}   {:14.1f}   {:10.3f}\n", event.name, event.count, interval,
                             interval * real_hours_per_game_day, event.strongest);
    return 1.0 / interval;
  };

  const double small_rate = line(small_day) + line(small_night);
  const double large_rate = line(large_day) + line(large_night);

  if (small_rate > 0.0) {
    const int32_t count = small_day.count + small_night.count;
    std::cout << std::format("  {:<30} {:5}  {:10.0f}   {:14.1f}\n", "ВСЕ малые", count, 1.0 / small_rate,
                             real_hours_per_game_day / small_rate);
  }
  if (large_rate > 0.0) {
    const int32_t count = large_day.count + large_night.count;
    std::cout << std::format("  {:<30} {:5}  {:10.0f}   {:14.1f}\n", "ВСЕ крупные", count, 1.0 / large_rate,
                             real_hours_per_game_day / large_rate);
  }
  std::cout << std::format("  глубочайшее дневное затемнение за окно: {:.1f}% на сутки {:.2f}\n", 100.0 * deepest_day,
                           deepest_day_time);
  std::cout << std::format("  глубочайшее ночное затемнение за окно: {:.1f}%\n", 100.0 * deepest_night);
}

} // namespace

bool observable_event_state::any() const {
  const auto positive = [](const double value) { return value > 0.0; };
  return parade || std::ranges::any_of(star_occultation, positive) ||
         std::ranges::any_of(lunar_eclipse, positive) || std::ranges::any_of(moon_occultation, positive);
}

void observe_events(const sky_state& state, const survey_options& options, observable_event_state& out) {
  const size_t moon_count = state.moons.size();
  out.star_occultation.fill(0.0);
  out.lunar_eclipse.assign(moon_count, 0.0);
  out.moon_occultation.assign(moon_count, 0.0);
  out.moon_occulting_body.assign(moon_count, -1);
  out.parade = false;
  out.parade_spread_deg = 0.0;

  for (size_t s = 0; s < state.stars.size(); ++s) {
    const auto& star = state.stars[s];
    if (star.horizon_fraction > 0.0 && star.occluded_fraction > options.min_star_occultation) {
      out.star_occultation[s] = star.occluded_fraction;
    }
  }

  const bool night = state.stars[0].horizon_fraction <= 0.0 && state.stars[1].horizon_fraction <= 0.0;
  for (size_t m = 0; m < moon_count; ++m) {
    const auto& moon = state.moons[m];
    if (night && moon.horizon_fraction > 0.0 && moon.occluded_fraction > options.min_lunar_eclipse) {
      out.lunar_eclipse[m] = moon.occluded_fraction;
    }
  }

  // Для каждой дальней луны сохраняется только самое глубокое покрытие: два ближних диска одновременно
  // практически невозможны, а календарю всё равно нужен один предмет и один закрывающий его объект.
  for (size_t far_index = 0; far_index < moon_count; ++far_index) {
    const auto& far = state.moons[far_index];
    for (size_t near_index = 0; near_index < moon_count; ++near_index) {
      if (near_index == far_index) continue;
      const auto& near = state.moons[near_index];
      if (near.distance_km >= far.distance_km) continue;
      if (near.horizon_fraction <= 0.0 || far.horizon_fraction <= 0.0) continue;
      const double separation = angle_between(near.direction, far.direction);
      const double fraction = disk_occluded_fraction(to_radians(separation), to_radians(far.angular_radius_deg),
                                                     to_radians(near.angular_radius_deg));
      if (fraction <= options.min_moon_occultation || fraction <= out.moon_occultation[far_index]) continue;
      out.moon_occultation[far_index] = fraction;
      out.moon_occulting_body[far_index] = 3 + static_cast<int32_t>(near_index);
    }
  }

  bool all_visible = true;
  for (const auto& star : state.stars) {
    all_visible = all_visible && star.horizon_fraction > 0.0;
  }
  for (const auto& moon : state.moons) {
    all_visible = all_visible && moon.horizon_fraction > 0.0;
  }
  if (all_visible) {
    const size_t body_count = state.stars.size() + moon_count;
    const auto direction = [&](const size_t index) -> const glm::dvec3& {
      return index < state.stars.size() ? state.stars[index].direction
                                        : state.moons[index - state.stars.size()].direction;
    };
    for (size_t a = 0; a < body_count; ++a) {
      for (size_t b = a + 1; b < body_count; ++b) {
        out.parade_spread_deg = std::max(out.parade_spread_deg, angle_between(direction(a), direction(b)));
      }
    }
    out.parade = out.parade_spread_deg <= options.parade_threshold_deg;
  }
}

observable_event_budget calculate_event_budget(const celestial_system& system, const survey_options& options) {
  observable_event_budget out;
  constexpr size_t kind_count = size_t(observable_event_kind::count);
  std::array<bool, kind_count> active{};
  std::array<bool, kind_count> seen{};
  bool unique_active = false;

  const auto update = [&](const observable_event_kind kind, const bool condition, const double magnitude) {
    const size_t index = size_t(kind);
    if (!condition) {
      active[index] = false;
      return;
    }
    if (!seen[index]) {
      out.categories[index].strongest = magnitude;
      seen[index] = true;
    } else if (kind == observable_event_kind::parade) {
      out.categories[index].strongest = std::min(out.categories[index].strongest, magnitude);
    } else {
      out.categories[index].strongest = std::max(out.categories[index].strongest, magnitude);
    }
    if (active[index]) return;
    active[index] = true;
    ++out.categories[index].count;
  };

  const double step_days = options.budget_step_minutes / minutes_per_day;
  const int32_t steps = static_cast<int32_t>(options.budget_span_days / step_days);
  const double start_days = system.from_calendar(1, 0, 0.0);
  observable_event_state observed;
  for (int32_t i = 0; i <= steps; ++i) {
    const auto state = system.evaluate(start_days + i * step_days);
    observe_events(state, options, observed);

    bool mutual = false;
    bool by_moon = false;
    double mutual_depth = 0.0;
    double by_moon_depth = 0.0;
    bool both_covered = true;
    double both_depth = 1.0;
    for (size_t s = 0; s < state.stars.size(); ++s) {
      const double depth = observed.star_occultation[s];
      const bool by_star = state.stars[s].occluder >= 0 && state.stars[s].occluder < 2;
      const bool by_moon_body = state.stars[s].occluder >= 3;
      mutual = mutual || (depth > 0.0 && by_star);
      by_moon = by_moon || (depth > 0.0 && by_moon_body);
      if (by_star) mutual_depth = std::max(mutual_depth, depth);
      if (by_moon_body) by_moon_depth = std::max(by_moon_depth, depth);
      if (depth > 0.0 && by_moon_body) both_depth = std::min(both_depth, depth);
      else both_covered = false;
    }
    const bool both_at_once = both_covered && state.stars[0].occluder == state.stars[1].occluder &&
                              state.stars[0].occluder >= 3;
    const double lunar_depth = observed.lunar_eclipse.empty()
                                 ? 0.0 : *std::ranges::max_element(observed.lunar_eclipse);
    const double moon_over_depth = observed.moon_occultation.empty()
                                     ? 0.0 : *std::ranges::max_element(observed.moon_occultation);

    update(observable_event_kind::mutual_stars, mutual, mutual_depth);
    update(observable_event_kind::star_by_moon, by_moon, by_moon_depth);
    update(observable_event_kind::lunar_eclipse, lunar_depth > 0.0, lunar_depth);
    update(observable_event_kind::moon_by_moon, moon_over_depth > 0.0, moon_over_depth);
    update(observable_event_kind::parade, observed.parade, observed.parade_spread_deg);
    update(observable_event_kind::moon_over_both_stars, both_at_once, both_depth);

    if (observed.any() && !unique_active) ++out.unique.count;
    unique_active = observed.any();
  }

  return out;
}

void run_survey(const celestial_system& system, const survey_options& options) {
  print_structure(system);
  print_seasons(system, options);
  print_binary_cycle(system, options);
  print_daytime_moons(system, options);
  print_parades(system, options);
  print_double_eclipses(system, options);
  print_event_budget(system, options);
  print_effect_scale(system, options);
}

} // namespace devils_engine::pf08
