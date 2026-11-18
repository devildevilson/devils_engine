// ПРОБА МАТЕМАТИКИ МЕЖДУ ПЛАТФОРМАМИ. std::sin/exp/pow и прочие не обязаны быть корректно округлены,
// и libm у glibc и CRT у MSVC вправе расходиться в последнем бите. Генераторы зовут их сотни раз, а
// мир обязан совпадать у клиента и сервера на разных системах. Пробы *_cross_compiler_probe сравнивают
// GCC и Clang НА ОДНОЙ машине, то есть с одной libm, и этого расхождения не видят.
//
// Тест сверяет дайджест битов результатов с эталоном, снятым GCC/glibc на x86-64 Linux. Входы строятся
// только точными операциями (целое * степень двойки), поэтому различаться могут лишь сами функции.
// При расхождении печатаются первые входы и выходы в hex — по ним видно, сколько ULP и где.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include <doctest/doctest.h>

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

} // namespace

TEST_CASE("transcendental functions give the same bits as the Linux reference [utils][math][platform]") {
  std::string differing;
  for (const auto& expected : reference) {
    for (const bool single : {false, true}) {
      const auto result = single ? run_named<float>(expected.name, seed_of(expected.name))
                                 : run_named<double>(expected.name, seed_of(expected.name));
      const uint64_t want = single ? expected.f32 : expected.f64;
      const char* type = single ? "float" : "double";
      // Строка для пересъёмки эталона: печатается всегда, видна с --success.
      MESSAGE(std::format("{} {} 0x{:016x}", expected.name, type, result.digest));
      if (result.digest != want) {
        differing += std::format("\n{}<{}> digest 0x{:016x} (reference 0x{:016x}); first samples:", expected.name,
                                 type, result.digest, want);
        for (const auto& line : result.head) differing += "\n    " + line;
      }
    }
  }
  INFO("functions whose bits differ from GCC/glibc x86-64:" << differing);
  CHECK(differing.empty());
}
