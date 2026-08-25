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

void run_survey(const celestial_system& system, const survey_options& options);

} // namespace devils_engine::pf07

#endif
