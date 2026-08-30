#ifndef DEVILS_ENGINE_UTILS_EASING_H
#define DEVILS_ENGINE_UTILS_EASING_H

// Кривые смягчения с easings.net плюс то, чего там нет и без чего они обычно применяются неправильно.
//
// ВАЖНОЕ РАЗЛИЧИЕ, из-за которого этот заголовок состоит из двух половин.
//
// Кривая смягчения — это функция `[0,1] -> [0,1]`, переводящая ДОЛЮ ПРОЙДЕННОГО ВРЕМЕНИ в долю пройденного
// пути. Она описывает переход из A в B за ИЗВЕСТНУЮ длительность: щелчок колеса, открытие панели, полёт
// камеры к выбранной точке. У такого перехода есть начало, конец и продолжительность, и без них кривая
// бессмысленна.
//
// Непрерывный ввод — зажатая клавиша, стик, мышь — устроен иначе: у него нет ни цели, ни длительности,
// есть только СКОРОСТЬ, которая включается и выключается. Приложить сюда `ease_out_cubic` не к чему:
// подставить в неё нечего, кроме выдуманного «времени с начала нажатия», и на отпускании клавиши всё
// равно получится разрыв. Здесь нужны фильтры — `approach` и `smooth_damp`, — и они ниже, во второй
// половине. Смешивать эти два инструмента — обычная причина камеры, которая «плывёт» и не слушается.
//
// Практическое правило: дискретное событие с длительностью — кривая; удерживаемый ввод — фильтр.
// Хорошая камера пользуется обоими: колесо зума ведёт `tween`, а газ панорамирования — `approach`.

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>

