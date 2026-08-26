#ifndef DEVILS_ENGINE_PF07_CELESTIAL_H
#define DEVILS_ENGINE_PF07_CELESTIAL_H

// Небесная механика PF07: две звезды P-type двойной, планета вокруг барицентра, произвольное число лун.
//
// Модуль полностью аналитический и не хранит состояния между вызовами: `evaluate(t)` зависит только от
// конфига и от времени. Поэтому прыжок в любой момент времени и пошаговое накопление дают побитно
// одинаковый результат, что и является критерием закрытия среза 1.
//
// Все внутренние вычисления идут в километрах и радианах, double. Освещённость — перпендикулярная
// (на площадку, нормальную к лучу), ВНЕ атмосферы: атмосферное поглощение принадлежит срезу 2 и здесь
// сознательно не моделируется. Единственное исключение из «вне атмосферы» — отсутствие рефракции у
// горизонта, что тоже относится к срезу 2.
//
// Наблюдатель находится на поверхности планеты, поэтому положения тел топоцентрические: параллакс луны
// на расстоянии 180 тыс. км достигает двух градусов, то есть четырёх её диаметров, и без него полосы
// затмений не существует в принципе.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

namespace devils_engine::pf07 {

// --- конфигурация (tavl) ---

struct star_config {
  std::string name = "star";
  double mass_solar = 1.0;
  double radius_solar = 1.0;
  double temperature_kelvin = 5772.0;
};

// Кеплеровы элементы. Плоскость двойной является опорной: её наклонение и узел равны нулю по
// определению, наклонения остальных орбит отсчитываются от неё.
struct orbit_config {
  double semi_major_axis = 1.0;         // AU для орбит вокруг барицентра, км для лун
  double eccentricity = 0.0;
  double inclination_deg = 0.0;
  double ascending_node_deg = 0.0;
  double periapsis_argument_deg = 0.0;
  double mean_anomaly_epoch_deg = 0.0;  // средняя аномалия при time_days == 0
};

struct moon_config {
  std::string name = "moon";
  double radius_km = 1000.0;
  double albedo = 0.12;
  // Множители яркости и размера РИСУЕМОГО диска. Чистая презентация: физику освещения, затмений и
  // бюджета событий не трогают.
  double disc_boost = 1.0;
  double disc_visual_scale = 1.0;
  orbit_config orbit;                   // semi_major_axis в километрах, вокруг центра планеты
};

struct planet_config {
  double radius_km = 6371.0;
  double mass_earth = 1.0;              // задаёт периоды лун через GM
  double rotation_period_hours = 24.0;  // звёздные сутки
  double axial_tilt_deg = 23.4;         // от нормали к плоскости двойной
  double axis_azimuth_deg = 0.0;        // направление наклона оси в опорной плоскости
  double prime_meridian_epoch_deg = 0.0;
  orbit_config orbit;                   // semi_major_axis в AU, вокруг барицентра
};

struct observer_config {
  double latitude_deg = 45.0;
  double longitude_deg = 0.0;
};

struct system_config {
  star_config primary;
  star_config companion;
  orbit_config binary;                  // относительная орбита companion вокруг primary
  planet_config planet;
  observer_config observer;
  std::vector<moon_config> moons;
};

// --- производные свойства, считаются один раз ---

struct star_properties {
  double luminosity_solar = 1.0;
  double luminous_efficacy_lm_per_w = 0.0;  // световая отдача спектра Планка этой температуры
  double illuminance_at_1au_lx = 0.0;       // перпендикулярная, вне атмосферы
  glm::dvec3 color_linear{1.0, 1.0, 1.0};   // linear sRGB, нормирован по максимальному каналу
};

// --- мгновенное состояние ---

enum class body_kind : uint32_t { star, moon };

struct body_view {
  std::string_view name;
  body_kind kind = body_kind::star;
  glm::dvec3 direction{0.0, 0.0, 0.0};   // ENU: x — восток, y — север, z — зенит
  double altitude_deg = 0.0;
  double azimuth_deg = 0.0;              // от севера через восток
  double distance_km = 0.0;
  double angular_radius_deg = 0.0;
  // Звёзды: доля площади собственного диска, закрытая другим телом. Луны: доля потерянного
  // освещения, то есть глубина лунного затмения. В обоих случаях 0 означает «затмения нет».
  double occluded_fraction = 0.0;
  double horizon_fraction = 1.0;         // доля площади диска выше геометрического горизонта
  double illuminance_lx = 0.0;           // перпендикулярная, уже с учётом затмения и горизонта
  double unocculted_lx = 0.0;            // она же без затмения, но с учётом горизонта
  // Перпендикулярная освещённость с учётом затмения, но БЕЗ горизонта. Именно её потребляет
  // атмосферный рендер: горизонт у каждой точки атмосферы свой, и верхние слои продолжают видеть
  // светило, когда наблюдатель уже нет. Из этого и состоят сумерки.
  double space_illuminance_lx = 0.0;
  glm::dvec3 color_linear{1.0, 1.0, 1.0};
  double phase = 1.0;                    // луны: освещённая доля диска; звёзды: всегда 1
  int32_t occluder = -1;                 // индекс тела в общем списке или -1
};

struct sky_state {
  double time_days = 0.0;
  // Локальный горизонтальный базис, выраженный в инерциальной системе. Нужен всему, что закреплено
  // за небом, а не за наблюдателем: звёздное поле обязано вращаться вместе с планетой.
  glm::dvec3 east_inertial{1.0, 0.0, 0.0};
  glm::dvec3 north_inertial{0.0, 1.0, 0.0};
  glm::dvec3 up_inertial{0.0, 0.0, 1.0};
  std::array<body_view, 2> stars{};
  std::vector<body_view> moons;
  double horizontal_illuminance_lx = 0.0;   // сумма вкладов всех тел с учётом наклона к горизонту
  // Та же сумма, посчитанная так, будто затмений нет. Отношение одного к другому и есть та величина,
  // по которой событие делится на малое и крупное: игрок видит не геометрию, а изменение света.
  double unocculted_star_illuminance_lx = 0.0;
  double unocculted_moon_illuminance_lx = 0.0;
  double star_illuminance_lx = 0.0;
  double moon_illuminance_lx = 0.0;
};

// --- система ---

class celestial_system {
public:
  explicit celestial_system(system_config config);

