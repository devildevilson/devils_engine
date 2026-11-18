// МАТЕМАТИКА МЕЖДУ ПЛАТФОРМАМИ. Два теста на одних и тех же входах.
//
// 1. КОНТРАКТ utils::deterministic (double: sin, cos, tan, asin, acos, atan, atan2, exp, pow, hypot):
//    дайджест битов обязан совпасть с эталоном на КАЖДОЙ платформе и любом компиляторе. Эталон снят
//    2026-11-18 и побитово равен libm musl 1.2.5 (Alpine 3.22), с которого сделан порт.
//
// 2. ПРОБА platform libm (std::sin и прочие): только отчёт, тест не падает. libm не обязана быть
//    корректно округлена и меняет биты между версиями: замер 2026-11-18 — glibc 2.39, 2.41–2.43 и
//    2.44 дают три разных набора (до 2 ULP), MSVC CRT расходится почти во всём. Поэтому код, от
//    которого зависит мир, зовёт utils::deterministic, а не std. Эталон пробы — GCC 16 / glibc 2.44.
//
// Входы строятся только точными операциями (целое * степень двойки), поэтому различаться могут лишь
// сами функции. Расхождения печатаются в hex: по ним видно, сколько ULP и где.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/utils/deterministic_math.h"

namespace {

constexpr size_t sample_count = 4096;

uint64_t splitmix(uint64_t& state) {
  uint64_t x = (state += 0x9e3779b97f4a7c15ull);
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// Целое k из [lo, hi), умноженное на 2^-shift: обе операции точны для |k| < 2^53.
double exact_input(uint64_t& state, const int64_t lo, const int64_t hi, const int shift) {
  const uint64_t span = uint64_t(hi - lo);
  const int64_t k = lo + int64_t(splitmix(state) % span);
  return std::ldexp(double(k), -shift);
}

struct domain {
  int64_t lo;
  int64_t hi;
  int shift;
};

struct probe_result {
  uint64_t digest = 0xcbf29ce484222325ull;
  std::vector<std::string> head;
};

void fold(probe_result& r, const uint64_t bits) {
  for (int i = 0; i < 8; ++i) {
    r.digest ^= (bits >> (i * 8)) & 0xffu;
    r.digest *= 0x100000001b3ull;
  }
}

template <typename F>
uint64_t bits_of(const F value) {
  if constexpr (sizeof(F) == 4) return std::bit_cast<uint32_t>(value);
  else return std::bit_cast<uint64_t>(value);
}

template <typename F, typename Fn>
probe_result run_unary(const uint64_t seed, const domain d, Fn&& fn) {
  probe_result r;
  uint64_t state = seed;
  for (size_t i = 0; i < sample_count; ++i) {
    const F x = F(exact_input(state, d.lo, d.hi, d.shift));
    const F y = fn(x);
    fold(r, bits_of(y));
    if (r.head.size() < 4) r.head.push_back(std::format("{:a} -> {:a}", x, y));
  }
  return r;
}

template <typename F, typename Fn>
probe_result run_binary(const uint64_t seed, const domain a, const domain b, Fn&& fn) {
  probe_result r;
  uint64_t state = seed;
  for (size_t i = 0; i < sample_count; ++i) {
    const F x = F(exact_input(state, a.lo, a.hi, a.shift));
    const F z = F(exact_input(state, b.lo, b.hi, b.shift));
    const F y = fn(x, z);
    fold(r, bits_of(y));
    if (r.head.size() < 4) r.head.push_back(std::format("({:a}, {:a}) -> {:a}", x, z, y));
  }
  return r;
}

// Области — те, где эти функции живут в генераторах, плюс большие аргументы sin/cos/tan: на них
// разные libm по-разному сводят аргумент.
constexpr domain angle{-(int64_t(1) << 20), int64_t(1) << 20, 12};    // [-256, 256)
constexpr domain big_angle{-(int64_t(1) << 24), int64_t(1) << 24, 0}; // [-1.6e7, 1.6e7)
constexpr domain unit{-(int64_t(1) << 20), int64_t(1) << 20, 20};     // [-1, 1)
constexpr domain wide{-(int64_t(1) << 22), int64_t(1) << 22, 16};     // [-64, 64)
constexpr domain exp_range{-(int64_t(40) << 16), int64_t(40) << 16, 16};
constexpr domain positive{1, int64_t(1) << 30, 10};                  // (0, 1e6]
constexpr domain base{1, int64_t(16) << 16, 16};                     // (0, 16]
constexpr domain power{-(int64_t(8) << 16), int64_t(8) << 16, 16};   // [-8, 8)
constexpr domain cube{-(int64_t(1) << 30), int64_t(1) << 30, 10};    // [-1e6, 1e6)
constexpr domain hyper{-(int64_t(20) << 16), int64_t(20) << 16, 16}; // [-20, 20)

struct expectation {
  const char* name;
  uint64_t f64;
  uint64_t f32;
};

// Эталон: GCC/glibc, x86-64 Linux. Пересняв его, назвать машину и libm в сообщении коммита.
constexpr expectation reference[] = {
#include "utils_math_platform_reference.inc"
};

template <typename F>
probe_result run_named(const std::string_view name, const uint64_t seed) {
  using std::abs;
  if (name == "sin") return run_unary<F>(seed, angle, [](F x) { return std::sin(x); });
  if (name == "cos") return run_unary<F>(seed, angle, [](F x) { return std::cos(x); });
  if (name == "tan") return run_unary<F>(seed, angle, [](F x) { return std::tan(x); });
  if (name == "sin_big") return run_unary<F>(seed, big_angle, [](F x) { return std::sin(x); });
  if (name == "cos_big") return run_unary<F>(seed, big_angle, [](F x) { return std::cos(x); });
  if (name == "asin") return run_unary<F>(seed, unit, [](F x) { return std::asin(x); });
  if (name == "acos") return run_unary<F>(seed, unit, [](F x) { return std::acos(x); });
  if (name == "atan") return run_unary<F>(seed, wide, [](F x) { return std::atan(x); });
  if (name == "atan2") return run_binary<F>(seed, wide, wide, [](F y, F x) { return std::atan2(y, x); });
  if (name == "exp") return run_unary<F>(seed, exp_range, [](F x) { return std::exp(x); });
  if (name == "exp2") return run_unary<F>(seed, exp_range, [](F x) { return std::exp2(x); });
  if (name == "log") return run_unary<F>(seed, positive, [](F x) { return std::log(x); });
  if (name == "log2") return run_unary<F>(seed, positive, [](F x) { return std::log2(x); });
  if (name == "log10") return run_unary<F>(seed, positive, [](F x) { return std::log10(x); });
  if (name == "pow") return run_binary<F>(seed, base, power, [](F x, F y) { return std::pow(x, y); });
  if (name == "cbrt") return run_unary<F>(seed, cube, [](F x) { return std::cbrt(x); });
  if (name == "hypot") return run_binary<F>(seed, wide, wide, [](F x, F y) { return std::hypot(x, y); });
  if (name == "sinh") return run_unary<F>(seed, hyper, [](F x) { return std::sinh(x); });
  if (name == "cosh") return run_unary<F>(seed, hyper, [](F x) { return std::cosh(x); });
  if (name == "tanh") return run_unary<F>(seed, hyper, [](F x) { return std::tanh(x); });
  if (name == "sqrt") return run_unary<F>(seed, positive, [](F x) { return std::sqrt(x); });
  FAIL("unknown function ", name);
  return {};
}

uint64_t seed_of(const std::string_view name) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const char c : name) {
    h ^= uint8_t(c);
    h *= 0x100000001b3ull;
  }
  return h;
}

