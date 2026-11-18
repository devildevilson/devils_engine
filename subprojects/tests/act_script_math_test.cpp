#include <bit>
#include <cstdint>
#include <string>

#include <devils_engine/act/call_context.h>
#include <devils_engine/act/execution_scratch.h>
#include <devils_engine/act/function.h>
#include <devils_engine/act/script_math.h>
#include <devils_engine/utils/deterministic_math.h>
#include <devils_script/system.h>
#include <doctest/doctest.h>

using namespace devils_engine;

// act::init_deterministic_math — замена init_math: тригонометрия обязана идти через
// utils::deterministic (одни биты на всех платформах), остальное — вести себя как у ds.

namespace {

struct runner {
  devils_script::system sys;
  runner() {
    sys.init_basic_functions();
    act::init_deterministic_math(sys);
  }

  double run(const std::string& source) {
    const auto program = sys.parse<double, void>("math", source);
    act::execution_scratch scratch;
    act::exec_context exec;
    exec.scratch = &scratch;
    act::script_function<act::real_t> fn(&program);
    return fn.invoke(exec);
  }
};

uint64_t bits(const double value) { return std::bit_cast<uint64_t>(value); }

} // namespace

TEST_CASE("ds trigonometry goes through utils::deterministic [act][devils_script][deterministic]") {
  namespace det = utils::deterministic;
  runner r;
  CHECK(bits(r.run("{ sin(1.5) }")) == bits(det::sin(1.5)));
  CHECK(bits(r.run("{ cos(1000000.25) }")) == bits(det::cos(1000000.25)));
  CHECK(bits(r.run("{ tan(0.75) }")) == bits(det::tan(0.75)));
  CHECK(bits(r.run("{ asin(0.3) }")) == bits(det::asin(0.3)));
  CHECK(bits(r.run("{ acos(-0.6) }")) == bits(det::acos(-0.6)));
  CHECK(bits(r.run("{ atan(7.5) }")) == bits(det::atan(7.5)));
  CHECK(bits(r.run("{ exp(-3.25) }")) == bits(det::exp(-3.25)));
}

TEST_CASE("ds operators and helpers keep the init_math semantics [act][devils_script]") {
  runner r;
  CHECK(r.run("{ 2 + 3 * 4 }") == 14.0);
  CHECK(r.run("{ 7 / 2 }") == 3.5); // `/` без целой перегрузки
  CHECK(r.run("{ 7 % 3 }") == 1.0);
  CHECK(r.run("{ -5.5 % 2 }") == -1.5);
  CHECK(r.run("{ max(2, 9) - min(4, 1) }") == 8.0);
  CHECK(r.run("{ abs(-2.5) + floor(1.75) + ceil(0.25) + round(2.5) + trunc(-1.5) }") == 6.5);
  CHECK(r.run("{ sqrt(16) + inversesqrt(4) + inv(8) }") == 4.625);
  CHECK(r.run("{ clamp(5, 0, 3) + step(1, 2) + sign(-4) + fract(2.25) }") == 3.25);
  CHECK(r.run("{ mix(2, 6, 0.25) + smoothstep(0, 1, 0.5) + fma(2, 3, 1) }") == 10.5);
  CHECK(r.run("{ to_int(7.9) }") == 7.0);
}