namespace devils_engine {
namespace utils {

// --- половина первая: кривые ---
//
// Все принимают и возвращают долю в `[0,1]`; за пределами диапазона значения не зажимаются намеренно —
// зажимать должен тот, кто ведёт время, иначе `tween` не смог бы отличить «дошли» от «стоим на месте».
// Полиномиальные помечены `constexpr`, потому что это ничего не стоит; те, что зовут `<cmath>`, — нет:
// `std::sin` и `std::pow` в C++23 не constexpr.

template <typename T>
[[nodiscard]] constexpr T ease_linear(const T t) noexcept { return t; }

template <typename T>
[[nodiscard]] inline T ease_in_sine(const T t) noexcept {
  return T(1) - std::cos((t * T(std::numbers::pi)) / T(2));
}
template <typename T>
[[nodiscard]] inline T ease_out_sine(const T t) noexcept {
  return std::sin((t * T(std::numbers::pi)) / T(2));
}
template <typename T>
[[nodiscard]] inline T ease_in_out_sine(const T t) noexcept {
  return -(std::cos(T(std::numbers::pi) * t) - T(1)) / T(2);
}

template <typename T>
[[nodiscard]] constexpr T ease_in_quad(const T t) noexcept { return t * t; }
template <typename T>
[[nodiscard]] constexpr T ease_out_quad(const T t) noexcept { return T(1) - (T(1) - t) * (T(1) - t); }
template <typename T>
[[nodiscard]] constexpr T ease_in_out_quad(const T t) noexcept {
  return t < T(0.5) ? T(2) * t * t : T(1) - (T(-2) * t + T(2)) * (T(-2) * t + T(2)) / T(2);
}

template <typename T>
[[nodiscard]] constexpr T ease_in_cubic(const T t) noexcept { return t * t * t; }
template <typename T>
[[nodiscard]] constexpr T ease_out_cubic(const T t) noexcept {
  const T u = T(1) - t;
  return T(1) - u * u * u;
}
template <typename T>
[[nodiscard]] constexpr T ease_in_out_cubic(const T t) noexcept {
  if (t < T(0.5)) return T(4) * t * t * t;
  const T u = T(-2) * t + T(2);
  return T(1) - u * u * u / T(2);
}

template <typename T>
[[nodiscard]] constexpr T ease_in_quart(const T t) noexcept { return t * t * t * t; }
template <typename T>
[[nodiscard]] constexpr T ease_out_quart(const T t) noexcept {
  const T u = T(1) - t;
  return T(1) - u * u * u * u;
}
template <typename T>
[[nodiscard]] constexpr T ease_in_out_quart(const T t) noexcept {
  if (t < T(0.5)) return T(8) * t * t * t * t;
  const T u = T(-2) * t + T(2);
  return T(1) - u * u * u * u / T(2);
}

template <typename T>
[[nodiscard]] constexpr T ease_in_quint(const T t) noexcept { return t * t * t * t * t; }
template <typename T>
[[nodiscard]] constexpr T ease_out_quint(const T t) noexcept {
  const T u = T(1) - t;
  return T(1) - u * u * u * u * u;
}
template <typename T>
[[nodiscard]] constexpr T ease_in_out_quint(const T t) noexcept {
  if (t < T(0.5)) return T(16) * t * t * t * t * t;
  const T u = T(-2) * t + T(2);
  return T(1) - u * u * u * u * u / T(2);
}

template <typename T>
[[nodiscard]] inline T ease_in_expo(const T t) noexcept {
  return t <= T(0) ? T(0) : std::pow(T(2), T(10) * t - T(10));
}
template <typename T>
[[nodiscard]] inline T ease_out_expo(const T t) noexcept {
  return t >= T(1) ? T(1) : T(1) - std::pow(T(2), T(-10) * t);
}
template <typename T>
[[nodiscard]] inline T ease_in_out_expo(const T t) noexcept {
  if (t <= T(0)) return T(0);
  if (t >= T(1)) return T(1);
  return t < T(0.5) ? std::pow(T(2), T(20) * t - T(10)) / T(2)
                    : (T(2) - std::pow(T(2), T(-20) * t + T(10))) / T(2);
}

template <typename T>
[[nodiscard]] inline T ease_in_circ(const T t) noexcept { return T(1) - std::sqrt(T(1) - t * t); }
template <typename T>
[[nodiscard]] inline T ease_out_circ(const T t) noexcept {
  const T u = t - T(1);
  return std::sqrt(T(1) - u * u);
}
template <typename T>
[[nodiscard]] inline T ease_in_out_circ(const T t) noexcept {
  if (t < T(0.5)) return (T(1) - std::sqrt(T(1) - T(4) * t * t)) / T(2);
  const T u = T(-2) * t + T(2);
  return (std::sqrt(T(1) - u * u) + T(1)) / T(2);
}

// `back` перелетает цель и возвращается; константы взяты с easings.net без изменений, чтобы кривая
// совпадала с той, по которой её выбирали глазами.
template <typename T>
[[nodiscard]] constexpr T ease_in_back(const T t) noexcept {
  constexpr T c1 = T(1.70158);
  constexpr T c3 = c1 + T(1);
  return c3 * t * t * t - c1 * t * t;
}
template <typename T>
[[nodiscard]] constexpr T ease_out_back(const T t) noexcept {
  constexpr T c1 = T(1.70158);
  constexpr T c3 = c1 + T(1);
  const T u = t - T(1);
  return T(1) + c3 * u * u * u + c1 * u * u;
}
template <typename T>
[[nodiscard]] constexpr T ease_in_out_back(const T t) noexcept {
  constexpr T c1 = T(1.70158);
  constexpr T c2 = c1 * T(1.525);
  if (t < T(0.5)) {
    const T u = T(2) * t;
    return (u * u * ((c2 + T(1)) * u - c2)) / T(2);
  }
  const T u = T(2) * t - T(2);
  return (u * u * ((c2 + T(1)) * u + c2) + T(2)) / T(2);
}

template <typename T>
[[nodiscard]] inline T ease_in_elastic(const T t) noexcept {
  constexpr T c4 = T(2) * T(std::numbers::pi) / T(3);
  if (t <= T(0)) return T(0);
  if (t >= T(1)) return T(1);
  return -std::pow(T(2), T(10) * t - T(10)) * std::sin((t * T(10) - T(10.75)) * c4);
}
template <typename T>
[[nodiscard]] inline T ease_out_elastic(const T t) noexcept {
  constexpr T c4 = T(2) * T(std::numbers::pi) / T(3);
  if (t <= T(0)) return T(0);
  if (t >= T(1)) return T(1);
  return std::pow(T(2), T(-10) * t) * std::sin((t * T(10) - T(0.75)) * c4) + T(1);
}
template <typename T>
[[nodiscard]] inline T ease_in_out_elastic(const T t) noexcept {
  constexpr T c5 = T(2) * T(std::numbers::pi) / T(4.5);
  if (t <= T(0)) return T(0);
  if (t >= T(1)) return T(1);
  if (t < T(0.5)) return -(std::pow(T(2), T(20) * t - T(10)) * std::sin((T(20) * t - T(11.125)) * c5)) / T(2);
  return (std::pow(T(2), T(-20) * t + T(10)) * std::sin((T(20) * t - T(11.125)) * c5)) / T(2) + T(1);
}

template <typename T>
[[nodiscard]] constexpr T ease_out_bounce(T t) noexcept {
  constexpr T n1 = T(7.5625);
  constexpr T d1 = T(2.75);
  if (t < T(1) / d1) return n1 * t * t;
  if (t < T(2) / d1) {
    t -= T(1.5) / d1;
    return n1 * t * t + T(0.75);
  }
  if (t < T(2.5) / d1) {
    t -= T(2.25) / d1;
    return n1 * t * t + T(0.9375);
  }
  t -= T(2.625) / d1;
  return n1 * t * t + T(0.984375);
}
template <typename T>
[[nodiscard]] constexpr T ease_in_bounce(const T t) noexcept { return T(1) - ease_out_bounce(T(1) - t); }
template <typename T>
[[nodiscard]] constexpr T ease_in_out_bounce(const T t) noexcept {
  return t < T(0.5) ? (T(1) - ease_out_bounce(T(1) - T(2) * t)) / T(2)
                    : (T(1) + ease_out_bounce(T(2) * t - T(1))) / T(2);
}

// Кривая как ЗНАЧЕНИЕ. Нужна затем, чтобы выбор кривой можно было положить в конфиг и передать в функцию,
// а не зашивать в код вызовом конкретного имени: «плавность» — вопрос вкуса и настройки, а не логики.
enum class easing : uint8_t {
  linear,
  in_sine, out_sine, in_out_sine,
  in_quad, out_quad, in_out_quad,
  in_cubic, out_cubic, in_out_cubic,
  in_quart, out_quart, in_out_quart,
  in_quint, out_quint, in_out_quint,
  in_expo, out_expo, in_out_expo,
  in_circ, out_circ, in_out_circ,
  in_back, out_back, in_out_back,
  in_elastic, out_elastic, in_out_elastic,
  in_bounce, out_bounce, in_out_bounce,
  count
};

template <typename T>
[[nodiscard]] inline T ease(const easing kind, const T t) noexcept {
  switch (kind) {
    case easing::linear: return ease_linear(t);
    case easing::in_sine: return ease_in_sine(t);
    case easing::out_sine: return ease_out_sine(t);
    case easing::in_out_sine: return ease_in_out_sine(t);
    case easing::in_quad: return ease_in_quad(t);
    case easing::out_quad: return ease_out_quad(t);
    case easing::in_out_quad: return ease_in_out_quad(t);
    case easing::in_cubic: return ease_in_cubic(t);
    case easing::out_cubic: return ease_out_cubic(t);
    case easing::in_out_cubic: return ease_in_out_cubic(t);
    case easing::in_quart: return ease_in_quart(t);
    case easing::out_quart: return ease_out_quart(t);
    case easing::in_out_quart: return ease_in_out_quart(t);
    case easing::in_quint: return ease_in_quint(t);
    case easing::out_quint: return ease_out_quint(t);
    case easing::in_out_quint: return ease_in_out_quint(t);
    case easing::in_expo: return ease_in_expo(t);
    case easing::out_expo: return ease_out_expo(t);
    case easing::in_out_expo: return ease_in_out_expo(t);
    case easing::in_circ: return ease_in_circ(t);
    case easing::out_circ: return ease_out_circ(t);
    case easing::in_out_circ: return ease_in_out_circ(t);
    case easing::in_back: return ease_in_back(t);
    case easing::out_back: return ease_out_back(t);
    case easing::in_out_back: return ease_in_out_back(t);
    case easing::in_elastic: return ease_in_elastic(t);
    case easing::out_elastic: return ease_out_elastic(t);
    case easing::in_out_elastic: return ease_in_out_elastic(t);
    case easing::in_bounce: return ease_in_bounce(t);
    case easing::out_bounce: return ease_out_bounce(t);
    case easing::in_out_bounce: return ease_in_out_bounce(t);
    default: return t;
  }
}

[[nodiscard]] constexpr std::string_view easing_name(const easing kind) noexcept {
  switch (kind) {
    case easing::linear: return "linear";
    case easing::in_sine: return "in_sine";
    case easing::out_sine: return "out_sine";
    case easing::in_out_sine: return "in_out_sine";
    case easing::in_quad: return "in_quad";
    case easing::out_quad: return "out_quad";
    case easing::in_out_quad: return "in_out_quad";
    case easing::in_cubic: return "in_cubic";
    case easing::out_cubic: return "out_cubic";
    case easing::in_out_cubic: return "in_out_cubic";
    case easing::in_quart: return "in_quart";
    case easing::out_quart: return "out_quart";
    case easing::in_out_quart: return "in_out_quart";
    case easing::in_quint: return "in_quint";
    case easing::out_quint: return "out_quint";
    case easing::in_out_quint: return "in_out_quint";
    case easing::in_expo: return "in_expo";
    case easing::out_expo: return "out_expo";
    case easing::in_out_expo: return "in_out_expo";
    case easing::in_circ: return "in_circ";
    case easing::out_circ: return "out_circ";
    case easing::in_out_circ: return "in_out_circ";
    case easing::in_back: return "in_back";
    case easing::out_back: return "out_back";
    case easing::in_out_back: return "in_out_back";
    case easing::in_elastic: return "in_elastic";
    case easing::out_elastic: return "out_elastic";
    case easing::in_out_elastic: return "in_out_elastic";
    case easing::in_bounce: return "in_bounce";
    case easing::out_bounce: return "out_bounce";
    case easing::in_out_bounce: return "in_out_bounce";
    default: return "?";
  }
}

// Переход из A в B за известное время. Ровно тот случай, для которого кривые и существуют: щелчок колеса,
// перелёт камеры к выбранной точке, открытие панели. Перенацеливание на ходу берёт за новое начало
// ТЕКУЩЕЕ значение, а не прежнее начало, — иначе второй щелёк колеса дёргал бы картинку назад.
template <typename T>
struct tween {
  T from{};
  T to{};
  T elapsed{};
  T duration{};
  easing curve = easing::out_cubic;

