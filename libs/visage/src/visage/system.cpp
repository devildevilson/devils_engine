#include "system.h"

#include <cstring>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/time-utils.hpp"
#include "devils_engine/bindings/env.h"
#include "devils_engine/bindings/nuklear_bindings.h"
#include "header.h"
#include "font.h"

namespace devils_engine {
namespace visage {
static void* simple_alloc(nk_handle, void* /*old*/, const size_t size) {
  // КОНТРАКТ nuklear: alloc всегда выделяет НОВЫЙ блок и игнорирует old. При росте буфера
  // nk_buffer_realloc сам копирует данные из старого блока и сам его освобождает через free.
  // Если бы тут был realloc, он мог бы переместить/освободить old -> nuklear затем читает
  // освобождённую память и освобождает её повторно (double free, см. nk_buffer_realloc).
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
  utils::error{}("Called Lua hook after {} instructions. Exit lua script after {} mcs ({} seconds). Context: {}:{}:{}", system::instruction_counter, mcs, s, source, name, ar->currentline);
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

  nk_allocator a;
  memset(&a, 0, sizeof(nk_allocator));
  a.alloc = &simple_alloc;
  a.free = &simple_free;

  nk_init(ctx.get(), &a, default_font->nkfont.get());

  cmds.reset(new nk_buffer);
  nk_buffer_init(cmds.get(), &a, 4 * 1024);

  bindings::setup_nk_context(ctx.get());
  bindings::nk_functions(env);
}

system::~system() noexcept {
  bindings::setup_nk_context(nullptr);
  if (cmds) nk_buffer_free(cmds.get());
  if (ctx) nk_free(ctx.get());
}

void system::load_entry_point(const std::string &path) {
  // ВАЖНО: гоняем скрипт в песочнице env, а не в глобальном стейте — иначе скрипт
  // не увидит ни nk, ни basic_functions (они зарегистрированы именно в env).
  const auto ret = lua.script_file(path, env);
  sol_lua_check_error(ret);
  entry = ret;
}

void system::input(const input_snapshot_t& in) {
  auto c = ctx.get();
  nk_input_begin(c);

  nk_input_motion(c, int(in.mouse_x), int(in.mouse_y));
  nk_input_button(c, NK_BUTTON_LEFT, int(in.mouse_x), int(in.mouse_y), in.mouse_left);
  nk_input_button(c, NK_BUTTON_MIDDLE, int(in.mouse_x), int(in.mouse_y), in.mouse_middle);
  nk_input_button(c, NK_BUTTON_RIGHT, int(in.mouse_x), int(in.mouse_y), in.mouse_right);
  if (in.scroll_x != 0.0f || in.scroll_y != 0.0f) nk_input_scroll(c, nk_vec2(in.scroll_x, in.scroll_y));

  nk_input_key(c, NK_KEY_SHIFT, in.key_shift);
  nk_input_key(c, NK_KEY_CTRL, in.key_ctrl);
  nk_input_key(c, NK_KEY_BACKSPACE, in.key_backspace);
  nk_input_key(c, NK_KEY_DEL, in.key_delete);
  nk_input_key(c, NK_KEY_ENTER, in.key_enter);
  nk_input_key(c, NK_KEY_TAB, in.key_tab);
  nk_input_key(c, NK_KEY_LEFT, in.key_left);
  nk_input_key(c, NK_KEY_RIGHT, in.key_right);
  nk_input_key(c, NK_KEY_UP, in.key_up);
  nk_input_key(c, NK_KEY_DOWN, in.key_down);

  for (size_t i = 0; i < in.text_count; ++i) nk_input_unicode(c, nk_rune(in.text[i]));

  nk_input_end(c);
}

void system::update(const size_t time) {
  instruction_counter = 0;
  start_tp = clock_t::now();

  // защищённый вызов: ошибка в lua не должна ронять движок (panic->abort) и, что важнее,
  // не должна оставлять nk в рассинхроне (незакрытое окно -> ассерт на следующем nk_begin).
  sol::protected_function pf = entry;
  const auto r = pf(time);
  if (!r.valid()) {
    const sol::error err = r;
    utils::warn("visage: entry error: {}", err.what());
    // восстановление: закрываем окна, которые lua не успел закрыть до ошибки
    while (ctx->current != nullptr) nk_end(ctx.get());
  }
}

void system::convert() {
  auto c = ctx.get();

  // layout вершины фиксирован под gui_vertex_t (см. render_output.h)
  static const struct nk_draw_vertex_layout_element vertex_layout[] = {
    {NK_VERTEX_POSITION, NK_FORMAT_FLOAT,     offsetof(gui_vertex_t, pos)},
    {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT,     offsetof(gui_vertex_t, uv)},
    {NK_VERTEX_COLOR,    NK_FORMAT_R8G8B8A8,  offsetof(gui_vertex_t, color)},
    {NK_VERTEX_LAYOUT_END}
  };

  nk_convert_config config;
  memset(&config, 0, sizeof(config));
  config.vertex_layout = vertex_layout;
  config.vertex_size = sizeof(gui_vertex_t);
  config.vertex_alignment = alignof(gui_vertex_t);
  config.tex_null = nk_draw_null_texture{nk_handle{0}, nk_vec2(0, 0)};
  config.circle_segment_count = 22;
  config.curve_segment_count = 22;
  config.arc_segment_count = 22;
  config.global_alpha = 1.0f;
  config.shape_AA = NK_ANTI_ALIASING_OFF;
  config.line_AA = NK_ANTI_ALIASING_OFF;

  // динамические буферы nuklear (растут сами); allocator тот же, что у контекста
  nk_allocator a;
  memset(&a, 0, sizeof(a));
  a.alloc = &simple_alloc;
  a.free = &simple_free;

  nk_buffer vbuf, ebuf;
  nk_buffer_init(&vbuf, &a, 4 * 1024);
  nk_buffer_init(&ebuf, &a, 4 * 1024);

  nk_buffer_clear(cmds.get());
  const nk_flags flags = nk_convert(c, cmds.get(), &vbuf, &ebuf, &config);
  if (flags != 0) utils::error{}("nk_convert failed, flags = {}", uint32_t(flags));

  vertices_.assign(static_cast<const uint8_t*>(vbuf.memory.ptr), static_cast<const uint8_t*>(vbuf.memory.ptr) + vbuf.allocated);
  indices_.assign(static_cast<const uint8_t*>(ebuf.memory.ptr), static_cast<const uint8_t*>(ebuf.memory.ptr) + ebuf.allocated);

  // id текстуры атласа шрифта (выставлен set_texture_id). По нему отличаем текст от фигур:
  // фигуры nuklear идут с null-текстурой (id 0), текст — с этим id, остальное — image.
  const int font_tex_id = (default_font && default_font->nkfont) ? default_font->nkfont->texture.id : -1;

  commands_.clear();
  const nk_draw_command* cmd = nullptr;
  nk_draw_foreach(cmd, c, cmds.get()) {
    if (cmd->elem_count == 0) continue;
    gui_draw_command_t out;
    out.elem_count = cmd->elem_count;
    out.clip_x = cmd->clip_rect.x;
    out.clip_y = cmd->clip_rect.y;
    out.clip_w = cmd->clip_rect.w;
    out.clip_h = cmd->clip_rect.h;
    out.texture_id = uint32_t(cmd->texture.id);
    if (int(cmd->texture.id) == font_tex_id) out.mode = gui_draw_mode::msdf;
    else if (cmd->texture.id == 0)           out.mode = gui_draw_mode::solid;
    else                                     out.mode = gui_draw_mode::image;
    out.transform_id = uint32_t(cmd->userdata.id); // userptr окна -> будущая кастомная матрица
    commands_.push_back(out);
  }

  nk_buffer_free(&vbuf);
  nk_buffer_free(&ebuf);

  // сброс контекста под следующий кадр (команды/память nk переиспользуются)
  nk_clear(c);
}

nk_context* system::ctx_native() const noexcept { return ctx.get(); }
nk_buffer* system::cmds_native() const noexcept { return cmds.get(); }

size_t system::instruction_counter = 0;
system::clock_t::time_point system::start_tp = clock_t::now();
}
}