// Те же имена и области, что у пробы libm; float-перегрузок порт не даёт (они у Jolt, с другим
// диапазоном), поэтому только double.
probe_result run_deterministic(const std::string_view name, const uint64_t seed) {
  namespace det = devils_engine::utils::deterministic;
  using F = double;
  if (name == "sin") return run_unary<F>(seed, angle, [](F x) { return det::sin(x); });
  if (name == "cos") return run_unary<F>(seed, angle, [](F x) { return det::cos(x); });
  if (name == "tan") return run_unary<F>(seed, angle, [](F x) { return det::tan(x); });
  if (name == "sin_big") return run_unary<F>(seed, big_angle, [](F x) { return det::sin(x); });
  if (name == "cos_big") return run_unary<F>(seed, big_angle, [](F x) { return det::cos(x); });
  if (name == "asin") return run_unary<F>(seed, unit, [](F x) { return det::asin(x); });
  if (name == "acos") return run_unary<F>(seed, unit, [](F x) { return det::acos(x); });
  if (name == "atan") return run_unary<F>(seed, wide, [](F x) { return det::atan(x); });
  if (name == "atan2") return run_binary<F>(seed, wide, wide, [](F y, F x) { return det::atan2(y, x); });
  if (name == "exp") return run_unary<F>(seed, exp_range, [](F x) { return det::exp(x); });
  if (name == "pow") return run_binary<F>(seed, base, power, [](F x, F y) { return det::pow(x, y); });
  if (name == "hypot") return run_binary<F>(seed, wide, wide, [](F x, F y) { return det::hypot(x, y); });
  FAIL("unknown function ", name);
  return {};
}

