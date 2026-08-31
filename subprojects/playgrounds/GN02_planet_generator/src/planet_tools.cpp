#include "planet_tools.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "devils_engine/utils/core.h"

namespace devils_engine::gn02 {

namespace {

using originator::aperture;
using originator::tool_call;
using originator::tool_description;
using originator::tool_registry;

struct vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

vec3 operator+(const vec3 a, const vec3 b) noexcept { return vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
vec3 operator-(const vec3 a, const vec3 b) noexcept { return vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
vec3 operator*(const vec3 a, const double s) noexcept { return vec3{a.x * s, a.y * s, a.z * s}; }

double dot(const vec3 a, const vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const vec3 a) noexcept { return std::sqrt(dot(a, a)); }

vec3 cross(const vec3 a, const vec3 b) noexcept {
  return vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

vec3 normalized(const vec3 a) noexcept {
  const double len = length(a);
  return len > 1e-12 ? a * (1.0 / len) : vec3{};
}

template <typename accessor_t>
vec3 read_vec3(const accessor_t& accessor, const size_t index) noexcept {
  return vec3{accessor.get(index, 0), accessor.get(index, 1), accessor.get(index, 2)};
}

template <typename accessor_t>
void write_vec3(const accessor_t& accessor, const size_t index, const vec3 value) noexcept {
  accessor.set(index, value.x, 0);
  accessor.set(index, value.y, 1);
  accessor.set(index, value.z, 2);
}

vec3 axis_vector(const int64_t axis) noexcept {
  switch (std::clamp<int64_t>(axis, 0, 2)) {
    case 0: return vec3{1.0, 0.0, 0.0};
    case 2: return vec3{0.0, 0.0, 1.0};
    default: return vec3{0.0, 1.0, 0.0};
  }
}

// Синус широты: проекция направления на ось вращения.
//
// Существует потому, что широта нужна ВСЕМУ дальше — ветрам, инсоляции, сезонам, — а достать
// компоненту вектора правило на devils_script не может: его скоуп это скалярные поля элемента.
// Значит компонента должна стать полем, и стоит она один pointwise-проход.
void tool_axis_component(const tool_call& call, const size_t begin, const size_t end) {
  const auto source = call.input(0).read();
  auto target = call.output(0).write();

  if (source.type().components < 3) {
    utils::error{}("GN02 step '{}': axis_component needs a 3-component field, '{}.{}' has {}",
                   call.step_name, call.input(0).buffer_name(), call.input(0).field_name(),
                   source.type().components);
  }

  const auto axis = std::clamp<int64_t>(call.params->integer("axis", 1), 0, 2);
  const bool normalize = call.params->integer("normalize", 1) != 0;

  for (size_t i = begin; i < end; ++i) {
    const vec3 point = read_vec3(source, i);
    const double len = normalize ? length(point) : 1.0;
    const double component = axis == 0 ? point.x : (axis == 2 ? point.z : point.y);
    target.set(i, len > 1e-12 ? component / len : 0.0);
  }
}

// Скорость поверхности плиты: v = ω × r.
//
// Плита на сфере не «едет в сторону» — она ПОВОРАЧИВАЕТСЯ вокруг своего полюса Эйлера, и это не
// придирка к терминологии: у линейного сдвига на замкнутой поверхности нет смысла, а у поворота
// есть, причём одна и та же плита у своего полюса почти стоит, а на экваторе от него движется
// быстро. Отсюда сами собой берутся и разные типы границ вдоль одного стыка.
void tool_plate_velocity(const tool_call& call, const size_t begin, const size_t end) {
  const auto positions = call.input(0).read();
  const auto axes = call.input(1).read();
  const auto rates = call.input(2).read();
  auto target = call.output(0).write();

  for (size_t i = begin; i < end; ++i) {
    const vec3 point = read_vec3(positions, i);
    const vec3 omega = normalized(read_vec3(axes, i)) * rates.get(i);
    write_vec3(target, i, cross(omega, point));
  }
}

// Взаимодействие плит на границе: сближение, сдвиг и сторона, которая уходит вниз.
//
// Тип границы здесь не объявляется списком, а СЧИТАЕТСЯ из скоростей: относительная скорость двух
// плит разлагается на составляющую вдоль стыка (сдвиг) и поперёк (сближение или расхождение).
// Поэтому один и тот же стык может быть сходящимся в одном месте и трансформным в другом — так же,
// как на настоящей планете.
//
// Кто уходит под кого, решает плотность коры: океаническая тонет под континентальной. При равной
// коре тонет плита с большим номером — тай-брейк произволен, но он ОБЯЗАН быть, иначе результат
// зависел бы от порядка обхода соседей.
void tool_plate_interaction(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto plates = call.input(2).read();
  const auto velocities = call.input(3).read();
  const auto positions = call.input(4).read();
  const auto crusts = call.input(5).read();

  auto convergence_out = call.output(0).write();
  auto shear_out = call.output(1).write();
  auto subduction_out = call.output(2).write();

  for (size_t i = begin; i < end; ++i) {
    const auto own_plate = uint32_t(plates.get(i));
    const vec3 own_position = read_vec3(positions, i);
    const vec3 own_normal = normalized(own_position);
    const vec3 own_velocity = read_vec3(velocities, i);
    const double own_crust = crusts.get(i);

    double dominant = 0.0;
    double dominant_abs = -1.0;
    double shear_max = 0.0;
    double subduction = 0.0;

    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    for (size_t k = first; k < last; ++k) {
      const size_t other = size_t(arcs.get(k));
      const auto other_plate = uint32_t(plates.get(other));
      if (other_plate == own_plate) {
        continue;
      }

      // Направление на соседа берётся КАСАТЕЛЬНЫМ: радиальная составляющая хорды к движению плит
      // отношения не имеет, а на грубой решётке она заметна.
      const vec3 chord = read_vec3(positions, other) - own_position;
      const vec3 direction = normalized(chord - own_normal * dot(chord, own_normal));
      if (length(direction) < 0.5) {
        continue;
      }

      const vec3 relative = own_velocity - read_vec3(velocities, other);
      const double closing = dot(relative, direction);
      const double slip = length(relative - direction * closing);
      shear_max = std::max(shear_max, slip);

      if (std::abs(closing) > dominant_abs) {
        dominant_abs = std::abs(closing);
        dominant = closing;

        const double other_crust = crusts.get(other);
        const bool sinks = other_crust > own_crust || (other_crust == own_crust && other_plate < own_plate);
        subduction = closing > 0.0 && sinks ? 1.0 : 0.0;
      }
    }

    convergence_out.set(i, dominant);
    shear_out.set(i, shear_max);
    subduction_out.set(i, subduction);
  }
}

// Поле приземного ветра: зональные пояса.
//
// Число поясов зависит от вращения, и это единственная честная связь ветров с ним: быстрое вращение
// разрезает одну ячейку Хэдли на несколько, а медленное сводит циркуляцию к одной ячейке на
// полушарие. Поэтому bands — параметр конфига, а не константа кода, и rotation отдельно смещает
// баланс между зональной и меридиональной составляющей: чем сильнее Кориолис, тем более
// «широтным» становится ветер.
void tool_wind_field(const tool_call& call, const size_t begin, const size_t end) {
  const auto positions = call.input(0).read();
  const auto latitudes = call.input(1).read();
  auto target = call.output(0).write();

  const vec3 axis = axis_vector(call.params->integer("axis", 1));
  const auto bands = size_t(std::clamp<int64_t>(call.params->integer("bands", 3), 1, 9));
  const double speed = call.params->number("speed", 1.0);
  const double meridional = call.params->number("meridional", 0.4);
  const double rotation = std::max(0.05, call.params->number("rotation", 1.0));

  const double band_width = (0.5 * std::numbers::pi) / double(bands);

  for (size_t i = begin; i < end; ++i) {
    const vec3 point = normalized(read_vec3(positions, i));
    const double latitude = std::asin(std::clamp(latitudes.get(i), -1.0, 1.0));
    const double hemisphere = latitude >= 0.0 ? 1.0 : -1.0;

    const auto band = std::min(size_t(std::abs(latitude) / band_width), bands - 1);
    // Пояса чередуются: у поверхности пассаты дуют к экватору и на запад, западные ветры — к полюсу
    // и на восток, полярные восточные снова к экватору.
    const double zonal_sign = band % 2 == 0 ? -1.0 : 1.0;
    const double meridional_sign = band % 2 == 0 ? -1.0 : 1.0;

    vec3 east = normalized(cross(axis, point));
    if (length(east) < 0.5) {
      // На самом полюсе восток не определён. Ветер там всё равно почти вертикальный, поэтому
      // берётся любое устойчивое касательное направление, а не оставляется ноль. Опорный вектор
      // выбирается НЕ параллельным оси: с осью X ссылка на X дала бы нулевое произведение и ветер
      // ровно ноль в полярной клетке.
      const vec3 reference = std::abs(axis.x) < 0.9 ? vec3{1.0, 0.0, 0.0} : vec3{0.0, 0.0, 1.0};
      east = normalized(cross(axis, reference));
    }
    const vec3 poleward = normalized(cross(point, east)) * hemisphere;

    const vec3 wind = east * (zonal_sign * rotation) + poleward * (meridional_sign * meridional / rotation);
    write_vec3(target, i, normalized(wind) * speed);
  }
}

// Один шаг переноса влаги по графу.
//
// Почему шаг, а не готовое поле осадков: перенос влаги — это состояние, которое движется, и его
// нельзя посчитать «на месте». Здесь один проход тянет влагу из НАВЕТРЕННОГО соседа, добавляет
// испарение с воды, отнимает осадки и накапливает их. Тело шага вызывает инструмент нужное число
// раз, перекидывая поля туда-обратно, — и каждый вызов остаётся параллельным gather'ом.
//
// Осадки копятся в СВОЁМ элементе выходного поля, поэтому накопление не делает проход
// последовательным: клетка читает и пишет только себя, и порядок обхода ни на что не влияет.
void tool_moisture_step(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto positions = call.input(2).read();
  const auto winds = call.input(3).read();
  const auto moisture = call.input(4).read();
  const auto land = call.input(5).read();
  const auto temperature = call.input(6).read();
  const auto height = call.input(7).read();

  auto moisture_out = call.output(0).write();
  const auto rain_in = call.output(1).read();
  auto rain_out = call.output(1).write();

  const double transport = std::clamp(call.params->number("transport", 0.94), 0.0, 1.0);
  const double evaporation = call.params->number("evaporation", 0.16);
  const double land_evaporation = call.params->number("land_evaporation", 0.03);
  const double capacity_base = call.params->number("capacity_base", 0.35);
  const double capacity_gain = call.params->number("capacity_gain", 0.03);
  const double convective = call.params->number("convective", 0.06);
  const double convective_reference = std::max(1.0, call.params->number("convective_reference", 26.0));
  // Показатель у конвективного слагаемого больше единицы намеренно. Линейная зависимость от
  // температуры даёт слишком ровный по широте дождь, и экватор в первом прогоне вышел суше
  // средних широт — то есть наоборот. Тёплый воздух не просто держит больше влаги: он ещё и
  // неустойчив, поэтому в тропиках дождь идёт непропорционально чаще.
  const double convective_exponent = std::max(0.1, call.params->number("convective_exponent", 2.0));
  const double orographic = call.params->number("orographic", 0.00055);
  const double ocean_rain = call.params->number("ocean_rain", 0.03);

  for (size_t i = begin; i < end; ++i) {
    const vec3 point = normalized(read_vec3(positions, i));
    const vec3 wind = read_vec3(winds, i);
    const vec3 wind_direction = normalized(wind);

    // Наветренный сосед — тот, направление на которого наиболее ПРОТИВОПОЛОЖНО ветру. На графе это
    // единственный смысл, который можно придать слову «откуда пришёл воздух».
    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    size_t upwind = i;
    double best = 0.0;
    for (size_t k = first; k < last; ++k) {
      const size_t other = size_t(arcs.get(k));
      const vec3 chord = read_vec3(positions, other) - point;
      const vec3 direction = normalized(chord - point * dot(chord, point));
      const double alignment = dot(direction, wind_direction);
      if (alignment < best) {
        best = alignment;
        upwind = other;
      }
    }

    const double own_temperature = temperature.get(i);
    const double capacity = std::max(0.05, capacity_base + capacity_gain * own_temperature);

    double carried = moisture.get(upwind) * transport;
    const bool water = land.get(i) == 0.0;
    // Испарение зависит от ёмкости, то есть от температуры: холодное море почти не питает воздух, и
    // именно поэтому полярные пустыни существуют при океане под боком.
    carried += (water ? evaporation : land_evaporation) * capacity;
    carried = std::min(carried, capacity);

    const double lift = std::max(0.0, height.get(i) - height.get(upwind));
    const double instability = std::clamp(own_temperature / convective_reference, 0.0, 1.0);
    double share = convective * std::pow(instability, convective_exponent) + orographic * lift;
    if (water) {
      share += ocean_rain;
    }
    share = std::clamp(share, 0.0, 1.0);

    const double rain = carried * share;
    moisture_out.set(i, carried - rain);
    rain_out.set(i, rain_in.get(i) + rain);
  }
}

// Инсоляция и температура сезона.
//
// Считаются СРАЗУ две величины: температура при нулевом наклонении (годовое положение) и при
// заданном сезоне. Разница между ними — сезонная амплитуда, и именно её усиливает
// континентальность: вода греется и остывает медленно, поэтому берег живёт почти без сезонов, а
// центр материка — с двумя разными климатами в одном месте. Без этой пары градиент «лето-зима»
// получился бы одинаковым на всей широте, что неверно грубо.
void tool_insolation(const tool_call& call, const size_t begin, const size_t end) {
  const auto latitudes = call.input(0).read();
  const auto height = call.input(1).read();
  const auto ocean_distance = call.input(2).read();
  auto target = call.output(0).write();

  const double season = std::clamp(call.params->number("season", 0.0), -1.0, 1.0);
  const double tilt = call.params->number("tilt", 23.44) * std::numbers::pi / 180.0;
  const double equator = call.params->number("equator_temperature", 27.0);
  const double pole = call.params->number("pole_temperature", -26.0);
  const double exponent = std::max(0.2, call.params->number("exponent", 1.25));
  const double lapse = call.params->number("lapse_rate", 6.5);
  const double continental_gain = call.params->number("continental_gain", 0.95);
  const double ocean_reference = std::max(1.0, call.params->number("ocean_reference", 8.0));

  const auto temperature_at = [&](const double latitude, const double declination) {
    const double insolation = std::max(0.0, std::cos(latitude - declination));
    return pole + (equator - pole) * std::pow(insolation, exponent);
  };

  for (size_t i = begin; i < end; ++i) {
    const double latitude = std::asin(std::clamp(latitudes.get(i), -1.0, 1.0));
    const double annual = temperature_at(latitude, 0.0);
    const double seasonal = temperature_at(latitude, tilt * season);

    // Недостигнутое расстояние (метка -1 у graph_flood) означает «океана рядом нет вовсе», то есть
    // максимальную континентальность, а не нулевую.
    const double distance = ocean_distance.get(i);
    const double inland = distance < 0.0 ? 1.0 : std::clamp(distance / ocean_reference, 0.0, 1.0);
    const double amplitude = 1.0 + continental_gain * inland;

    const double elevation = std::max(0.0, height.get(i));
    target.set(i, annual + (seasonal - annual) * amplitude - lapse * elevation / 1000.0);
  }
}

} // namespace

void add_planet_tools(tool_registry& registry) {
  registry.add(tool_description{.name = "axis_component", .shape = aperture::pointwise,
                                .input_count = 1, .output_count = 1, .body = tool_axis_component});
  registry.add(tool_description{.name = "plate_velocity", .shape = aperture::pointwise,
                                .input_count = 3, .output_count = 1, .body = tool_plate_velocity});
  registry.add(tool_description{.name = "plate_interaction", .shape = aperture::gather,
                                .input_count = 6, .output_count = 3, .body = tool_plate_interaction});
  registry.add(tool_description{.name = "wind_field", .shape = aperture::pointwise,
                                .input_count = 2, .output_count = 1, .body = tool_wind_field});
  registry.add(tool_description{.name = "moisture_step", .shape = aperture::gather,
                                .input_count = 8, .output_count = 2, .body = tool_moisture_step});
  registry.add(tool_description{.name = "insolation", .shape = aperture::pointwise,
                                .input_count = 3, .output_count = 1, .body = tool_insolation});
}

} // namespace devils_engine::gn02
