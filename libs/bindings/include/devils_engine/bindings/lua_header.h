#ifndef DEVILS_ENGINE_BINDINGS_LUA_HEADER_H
#define DEVILS_ENGINE_BINDINGS_LUA_HEADER_H

#ifndef _NDEBUG
  #define SOL_SAFE_USERTYPE 1
  #define SOL_SAFE_REFERENCES 1
  #define SOL_SAFE_FUNCTION_CALLS 1
  #define SOL_SAFE_NUMERICS 0
  #define SOL_SAFE_GETTER 1
  #define SOL_SAFE_FUNCTION_CALLS 1
  //#define SOL_ALL_SAFETIES_ON 1
  #include <sol/sol.hpp>
#else // release
  #include <sol/sol.hpp>
#endif

#define DEVILS_ENGINE_TO_LUA_INDEX(index) ((index)+1)
#define DEVILS_ENGINE_FROM_LUA_INDEX(index) ((index)-1)

void sol_lua_check_error(sol::this_state s, const sol::function_result &res);
void sol_lua_check_error(const sol::function_result &res);
void sol_lua_warn_error(const sol::function_result &res);

#endif