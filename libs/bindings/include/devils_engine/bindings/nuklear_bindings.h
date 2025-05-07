#ifndef DEVILS_ENGINE_BINDINGS_NUKLEAR_H
#define DEVILS_ENGINE_BINDINGS_NUKLEAR_H

#include <cstddef>
#include <cstdint>
#include "lua_header.h"

struct nk_context;

namespace devils_engine {
namespace bindings {
void nk_functions(sol::table t);
void setup_nk_context(nk_context* ptr);
}
}

#endif