#pragma once

#include <cmath>
#include <devils_script/system.h>

// Математика generation programs регистрируется одинаково у VM и обоих AST consumers.
// Pow отсутствует в базовом наборе DS; это обычная формула, не повод делать native tool мира.
namespace devils_engine::originator::detail {
inline double script_power(const double value, const double exponent) { return std::pow(value, exponent); }
inline double script_atan2(const double y, const double x) { return std::atan2(y, x); }
inline void add_generation_math(devils_script::system& system) {
  system.register_function<&script_power>("pow");
  system.register_function<&script_atan2>("atan");
}
} // namespace devils_engine::originator::detail
