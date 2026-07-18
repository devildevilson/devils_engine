#ifndef DEVILS_ENGINE_BINDINGS_NUKLEAR_H
#define DEVILS_ENGINE_BINDINGS_NUKLEAR_H

// Installs the project Nuklear API into a caller-owned Lua state.

#include <cstddef>
#include <cstdint>

#include "lua_header.h"

struct nk_context;

namespace devils_engine {
namespace bindings {
void nk_functions(sol::table t);
void setup_nk_context(nk_context* ptr);
// Снимает глобальный bridge только если им всё ещё владеет expected. При замене visage::system
// новая система успевает установить свой context до деструктора старой.
void release_nk_context(nk_context* expected);
} // namespace bindings
} // namespace devils_engine

#endif