  const system_config& config() const;
  const star_properties& star(const size_t index) const;

  double binary_period_days() const;
  double planet_year_days() const;
  double moon_period_days(const size_t index) const;
  // Средний интервал между соединениями звёзд, то есть между их взаимными затмениями.
  double star_conjunction_interval_days() const;

  // Число оборотов двойной за год планеты. Снизу ограничено устойчивостью и не может быть меньше
  // примерно четырёх: медленная двойная и широко разнесённые солнца — это одно и то же требование.
  double binary_revolutions_per_year() const;
  // Отношение фактического a_планеты/a_двойной к критическому. Меньше единицы — орбита неустойчива.
  double stability_margin() const;
  // Максимальное угловое разделение светил, наблюдаемое с планеты.
  double maximum_star_separation_deg() const;

  sky_state evaluate(const double time_days) const;

  // Барицентрические положения тел. Нужны проверкам среза 1 и будущим debug-видам неба; обычный
  // потребитель берёт направления и освещённости из `evaluate`.
  struct body_positions {
    std::array<glm::dvec3, 2> stars{};
    glm::dvec3 planet{0.0, 0.0, 0.0};
    std::vector<glm::dvec3> moons;
    glm::dvec3 observer{0.0, 0.0, 0.0};
  };
  body_positions positions(const double time_days) const;

private:
  struct body_position {
    glm::dvec3 position{0.0, 0.0, 0.0};  // км, барицентрическая инерциальная система
    double radius_km = 0.0;
  };

  struct illumination_sample {
    double illuminance_lx = 0.0;      // с учётом затмевателей
    double unoccluded_lx = 0.0;       // без них; хранится отдельно, потому что при полном
                                      // перекрытии восстановить его делением уже невозможно
    double occluded_fraction = 0.0;
    int32_t occluder = -1;
  };

  struct instant {
    std::array<body_position, 2> stars{};
    body_position planet{};
    std::vector<body_position> moons;
    glm::dvec3 observer{0.0, 0.0, 0.0};
    glm::dvec3 east{0.0, 0.0, 0.0};
    glm::dvec3 north{0.0, 0.0, 0.0};
    glm::dvec3 up{0.0, 0.0, 0.0};
  };

  instant sample(const double time_days) const;
  // Освещённость от светила в произвольной точке пространства с учётом затмевателей. Именно этот
  // вызов из точки луны и даёт лунное затмение: затмевателем оказывается сама планета.
  illumination_sample star_illuminance_at(const size_t star_index, const glm::dvec3& point, const instant& now,
                                          const int32_t ignored_moon) const;

  system_config config_;
  std::array<star_properties, 2> stars_{};
  double binary_period_days_ = 0.0;
  double planet_year_days_ = 0.0;
  std::vector<double> moon_periods_days_;
};

// --- отдельно проверяемые примитивы ---

// Уравнение Кеплера решается фиксированным числом итераций Ньютона, а не по достижению точности:
// одинаковое число операций на всех платформах — часть требования побитной воспроизводимости.
double solve_kepler(const double mean_anomaly_rad, const double eccentricity);

// Доля площади диска источника, закрытая диском затмевателя. Оба диска считаются однородными:
// потемнение к краю относится к срезу 2, где появится собственно диск светила.
double disk_occluded_fraction(const double separation_rad, const double source_radius_rad,
                              const double occluder_radius_rad);

// Доля площади диска выше горизонта, для тела, чей центр стоит на высоте altitude_rad.
double disk_fraction_above_horizon(const double altitude_rad, const double radius_rad);

// Критическая большая полуось устойчивой циркумбинарной (P-type) орбиты, в единицах большой полуоси
// двойной (Holman & Wiegert 1999). mass_ratio = M_компаньона / (M_главной + M_компаньона).
double holman_wiegert_critical_ratio(const double eccentricity, const double mass_ratio);

// Световая отдача излучения Планка, лм/Вт. Для 5772 K даёт около 92, для 4600 K — около 72:
// компаньон отдаёт заметно меньше видимого света, чем следует из его болометрической светимости.
double blackbody_luminous_efficacy(const double temperature_kelvin);

// Цвет излучения Планка в linear sRGB, нормированный по максимальному каналу.
glm::dvec3 blackbody_color_linear(const double temperature_kelvin);

// Загрузка конфига из tavl-текста. Возвращает false и заполняет diagnostics при ошибке разбора.
bool parse_system_config(const std::string& text, system_config& out, std::string& diagnostics);

} // namespace devils_engine::pf07

#endif
