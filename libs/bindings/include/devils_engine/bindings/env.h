#ifndef DEVILS_ENGINE_BINDINGS_ENV_H
#define DEVILS_ENGINE_BINDINGS_ENV_H

#include <cstddef>
#include <cstdint>
#include <string>
#include "lua_header.h"

namespace devils_engine {
namespace bindings {
// создадим окружение и заполним его дефолтными вещами
sol::environment create_env(sol::state_view s);
void basic_functions(sol::table t);
}
}

#endif