struct deterministic_expectation {
  const char* name;
  uint64_t digest;
};

// Эталон порта: снят 2026-11-18, равен libm musl 1.2.5 на тех же входах. Пересъёмка = смена
// численного контракта: назвать причину в сообщении коммита.
constexpr deterministic_expectation deterministic_reference[] = {
#include "utils_deterministic_math_reference.inc"
};

uint64_t bits(const double value) { return std::bit_cast<uint64_t>(value); }

} // namespace

TEST_CASE("deterministic math gives the same bits on every platform [utils][math][deterministic]") {
  std::string names; // сначала короткий список: он гарантированно влезает в отчёт CI
  std::string differing;
  for (const auto& expected : deterministic_reference) {
    const auto result = run_deterministic(expected.name, seed_of(expected.name));
    MESSAGE(std::format("{} 0x{:016x}", expected.name, result.digest)); // строка для пересъёмки, видна с -s
    if (result.digest != expected.digest) {
      names += std::format(" {}", expected.name);
      differing += std::format("\n{} digest 0x{:016x} (reference 0x{:016x}); first samples:", expected.name,
                               result.digest, expected.digest);
      for (const auto& line : result.head) differing += "\n    " + line;
    }
  }
  INFO("deterministic functions whose bits differ from the reference:" << names << "\ndetails:" << differing);
  CHECK(differing.empty());
}

TEST_CASE("deterministic math keeps signed zero, infinities and one NaN [utils][math][deterministic]") {
  namespace det = devils_engine::utils::deterministic;
  constexpr double inf = std::numeric_limits<double>::infinity();
  const double nan = std::bit_cast<double>(det::canonical_nan_bits);
  const double negative_nan = std::bit_cast<double>(UINT64_C(0xfff8000000000001)); // знак и payload

  for (const double zero : {0.0, -0.0}) {
    CHECK(bits(det::sin(zero)) == bits(zero));
    CHECK(bits(det::tan(zero)) == bits(zero));
    CHECK(bits(det::asin(zero)) == bits(zero));
    CHECK(bits(det::atan(zero)) == bits(zero));
    CHECK(det::cos(zero) == 1.0);
    CHECK(det::exp(zero) == 1.0);
  }
  CHECK(bits(det::atan2(-0.0, 1.0)) == bits(-0.0));
  CHECK(det::atan2(0.0, -1.0) == std::numbers::pi);

  // Любой NaN на выходе — один и тот же: x86 и ARM рождают NaN с разным знаком.
  for (const double x : {inf, -inf, nan, negative_nan}) {
    CHECK(bits(det::sin(x)) == det::canonical_nan_bits);
    CHECK(bits(det::cos(x)) == det::canonical_nan_bits);
    CHECK(bits(det::tan(x)) == det::canonical_nan_bits);
  }
  CHECK(bits(det::asin(2.0)) == det::canonical_nan_bits);
  CHECK(bits(det::acos(-2.0)) == det::canonical_nan_bits);
  CHECK(bits(det::pow(-8.0, 1.0 / 3.0)) == det::canonical_nan_bits);
  CHECK(bits(det::exp(negative_nan)) == det::canonical_nan_bits);
  CHECK(bits(det::atan2(negative_nan, 1.0)) == det::canonical_nan_bits);

  CHECK(det::exp(inf) == inf);
  CHECK(bits(det::exp(-inf)) == bits(0.0));
  CHECK(det::exp(1000.0) == inf);
  CHECK(bits(det::exp(-1000.0)) == bits(0.0));
  CHECK(det::pow(0.0, -1.0) == inf);
  CHECK(det::pow(-2.0, 3.0) == -8.0);
  CHECK(det::pow(nan, 0.0) == 1.0);
  CHECK(det::hypot(inf, nan) == inf);
  CHECK(det::hypot(3.0, 4.0) == 5.0);
  CHECK(det::atan(inf) == std::numbers::pi / 2);
}