  void reset(const T value) noexcept {
    from = value;
    to = value;
    elapsed = duration;
  }

  void retarget(const T current, const T target, const T seconds) noexcept {
    from = current;
    to = target;
    elapsed = T(0);
    duration = seconds;
  }

  [[nodiscard]] bool done() const noexcept { return elapsed >= duration; }

  T advance(const T dt) noexcept {
    if (duration <= T(0)) return to;
    elapsed = elapsed + dt < duration ? elapsed + dt : duration;
    return value();
  }

  [[nodiscard]] T value() const noexcept {
    if (duration <= T(0)) return to;
    const T t = elapsed / duration;
    return from + (to - from) * ease(curve, t > T(1) ? T(1) : t);
  }
};

// --- половина вторая: фильтры для удерживаемого ввода ---
//
// У зажатой клавиши нет ни цели, ни длительности, поэтому кривая к ней неприменима. Нужен фильтр,
// сглаживающий скачок скорости, и он обязан быть НЕЗАВИСИМЫМ ОТ ЧАСТОТЫ КАДРОВ: наивное
// `current += (target - current) * 0.2f` даёт разную плавность на 60 и 144 герцах, то есть разную игру
// на разных машинах.

// Экспоненциальное приближение с ПЕРИОДОМ ПОЛУРАСПАДА: за `half_life` секунд остаток расстояния до цели
// сокращается вдвое, сколько бы кадров на них ни пришлось. Половина времени — величина, которую можно
// назвать и подобрать на слух, в отличие от безымянного коэффициента.
template <typename T>
[[nodiscard]] inline T approach(const T current, const T target, const T half_life, const T dt) noexcept {
  if (half_life <= T(0)) return target;
  return target + (current - target) * std::exp2(-dt / half_life);
}

// Критически задемпфированная пружина: доводит до цели без перелёта и без рывка на старте, помня
// скорость. Это то, чем следят камерой за движущейся целью, — `approach` для этого слишком «мягок» в
// начале и слишком долго доползает в конце.
template <typename T>
inline T smooth_damp(const T current, const T target, T& velocity, const T smooth_time, const T dt,
                     const T max_speed = T(1e30)) noexcept {
  const T time = smooth_time > T(0.0001) ? smooth_time : T(0.0001);
  const T omega = T(2) / time;
  const T x = omega * dt;
  const T decay = T(1) / (T(1) + x + T(0.48) * x * x + T(0.235) * x * x * x);

  T delta = current - target;
  const T limit = max_speed * time;
  delta = delta > limit ? limit : (delta < -limit ? -limit : delta);

  const T goal = current - delta;
  const T temp = (velocity + omega * delta) * dt;
  velocity = (velocity - omega * temp) * decay;

  T result = goal + (delta + temp) * decay;

  // Перелёт гасится явно: без этого пружина у самой цели ещё несколько кадров качается вокруг неё, и
  // остановка выглядит как дрожь.
  if ((target - current > T(0)) == (result > target)) {
    result = target;
    velocity = (result - target) / dt;
  }
  return result;
}

} // namespace utils
} // namespace devils_engine

#endif
