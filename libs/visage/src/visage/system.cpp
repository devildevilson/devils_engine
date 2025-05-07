#include "system.h"

#include <cstring>

#include "utils/core.h"
#include "utils/time-utils.hpp"
#include "bindings/env.h"
#include "bindings/nuklear_bindings.h"
#include "header.h"
#include "font.h"

namespace devils_engine {
namespace visage {
static void* simple_alloc(nk_handle, void* old, const size_t size) {
  if (old != nullptr) { return realloc(old, size); }
  return malloc(size);
}

static void simple_free(nk_handle, void* old) {
  free(old);
}

static void simple_hook(lua_State *L, lua_Debug *ar) {
  system::instruction_counter += system::hook_after_instructions_count;
  auto cur_tp = std::chrono::steady_clock::now();
  const auto mcs = utils::count_mcs(system::start_tp, cur_tp);
  if (mcs < system::seconds10) return;
  const double s = double(std::abs(mcs)) / double(utils::app_clock::resolution());

  lua_getinfo(L, "nSl", ar);
  std::string_view source(ar->source, ar->srclen);
  std::string_view name(ar->name ? ar->name : "<unknown>");
  utils::error("Called Lua hook after {} instructions. Exit lua script after {} mcs ({} seconds). Context: {}:{}:{}", system::instruction_counter, mcs, s, source, name, ar->currentline);
}

system::system(const font_t* default_font) : default_font(default_font) {
  lua.open_libraries(
    //sol::lib::debug,
    sol::lib::base, 
    sol::lib::bit32, 
    sol::lib::coroutine, 
    sol::lib::math, 
    //sol::lib::package, 
    sol::lib::string, 
    sol::lib::table, 
    sol::lib::utf8,
    sol::lib::os
  );

  lua_sethook(lua.lua_state(), &simple_hook, LUA_MASKCOUNT, hook_after_instructions_count);

  env = bindings::create_env(lua);
  bindings::basic_functions(env);
  
  ctx.reset(new nk_context);
  //cmds.reset(new nk_buffer);

  nk_allocator a;
  memset(&a, 0, sizeof(nk_allocator));
  a.alloc = &simple_alloc; 
  a.free = &simple_free;
  
  nk_init(ctx.get(), &a, default_font->nkfont.get());
  cmds = &ctx->memory;

  bindings::setup_nk_context(ctx.get());
  bindings::nk_functions(env);
}

system::~system() noexcept {
  bindings::setup_nk_context(nullptr);
}

void system::load_entry_point(const std::string &path) {
  const auto ret = lua.script_file(path);
  sol_lua_check_error(ret);
  entry = ret;
}

void system::update(const size_t time) {
  instruction_counter = 0;
  start_tp = clock_t::now();
  entry(time);
}

nk_context* system::ctx_native() const noexcept { return ctx.get(); }
nk_buffer* system::cmds_native() const noexcept { return cmds; }

size_t system::instruction_counter = 0;
system::clock_t::time_point system::start_tp = clock_t::now();
}
}