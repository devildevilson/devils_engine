#ifndef DEVILS_ENGINE_PF07_SURVEY_H
#define DEVILS_ENGINE_PF07_SURVEY_H

// Обзор поведения небесной системы на длинных интервалах: сезоны, суточная структура, парады тел,
// двойные затмения и дневная видимость лун.
//
// Это не часть рантайма сцены, а инструмент понимания конфига: он отвечает на вопросы «что игрок
// увидит за год» и «как часто случается вот это» числами, которые пересчитываются при любой правке
// `system.tavl`. Все ответы получаются перебором состояний неба, без отдельной аналитики, поэтому
// не могут разойтись с тем, что позже увидит рендер.

#include "celestial.h"

#include <array>

namespace devils_engine::pf07 {

struct survey_options {
  double year_samples = 13.0;         // сколько дней года разобрать посуточно
  double day_step_minutes = 2.0;      // шаг внутри суток
  double parade_span_days = 1958.0;   // окно поиска парадов, примерно шесть лет
  double parade_step_minutes = 5.0;
  double eclipse_span_days = 3264.0;  // окно поиска двойных затмений, примерно десять лет
  double eclipse_step_minutes = 3.0;
  double parade_threshold_deg = 30.0;

  // Бюджет событий. Считаются только события, ВИДИМЫЕ наблюдателю: затмение под горизонтом для
  // игрока не существует, а геометрическая частота вдвое выше наблюдаемой.
  double budget_span_days = 3264.0;        // примерно десять лет планеты
  double budget_step_minutes = 2.0;
  double min_star_occultation = 0.05;      // ниже этого перекрытие не читается как событие
  double min_lunar_eclipse = 0.30;
  double min_moon_occultation = 0.10;
  double game_minutes_per_real_second = 1.0;

  // Деление эффектов на малые и крупные. Мерой служит не тип геометрии, а изменение света, которое
  // видит игрок: доля потерянной освещённости — дневной от светил, ночной от лун. Малое явление
  // читается как «небо чем-то занято», крупное меняет характер сцены целиком.
  double small_effect_dimming = 0.02;
  double large_effect_dimming = 0.15;
  // Второй признак крупного явления: светило гаснет практически целиком. Пропажа одного из двух
  // источников теней меняет картинку сильнее, чем следует из потери его доли света.
  double large_effect_star_occultation = 0.90;
};

// Один источник истины для `--events`, длинного бюджета и проверок. Здесь решается, существует ли
// геометрическое совпадение ДЛЯ ИГРОКА: тело должно быть над горизонтом, а лунное затмение — ещё и
// происходить после захода обоих светил. Форматирование календаря сюда намеренно не входит.
struct observable_event_state {
  std::array<double, 2> star_occultation{};
  std::vector<double> lunar_eclipse;
  std::vector<double> moon_occultation;
  std::vector<int32_t> moon_occulting_body;
  bool parade = false;
  double parade_spread_deg = 0.0;

  bool any() const;
};

void observe_events(const sky_state& state, const survey_options& options, observable_event_state& out);

enum class observable_event_kind : size_t {
  mutual_stars,
  star_by_moon,
  lunar_eclipse,
  moon_by_moon,
  parade,
  moon_over_both_stars,
  count
};

struct event_budget_counter {
  int32_t count = 0;
  double strongest = 0.0;
};

struct observable_event_budget {
  std::array<event_budget_counter, size_t(observable_event_kind::count)> categories{};
  event_budget_counter unique;
};

// Окно начинается в гражданскую полночь г1 д0. Это важно: машинная эпоха t=0 находится на 1.1 часа
// позже, и событие на правой границе меняло итог 66 -> 65 при внешне одинаковом «семилетнем» запросе.
observable_event_budget calculate_event_budget(const celestial_system& system, const survey_options& options);

void run_survey(const celestial_system& system, const survey_options& options);

} // namespace devils_engine::pf07

#endif
