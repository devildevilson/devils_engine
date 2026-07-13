#ifndef DEVILS_ENGINE_BINDINGS_ENV_H
#define DEVILS_ENGINE_BINDINGS_ENV_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "lua_header.h"

namespace devils_engine {
namespace bindings {
// Обёртка состояния ПСЕВДОСЛУЧАЙНЫХ чисел для lua. Смысл — ОТВЯЗАТЬ prng-состояние от обычной
// математики: скрипт не считает случайность руками, а гоняет непрозрачный rng_state через
// prng64 (следующее состояние), value (число из состояния) и '+' (смешивание двух состояний).
// Держит одно 64-бит слово; usertype без арифметики кроме сложения-микса (см. basic_functions).
struct rng_state {
  uint64_t s = 0;
};

// создадим окружение и заполним его дефолтными вещами
sol::environment create_env(sol::state_view s);
void basic_functions(sol::table t);
} // namespace bindings
} // namespace devils_engine

#endif
