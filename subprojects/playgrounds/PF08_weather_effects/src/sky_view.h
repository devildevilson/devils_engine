#ifndef DEVILS_ENGINE_PF08_SKY_VIEW_H
#define DEVILS_ENGINE_PF08_SKY_VIEW_H

// Оконный слой среза 2: показывает небо, посчитанное из той же небесной механики, что и headless-отчёты.
// Отделён от `main.cpp`, чтобы командные режимы среза 1 не тянули за собой Vulkan.

#include <cstdint>
#include <string>

#include "celestial.h"
#include "shadows.h"
#include "sky_frame.h"
#include "weather.h"

namespace devils_engine::pf08 {

struct view_options {
  uint32_t width = 1280;
  uint32_t height = 720;
  bool validation = false;
  bool uncapped = false;
  uint32_t frames = 0;              // ноль — до закрытия окна
  std::string dump_path;            // непустой путь дампит кадр перед выходом
  bool show_overlay = true;         // эталонные кадры отключают меняющиеся FPS/timestamps
  double start_time_days = 0.0;
  // Старт по абсолютному календарному году. Ноль означает «не задано»: тогда действует
  // `start_time_days`. Положение внутри семилетнего beat выводится отдельно и не подменяет дату.
  uint32_t start_year = 0;
  uint32_t start_day_of_year = 0;
  double start_hour = 0.0;
  // Игровых суток за реальную секунду. Значение по умолчанию совпадает с темпом, от которого посчитан
  // бюджет событий: одна игровая минута за реальную секунду, то есть сутки за двадцать четыре минуты.
  // Прежние 0.02 давали сутки за пятьдесят СЕКУНД — отсюда и вертолётное вращение неба.
  double time_scale = 1.0 / 1440.0;
  bool paused = false;
  // Покадровая печать состояния экспозиции. Переходный процесс адаптации по одному дампу не виден
  // вообще: кадр показывает результат, а не то, догоняет ли экспозиция цель и как долго.
  bool trace_exposure = false;
  // Дальность каскадов теней в метрах. Опорная геометрия среза заканчивается на сорока пяти метрах,
  // и растягивать каскады на пустую землю значит тратить их разрешение впустую.
  double shadow_far_m = 60.0;
  // Сколько источников получают каскады. Не столько настройка качества, сколько ИНСТРУМЕНТ СРАВНЕНИЯ:
  // разница кадров при одном и при двух источниках и есть вторая тень, отделённая от всего прочего.
  uint32_t shadow_sources = shadow_source_count;
  // Радиус диска земли в метрах. Шесть километров с запасом перекрывают горизонт с высоты глаза
  // (5048 м при двух метрах), а дальше поверхность всё равно скрывает сама себя.
  double ground_radius_m = 6000.0;
  // Заросли. Число кустов, дальность отрисовки и граница между полным и упрощённым мешем — в метрах.
  uint32_t foliage_count = 6000;
  double foliage_range_m = 130.0;
  double foliage_lod_m = 35.0;
  // В скольких ближних каскадах заросли отбрасывают тень. Ноль — не отбрасывают вовсе. Смысл ключа
  // тот же, что у `--shadow-sources`: сравнивать кадры и цену, а не «настраивать качество».
  uint32_t foliage_shadow_cascades = cascade_count;
  // Доля логарифмического разбиения каскадов против равномерного, см. `build_cascades`.
  //
  // Значение выбрано измерением доли ЗАТЕНЁННОЙ земли, то есть тем, сколько травы реально даёт тень.
  // Классические 0.85 проигрывали во всех трёх полосах кадра сразу: нулевой каскад доставал лишь до
  // 3.7 м, и уже ближняя трава попадала в грубые тексели дальнего. При 0.40 ближняя полоса даёт 4.4%
  // затенения против 3.7%, средняя 3.3% против 2.4%, дальняя не меняется.
  double cascade_split_lambda = 0.40;
  // Ветер: направление в градусах от севера через восток и сила в метрах отклонения верхушки.
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
  double fog_range_m = 220.0;
  // Preset заполняет единое погодное состояние; прямые CLI-рычаги ниже могут независимо перекрыть
  // его поля для A/B. Отрицательная длительность означает значение из weather/presets.tavl.
  std::string weather_preset = "clear";
  double weather_transition_seconds = -1.0;
  bool weather_transition_overridden = false;
  bool turbidity_overridden = false;
  bool wind_direction_overridden = false;
  bool wind_strength_overridden = false;
  bool fog_extinction_overridden = false;
  bool fog_albedo_overridden = false;
  bool fog_anisotropy_overridden = false;
  bool fog_base_overridden = false;
  bool fog_height_overridden = false;
  bool fog_variation_overridden = false;
  bool fog_cell_overridden = false;
  bool fog_speed_overridden = false;
  // Фиксированное наведение камеры для повторяемых дампов: сравнивать состояния можно только из
  // одной и той же точки зрения, а мышь для этого не годится.
  bool fixed_look = false;
  double look_azimuth_deg = 0.0;    // от севера через восток
  double look_altitude_deg = 10.0;

  exposure_settings exposure;
  // Именованное состояние: время, наведение камеры и ЗАФИКСИРОВАННАЯ экспозиция. Сравнивать состояния
  // между собой можно только при фиксированном EV — правило PF03, здесь оно становится требованием к
  // каждому пресету.
  std::string preset;

  atmosphere_settings atmosphere;
  march_settings march;
  output_settings output;
};

int run_sky_view(const celestial_system& system, const view_options& options);

} // namespace devils_engine::pf08

#endif