TEST_CASE("deterministic math stays within a few ULP of the platform libm [utils][math][deterministic]") {
  // Ловит порчу порта, а не точность libm: musl < 1 ULP от истины, libm тоже около того.
  namespace det = devils_engine::utils::deterministic;
  constexpr uint64_t tolerance = 4;
  const auto ulp_distance = [](const double a, const double b) {
    const auto ordered = [](const double v) {
      const auto i = std::bit_cast<int64_t>(v);
      return i >= 0 ? i : std::numeric_limits<int64_t>::min() - i;
    };
    return uint64_t(std::abs(ordered(a) - ordered(b)));
  };
  uint64_t state = 20261118;
  uint64_t worst = 0;
  for (size_t i = 0; i < 4096; ++i) {
    const double x = exact_input(state, angle.lo, angle.hi, angle.shift);
    const double u = exact_input(state, unit.lo, unit.hi, unit.shift);
    const double e = exact_input(state, exp_range.lo, exp_range.hi, exp_range.shift);
    const double b = exact_input(state, base.lo, base.hi, base.shift);
    const double pairs[][2] = {
      {det::sin(x), std::sin(x)},           {det::cos(x), std::cos(x)},
      {det::tan(x), std::tan(x)},           {det::asin(u), std::asin(u)},
      {det::acos(u), std::acos(u)},         {det::atan(x), std::atan(x)},
      {det::atan2(x, u), std::atan2(x, u)}, {det::exp(e), std::exp(e)},
      {det::pow(b, u * 8), std::pow(b, u * 8)}, {det::hypot(x, e), std::hypot(x, e)},
    };
    for (const auto& pair : pairs) worst = std::max(worst, ulp_distance(pair[0], pair[1]));
  }
  MESSAGE("worst distance from platform libm: " << worst << " ULP");
  CHECK(worst <= tolerance);
}

TEST_CASE("platform libm against the GCC/glibc 2.44 reference, report only [utils][math][platform]") {
  std::string names; // сначала короткий список: он гарантированно влезает в отчёт CI
  std::string differing;
  for (const auto& expected : reference) {
    for (const bool single : {false, true}) {
      const auto result = single ? run_named<float>(expected.name, seed_of(expected.name))
                                 : run_named<double>(expected.name, seed_of(expected.name));
      const uint64_t want = single ? expected.f32 : expected.f64;
      const char* type = single ? "float" : "double";
      if (result.digest != want) {
        names += std::format(" {}<{}>", expected.name, type);
        differing += std::format("\n{}<{}> digest 0x{:016x} (reference 0x{:016x}); first samples:", expected.name,
                                 type, result.digest, want);
        for (const auto& line : result.head) differing += "\n    " + line;
      }
    }
  }
  // Не проверка: libm такого контракта не даёт. Видно с `-s` (doctest) или --verbose (ctest).
  MESSAGE("platform libm functions whose bits differ from GCC 16 / glibc 2.44:"
          << (names.empty() ? std::string(" none") : names) << differing);
}
