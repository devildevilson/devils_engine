#ifndef DEVILS_ENGINE_BINDINGS_CRITICAL_ERROR_H
#define DEVILS_ENGINE_BINDINGS_CRITICAL_ERROR_H

#include "lua_header.h"

namespace devils_engine::bindings {
// Устанавливать до копирования globals в sandbox. Обычные Lua errors остаются catchable;
// critical_error переносится через C ABI как Lua error и повторно бросается на C++ boundary.
void install_critical_error_policy(lua_State* state);
void rethrow_critical_error(lua_State* state);
} // namespace devils_engine::bindings
#endif
