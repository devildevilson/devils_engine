#include "devils_engine/act/script_math.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>

#include <devils_script/common.h>
#include <devils_script/prng.h>
#include <devils_script/system.h>

#include "devils_engine/utils/deterministic_math.h"

// Тела функций — копия devils_script v1.4.1 src/system_builtins.cpp (internal::raw*), кроме
// трансцендентных: они идут через utils::deterministic. Порядок регистрации тот же, что у init_math:
// от него зависит выбор перегрузки (целые раньше вещественных).

namespace devils_engine::act {
namespace {
namespace det = utils::deterministic;
using devils_script::script_float_t;
using devils_script::script_int_t;
using script_uint_t = std::make_unsigned_t<script_int_t>;

script_int_t rawaddi(const script_int_t a, const script_int_t b) noexcept { return script_int_t(script_uint_t(a) + script_uint_t(b)); }
script_int_t rawmuli(const script_int_t a, const script_int_t b) noexcept { return script_int_t(script_uint_t(a) * script_uint_t(b)); }
script_int_t rawsubi(const script_int_t a, const script_int_t b) noexcept { return script_int_t(script_uint_t(a) - script_uint_t(b)); }
script_int_t rawposi(const script_int_t a) noexcept { return +a; }
script_int_t rawnegi(const script_int_t a) noexcept { return script_int_t(0u - script_uint_t(a)); }
script_int_t rawmodi(const script_int_t a, const script_int_t b) {
  if (b == 0) throw std::runtime_error("Integer remainder by zero");
  if (b == -1) return 0;
  return a % b;
}
bool rawmorei(const script_int_t a, const script_int_t b) noexcept { return a > b; }
bool rawlessi(const script_int_t a, const script_int_t b) noexcept { return a < b; }
bool rawmoreeqi(const script_int_t a, const script_int_t b) noexcept { return a >= b; }
bool rawlesseqi(const script_int_t a, const script_int_t b) noexcept { return a <= b; }
script_float_t rawadd(const script_float_t a, const script_float_t b) noexcept { return a + b; }
script_float_t rawmul(const script_float_t a, const script_float_t b) noexcept { return a * b; }
script_float_t rawsub(const script_float_t a, const script_float_t b) noexcept { return a - b; }
script_float_t rawdiv(const script_float_t a, const script_float_t b) noexcept { return a / b; }
script_float_t rawmod(const script_float_t a, const script_float_t b) noexcept { return std::fmod(a, b); } // точная операция
script_float_t rawpos(const script_float_t a) noexcept { return +a; }
script_float_t rawneg(const script_float_t a) noexcept { return -a; }
bool rawnot(const bool a) noexcept { return !a; }
bool rawmore(const script_float_t a, const script_float_t b) noexcept { return a > b; }
bool rawless(const script_float_t a, const script_float_t b) noexcept { return a < b; }
bool rawmoreeq(const script_float_t a, const script_float_t b) noexcept { return a >= b; }
bool rawlesseq(const script_float_t a, const script_float_t b) noexcept { return a <= b; }

script_int_t rawmaxi(const script_int_t a, const script_int_t b) noexcept { return std::max(a, b); }
script_int_t rawmini(const script_int_t a, const script_int_t b) noexcept { return std::min(a, b); }
script_int_t rawabsi(const script_int_t a) noexcept { return a < 0 ? script_int_t(0u - script_uint_t(a)) : a; }
script_float_t rawmax(const script_float_t a, const script_float_t b) noexcept {
  return std::isnan(a) || std::isnan(b) ? std::numeric_limits<script_float_t>::quiet_NaN() : std::max(a, b);
}
script_float_t rawmin(const script_float_t a, const script_float_t b) noexcept {
  return std::isnan(a) || std::isnan(b) ? std::numeric_limits<script_float_t>::quiet_NaN() : std::min(a, b);
}
script_float_t rawabs(const script_float_t a) noexcept { return std::abs(a); }
script_float_t rawceil(const script_float_t a) noexcept { return std::ceil(a); }
script_float_t rawfloor(const script_float_t a) noexcept { return std::floor(a); }
script_float_t rawround(const script_float_t a) noexcept { return std::round(a); }
script_float_t rawtrunc(const script_float_t a) noexcept { return std::trunc(a); }
script_float_t rawsqrt(const script_float_t a) noexcept { return std::sqrt(a); } // IEEE: корректно округлён везде
script_float_t rawinversesqrt(const script_float_t a) noexcept { return 1.0 / std::sqrt(a); }
script_float_t rawexp(const script_float_t a) noexcept { return det::exp(a); }
script_float_t rawsin(const script_float_t a) noexcept { return det::sin(a); }
script_float_t rawcos(const script_float_t a) noexcept { return det::cos(a); }
script_float_t rawasin(const script_float_t a) noexcept { return det::asin(a); }
script_float_t rawacos(const script_float_t a) noexcept { return det::acos(a); }
script_float_t rawtan(const script_float_t a) noexcept { return det::tan(a); }
script_float_t rawatan(const script_float_t a) noexcept { return det::atan(a); }
script_float_t rawinc(const script_float_t a) noexcept { return a + 1.0; }
script_float_t rawdec(const script_float_t a) noexcept { return a - 1.0; }
script_float_t rawinv(const script_float_t a) noexcept { return 1.0 / a; }
script_int_t rawtoint(const script_float_t a) {
  if (!(a >= -9223372036854775808.0 && a < 9223372036854775808.0))
    throw std::runtime_error(std::format("'to_int' cannot represent {} as an integer", a));
  return script_int_t(a);
}

bool operator_or(const bool a, const bool b) noexcept { return a || b; }
bool operator_and(const bool a, const bool b) noexcept { return a && b; }

script_float_t rawsign(const script_float_t v) noexcept { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }
script_float_t rawfma(const script_float_t a, const script_float_t b, const script_float_t c) noexcept { return a * b + c; }
script_float_t rawfract(const script_float_t a) noexcept { return a - rawfloor(a); }
script_float_t rawmix(const script_float_t a, const script_float_t b, const script_float_t t) noexcept { return a * (1.0 - t) + b * t; }
script_float_t rawsclamp(const script_float_t t, const script_float_t lo, const script_float_t hi) {
  if (hi < lo) throw std::runtime_error(std::format("'clamp' lower bound {} is greater than upper bound {}", lo, hi));
  return std::clamp(t, lo, hi);
}
script_float_t rawsmoothstep(const script_float_t e0, const script_float_t e1, const script_float_t x) noexcept {
  if (x <= e0) return 0.0;
  if (x >= e1) return 1.0;
  const script_float_t t = rawsclamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
script_float_t rawstep(const script_float_t edge, const script_float_t x) noexcept { return x < edge ? 0.0 : 1.0; }
script_float_t rawrndmix1(const script_float_t a) noexcept {
  return devils_script::prng::prng_normalize(devils_script::prng::mix(uint64_t(devils_script::pack_float(a))));
}
script_float_t rawrndmix(const script_float_t a, const script_float_t b) noexcept {
  return devils_script::prng::prng_normalize(
      devils_script::prng::mix(uint64_t(devils_script::pack_float(a)), uint64_t(devils_script::pack_float(b))));
}
} // namespace

void init_deterministic_math(devils_script::system& sys) {
  using cd = devils_script::system::command_data;
  constexpr devils_script::system::operator_props prefix{14, cd::math_ftype::prefix, cd::associativity::right};
  constexpr devils_script::system::operator_props product{12, cd::math_ftype::binary, cd::associativity::left};
  constexpr devils_script::system::operator_props sum{11, cd::math_ftype::binary, cd::associativity::left};
  constexpr devils_script::system::operator_props compare{8, cd::math_ftype::binary, cd::associativity::left};

  sys.register_operator<&rawposi>("unary_plus", prefix);
  sys.register_operator<&rawnegi>("unary_minus", prefix);
  sys.register_operator<&rawpos>("unary_plus", prefix);
  sys.register_operator<&rawneg>("unary_minus", prefix);
  sys.register_operator<&rawnot>("not", prefix);
  sys.register_operator<&rawinc>("++", prefix);
  sys.register_operator<&rawdec>("--", prefix);
  sys.register_operator<&rawmuli>("*", product);
  sys.register_operator<&rawmul>("*", product);
  // `/` без целой перегрузки, как у ds: деление целых почти никогда не то, что имели в виду.
  sys.register_operator<&rawdiv>("/", product);
  sys.register_operator<&rawmodi>("%", product);
  sys.register_operator<&rawmod>("%", product);
  sys.register_operator<&rawaddi>("+", sum);
  sys.register_operator<&rawsubi>("-", sum);
  sys.register_operator<&rawadd>("+", sum);
  sys.register_operator<&rawsub>("-", sum);
  sys.register_operator<&rawmoreeqi>(">=", compare);
  sys.register_operator<&rawlesseqi>("<=", compare);
  sys.register_operator<&rawmorei>(">", compare);
  sys.register_operator<&rawlessi>("<", compare);
  sys.register_operator<&rawmoreeq>(">=", compare);
  sys.register_operator<&rawlesseq>("<=", compare);
  sys.register_operator<&rawmore>(">", compare);
  sys.register_operator<&rawless>("<", compare);
  sys.register_operator<&operator_and>("and", {3, cd::math_ftype::binary, cd::associativity::left});
  sys.register_operator<&operator_or>("or", {2, cd::math_ftype::binary, cd::associativity::left});

  // Целые раньше вещественных, как у ds: целый max не должен проходить через double.
  sys.register_function<&rawmaxi>("max");
  sys.register_function<&rawmini>("min");
  sys.register_function<&rawabsi>("abs");
  sys.register_function<&rawmax>("max");
  sys.register_function<&rawmin>("min");
  sys.register_function<&rawabs>("abs");
  sys.register_function<&rawceil>("ceil");
  sys.register_function<&rawfloor>("floor");
  sys.register_function<&rawround>("round");
  sys.register_function<&rawtrunc>("trunc");
  sys.register_function<&rawexp>("exp");
  sys.register_function<&rawsqrt>("sqrt");
  sys.register_function<&rawinversesqrt>("inversesqrt");
  sys.register_function<&rawsin>("sin");
  sys.register_function<&rawcos>("cos");
  sys.register_function<&rawasin>("asin");
  sys.register_function<&rawacos>("acos");
  sys.register_function<&rawtan>("tan");
  sys.register_function<&rawatan>("atan");
  sys.register_function<&rawinv>("inv");
  sys.register_function<&rawtoint>("to_int");

  sys.register_function<&rawsign>("sign");
  sys.register_function<&rawfma>("fma");
  sys.register_function<&rawfract>("fract");
  sys.register_function<&rawmix>("mix");
  sys.register_function<&rawsclamp>("clamp");
  sys.register_function<&rawsmoothstep>("smoothstep");
  sys.register_function<&rawstep>("step");

  sys.register_function<&rawrndmix1>("rndmix1");
  sys.register_function<&rawrndmix>("rndmix");
}
} // namespace devils_engine::act
