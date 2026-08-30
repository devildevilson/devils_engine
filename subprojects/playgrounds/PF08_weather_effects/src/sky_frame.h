#ifndef DEVILS_ENGINE_PF08_SKY_FRAME_H
#define DEVILS_ENGINE_PF08_SKY_FRAME_H

// Перевод состояния неба в блок для шейдера. Вынесено отдельно от рендера, потому что это чистая
// функция без Vulkan: её можно проверить численно, и именно здесь живёт единственная смена системы
// координат — небесный модуль работает в ENU (восток, север, зенит), мир рендера имеет Y вверх.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "celestial.h"

namespace devils_engine::pf08 {

constexpr size_t sky_star_count = 2;
constexpr size_t sky_moon_capacity = 4;

// Host-side gate условного multiscatter cache. Он не знает Vulkan, только число уже submitted frames:
// это делает требование doublebuffer проверяемым в headless --verify, а не спрятанным в оконном цикле.
class atmosphere_cache_gate {
public:
  explicit atmosphere_cache_gate(uint32_t minimum_frame_gap = 2) noexcept;
  bool try_rebuild(uint32_t submitted_frame) noexcept;

private:
  uint32_t minimum_frame_gap_ = 2;
  uint32_t last_rebuild_frame_ = 0;
  bool has_rebuilt_ = false;
};

// Раскладка обязана совпадать с `pf08_sky_block` в resources/shaders/pf08_records.glsl.
struct alignas(16) sky_gpu_block {
  glm::vec4 star_direction[sky_star_count];
  glm::vec4 star_color_illuminance[sky_star_count];
  glm::vec4 moon_direction[sky_moon_capacity];
  glm::vec4 moon_color_illuminance[sky_moon_capacity];
  glm::vec4 moon_phase[sky_moon_capacity];
  // x/y — доля Aurin/Ember, видимая с этой луны. Общая глубина затмения не сохраняет цвет остатка.
  glm::vec4 moon_star_visibility[sky_moon_capacity];
  glm::vec4 atmosphere_geometry;
  glm::vec4 atmosphere_medium;
  glm::vec4 march_params;
  glm::vec4 output_params;
  // Базис горизонта в инерциальной системе: звёздное поле закреплено за небом, а не за наблюдателем,
  // и обязано вращаться вместе с планетой.
  glm::vec4 sky_basis_east;
  glm::vec4 sky_basis_north;
  glm::vec4 sky_basis_up;
  // x — множитель размера рисуемых дисков, y — плотность звёзд, z — порог яркости звёзд, w — яркость
  // галактической полосы.
  glm::vec4 presentation_params;
  // Цветовой сценарий: rgb — оттенок, w — насыщенность.
  glm::vec4 grade_tint_saturation;
  // x — контраст, y — сила ночного зрения, z — множитель яркости короны, w — резерв.
  glm::vec4 grade_curve;
  // Какие ТЕЛА получили каскады теней, кодом: 0..1 — звезда с этим индексом, 2+m — луна m,
  // отрицательное — слот пуст. Без этого поля освещение и тени не нашли бы друг друга: система теней
  // берёт два самых ярких источника на небе, кем бы они ни были, и в цикле по звёздам и лунам иначе
  // нечем понять, чья это тень. x — первый слот, y — второй.
  glm::vec4 shadow_bodies;
  // Ветер: xy — направление в горизонтальной плоскости, z — сила в метрах отклонения верхушки,
  // w — время в секундах. Поле одно на весь мир, а не отдельное на каждый куст.
  glm::vec4 wind_params;
  // Локальная froxel-среда: x — extinction, 1/м; y — scattering albedo; z — HG anisotropy;
  // w — дальность объёма, м. Нулевой extinction включает точный clear-bypass композиции.
  glm::vec4 fog_params;
  // x — высота начала экспоненциального спада, м; y — scale height, м. zw зарезервированы для
  // пространственной модуляции следующего шага.
  glm::vec4 fog_shape;
  // x — амплитуда неоднородности; y — размер ячейки, м; z — скорость advection, м/с;
  // w — lighting stride froxel Z при активных осадках.
  glm::vec4 fog_noise;
  // Конечный облачный слой: coverage/extinction/albedo/anisotropy, его высоты и движение.
  glm::vec4 cloud_params;
  glm::vec4 cloud_shape;
  glm::vec4 cloud_motion;
  glm::vec4 precipitation_params;
  glm::vec4 precipitation_shape;
  glm::vec4 precipitation_time;
  glm::vec4 snow_params;
  glm::vec4 snow_shape;
  glm::vec4 shelter_minimum;
  glm::vec4 shelter_maximum;
  // x initial rain mm, y initial snow-water mm, z dry half-life hours, w visual response enabled.
  glm::vec4 surface_weather;
  // x map half extent, y cell size, z world seconds/real second, w snow melt mm/h.
  glm::vec4 surface_weather_shape;
  // x coverage, y world cell (m), z advection (m/s), w edge softness.
  glm::vec4 precipitation_field;
  // x splash extinction, y splash height, z rain mid radius, w snow mid radius.
  glm::vec4 precipitation_mist_lod;
  // Освещённость светила БЕЗ затмения и без горизонта, по одной на звезду в x и y. Нужна ровно диску:
  // затмение теперь показывается геометрически — луна закрывает часть диска собой, — и дополнительно
  // гасить сам диск значило бы посчитать затмение дважды.
  glm::vec4 star_disc_illuminance;
  // Расстояния до лун, км. Нужны, чтобы при наложении дисков ближняя луна закрывала дальнюю. Порядок
  // лун в массиве при этом НЕ переставляется: по индексу луны его читает система теней, и сортировка
  // здесь развела бы освещение с затенением без единого предупреждения.
  glm::vec4 moon_distance_km;
  // x intensity, y saturation, z band width, w edge sharpness.
  glm::vec4 rainbow_appearance;
  // x veil, y local background contrast, z memory persistence, w current-rain cutoff mm/h.
  glm::vec4 rainbow_context;
  // x source mode, y secondary-order bow, z source balance, w source separation scale.
  glm::vec4 rainbow_sources;
  // x intensity, y active microfacet density, z angular sharpness, w stellar source balance.
  glm::vec4 snow_sparkle;
  // Универсальное событие молнии. Геометрия хранится отдельно от двух временных envelope: дальняя
  // молния может потерять sub-pixel channel, но обязана оставить объёмную и поверхностную вспышку.
  glm::vec4 lightning_start_channel;
  glm::vec4 lightning_end_flash;
  glm::vec4 lightning_colour_intensity;
  // xyz — channel radius/luminance/glow radius, w — deterministic path seed. Автор остаётся CPU
  // metadata: weather и magic намеренно не выбирают разные shader branches.
  glm::vec4 lightning_shape;
  // Верхнеатмосферное сияние: художественная заметность отделена от сферической геометрии слоя и
  // магнитной привязки. Нулевая intensity — точный ранний выход прежнего неба.
  glm::vec4 aurora_appearance;
  glm::vec4 aurora_geometry;
  glm::vec4 aurora_magnetic;
};
static_assert(sizeof(sky_gpu_block) == 992);

struct atmosphere_settings {
  double height_km = 100.0;         // верх атмосферы над поверхностью
  double rayleigh_scale_km = 8.0;
  double mie_scale_km = 1.2;
  double ozone_center_km = 25.0;
  double ozone_width_km = 15.0;
  double mie_anisotropy = 0.76;
  // Атмосферный consumer погодного состояния. Поле остаётся здесь, потому что pack_sky_block принимает
  // готовый snapshot среды и не должен знать, пришёл он из пресета, перехода или CLI override.
  double turbidity = 1.0;
  double ground_albedo = 0.10;
};

struct march_settings {
  double camera_height_km = 0.002;
  int32_t primary_steps = 32;
  // Дальность таблицы воздушной перспективы, км. С высоты в два метра горизонт лежит в пяти
  // километрах, поэтому восьми хватает на всю видимую поверхность с запасом; рельефу срезов 4-5
  // понадобится больше.
  double aerial_range_km = 8.0;
};

// Экспозиция задаётся фотографически, через EV100, а не безымянным множителем.
//
// Мерится ЯРКОСТЬ кадра, а не освещённость сцены. Разница принципиальная и стоила отдельной ошибки:
// освещённость — это падающий свет, и она верна для сцены, которую свет освещает. Небо светом не
// освещается, оно само источник, и в сумерках занимает весь кадр целиком. Замер по освещённости такие
// кадры систематически пересвечивал — на рассвете он открывал экспозицию почти на три ступени сверх
// нужного, и небо выходило белым при формально верных числах.
//
// Отражённый замер при ISO 100 и калибровочной постоянной 12.5 даёт EV100 = log2(L * 100 / 12.5).
// Такая экспозиция выводит ЛЮБУЮ сцену в средне-серое — то есть делает полдень и полночь одинаково
// серыми. Для фотоаппарата это правильно, для мира — нет, поэтому за замером идёт единственный
// рычаг: полнота адаптации.
struct exposure_settings {
  bool manual = false;
  double manual_ev100 = 13.0;
  // Точка отсчёта: измеренная яркость полуденного кадра этого мира в единицах EV100. Кривая проходит
  // через неё, и `bias_stops` — привычная экспокоррекция фотографа именно в этой точке.
  double reference_ev100 = 14.3;
  double bias_stops = -1.9;
  // Полнота адаптации: доля, на которую экспозиция следует за яркостью сцены. Единица означает полное
  // привыкание — все состояния мира выходят средне-серыми, и ночь неотличима от дня по яркости. Ноль
  // означает фиксированную экспозицию.
  //
  // Это главный рычаг среза, и он заменил собой прежнюю пару «колено плюс компенсация». Колено
  // ломало шкалу пополам: яркие состояния оно не трогало вовсе, а тёмные тянуло к своему порогу, и
  // из-за этого рассвет оказывался на ступень СВЕТЛЕЕ средне-серого, тогда как ночь — на две ступени
  // темнее. Ослепляющая заря была ровно этим изломом, а не ошибкой в физике неба.
  //
  // Одна доля на все восемнадцать ступеней диапазона держит порядок сама: чем темнее сцена, тем
  // дальше её кадр уходит в темноту, монотонно и без порогов.
  //
  // Значение выбрано по ИЗМЕРИМОМУ признаку, а не на вкус: доля кадра, ушедшая в чистый белый на
  // рассвете. При 0.75 это 9.34% — сплошное выбеленное пятно вдоль горизонта, съедающее весь цвет
  // зари; при 0.65 остаётся 0.06%, то есть пересвет исчезает. Ниже опускаться нельзя по другому
  // измеряемому признаку: при 0.55 лунная освещённость земли падает с 11 до 6 и перестаёт читаться,
  // а ночь при полной луне обязана оставаться лунной. Между двумя порогами места почти нет, и это
  // не совпадение — оба упираются в один и тот же динамический диапазон.
  double adaptation_strength = 0.65;
  // Пол и потолок адаптации. Пол — это НЕ ограничение сверху на темноту, а отказ вытягивать ночь:
  // ниже него экспозиция не растёт, и безлунная ночь остаётся чёрной. Правило PF06 «renderer не
  // выдумывает видимость» здесь превращается в одно число.
  double min_ev100 = -6.0;
  double max_ev100 = 17.0;
  // Постоянные времени в РЕАЛЬНЫХ секундах. Глаз привыкает к свету быстро, а к темноте медленно, и
  // несимметричность здесь не украшение: именно она делает закат событием, а не сменой яркости.
  double adapt_brighter_seconds = 1.2;
  double adapt_darker_seconds = 6.0;
  // Темп игрового времени, для которого заданы постоянные выше, и это ОБЯЗАТЕЛЬНАЯ часть их смысла.
  //
  // Секунды здесь реальные, потому что привыкает глаз игрока, а не планета. Но при перемотке времени
  // такая привязка ломается: на тридцатикратной скорости восход укладывается в четверть реальной
  // секунды, тогда как глазу нужно больше секунды, экспозиция отстаёт на четыре ступени и небо стоит
  // белым, пока она догоняет. Ускорение времени — это ускоренная съёмка, и выдержка в ней обязана
  // идти за сценой, иначе кадр перестаёт нести изображение вообще.
  //
  // Поэтому постоянные сжимаются пропорционально ускорению. Растягиваться при ЗАМЕДЛЕНИИ они не
  // должны: на паузе игровое время стоит, а камера ходит, и адаптация обязана продолжать работать.
  double reference_time_scale = 1.0 / 1440.0;
};

enum class rainbow_source_mode : uint32_t {
  primary = 0,
  brightest = 1,
  all = 2
};

// Художественная заметность отделена от геометрии. Радиус дуги и anti-solar direction остаются
// физическими при separation_scale=1, но проект волен сделать эффект ярче, чище и долговечнее.
struct rainbow_settings {
  double intensity = 1.0;
  double saturation = 1.15;
  double width = 1.0;
  double sharpness = 1.0;
  double veil_strength = 1.0;
  double background_contrast = 0.08;
  double persistence = 1.0;
  double rain_cutoff_mm_h = 8.0;
  rainbow_source_mode sources = rainbow_source_mode::all;
  double secondary_bow_strength = 0.0;
  // Ноль сохраняет физическое отношение illuminance двух светил; единица делает обе дуги равными.
  double source_balance = 0.65;
  // Художественное увеличение углового расстояния между источниками только для радуг.
  double source_separation_scale = 1.0;
};

// Снежное мерцание — не ещё один material BRDF, а управляемая презентация множества ледяных
// микрограней. Геометрия блика остаётся specular, проект выбирает лишь его читаемость и то, насколько
// сильно второе светило разрешено поднять относительно физической освещённости.
struct snow_sparkle_settings {
  double intensity = 6.0;
  double density = 0.70;
  double sharpness = 0.50;
  double source_balance = 0.65;
};

bool valid_snow_sparkle_settings(const snow_sparkle_settings& settings);

struct aurora_settings {
  double intensity = 0.0;
  double saturation = 1.25;
  double curtain_density = 0.38;
  // Ноль оставляет только естественную ночную наблюдаемость; единица разрешает fantasy daylight aurora.
  double daylight_visibility = 0.0;
  double lower_altitude_km = 90.0;
  double upper_altitude_km = 240.0;
  double oval_angle_deg = 18.0;
  double oval_width_deg = 4.0;
  double magnetic_tilt_deg = 16.0;
  double magnetic_azimuth_deg = 330.0;
  double curtain_bands = 72.0;
  double drift_deg_per_second = 0.12;
};

bool valid_aurora_settings(const aurora_settings& settings);

struct output_settings {
  double exposure = 1.0e-4;
  // Ночное зрение: при низкой яркости колбочки перестают работать, цвет уходит и картинка холодает.
  // Ноль полностью отключает эффект.
  double scotopic_strength = 1.0;
  glm::vec3 grade_tint{1.0f, 1.0f, 1.0f};
  double grade_saturation = 1.0;
  double grade_contrast = 1.0;
  double debug_mode = 0.0;
  // Диски светил и лун физически малы: у Selen 0.90°, что при поле зрения 65° занимает десяток
  // пикселей. Это осознанное преувеличение для читаемости, и оно НЕ трогает освещённость: диск
  // становится крупнее, но света от него в сцене столько же. Угловые радиусы, которыми считаются
  // затмения, живут в небесной механике и этим множителем не затрагиваются.
  double disc_scale = 3.0;
  double star_density = 1.0;
  double star_brightness = 1.0;
  // Галактическая полоса выражена ПЛОТНОСТЬЮ звёзд, а не собственной светимостью: у россыпи точек
  // не бывает видимого края, а у размытой ленты света он был. Единица примерно утраивает занятость
  // ячеек в плоскости галактики, ноль делает небо равномерным.
  double galaxy_concentration = 1.0;
  // Доля физической скорости вращения звёздного неба. Сутки здесь идут за двадцать четыре реальные
  // минуты, поэтому честное вращение выглядит вертолётом; единица возвращает физику, ноль полностью
  // останавливает небо. Луны и светила при этом продолжают идти по-настоящему.
  // Яркость короны в долях физической. Корона — внешняя атмосфера светила, она в миллион раз слабее
  // фотосферы и потому видна только когда диск закрыт. В одиночной системе этого хватает: при полном
  // затмении небо темнеет в тысячу раз. Здесь второе светило продолжает светить, небо остаётся
  // дневным, и физическая корона едва превышает его яркость. Множитель — осознанная неправда того же
  // рода, что `disc_boost` у мелких лун, и он ничего не меняет в освещении сцены.
  //
  // Значение выбрано по картинке: при физической единице кольцо едва отличается от неба, при
  // двадцати пяти превращается в размытое пятно. Шестёрка даёт читаемый ободок, не трогая при этом
  // незатменные светила — они остаются чёткими дисками, потому что корона проявляется по мере
  // закрытия диска, см. `sky.frag.glsl`.
  double corona_strength = 6.0;
  double star_rotation_scale = 0.15;
  rainbow_settings rainbow;
  snow_sparkle_settings snow_sparkle;
  aurora_settings aurora;
};

// Ключ цветового сценария. Хранится по высоте главного светила: именно она задаёт время суток.
struct colour_script_key {
  double sun_altitude_deg = 0.0;
  std::vector<double> tint;
  double saturation = 1.0;
  double contrast = 1.0;
};

struct colour_script {
  std::vector<colour_script_key> keys;
};

bool parse_colour_script(const std::string& text, colour_script& out, std::string& diagnostics);

// Именованное состояние неба. Время, наведение камеры и зафиксированная экспозиция — три вещи, без
// любой из которых два дампа перестают быть сравнимыми.
struct view_preset {
  std::string name;
  // Время задаётся абсолютным годом календаря, а не якобы повторяющимся годом цикла: семилетний beat
  // двойной имеет остаточную ошибку и вообще не включает фазы лун.
  uint32_t year = 1;
  uint32_t day = 0;
  double hour = 12.0;
  double look_azimuth_deg = 0.0;
  double look_altitude_deg = 10.0;
  double ev100 = 13.0;
  // Отрицательные значения не меняют renderer presentation. Только специализированные showcase
  // пресеты вроде aurora вправе задать их сами; CLI override всё равно имеет приоритет.
  double aurora_intensity = -1.0;
  double scotopic_strength = -1.0;
};

struct view_preset_list {
  std::vector<view_preset> presets;
};

// Загрузка пресетов из tavl. Возвращает false и заполняет diagnostics при ошибке разбора.
bool parse_view_presets(const std::string& text, view_preset_list& out, std::string& diagnostics);

// Направление из локальной горизонтальной системы (x восток, y север, z зенит) в мир рендера
// (x восток, y вверх, z на юг).
glm::vec3 horizon_to_world(const glm::dvec3& horizon_direction);

// `star_frame` даёт базис горизонта для звёздного поля. Обычно это то же состояние, но при замедленном
// вращении неба сюда приходит состояние, посчитанное на замедленное время.
sky_gpu_block pack_sky_block(const sky_state& state, const sky_state& star_frame,
                             const atmosphere_settings& atmosphere, const march_settings& march,
                             const output_settings& output, const double planet_radius_km,
                             const std::vector<moon_config>& moons);

} // namespace devils_engine::pf08

#endif
