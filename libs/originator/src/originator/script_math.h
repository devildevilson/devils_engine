#pragma once

#include <devils_script/system.h>

#include "devils_engine/utils/deterministic_math.h"

// Математика generation programs регистрируется одинаково у VM и обоих AST consumers.
// Pow отсутствует в базовом наборе DS; это обычная формула, не повод делать native tool мира.
// Трансцендентные функции — utils::deterministic, не std: libm меняет последние биты между
// версиями glibc и у MSVC своя, а мир обязан совпасть у клиента и сервера на любых системах.
namespace devils_engine::originator::detail {
namespace det = utils::deterministic;
inline double script_power(const double value, const double exponent) { return det::pow(value, exponent); }
inline double script_atan2(const double y, const double x) { return det::atan2(y, x); }
inline void add_generation_math(devils_script::system& system) {
  system.register_function<&script_power>("pow");
  system.register_function<&script_atan2>("atan");
}
} // namespace devils_engine::originator::detail
