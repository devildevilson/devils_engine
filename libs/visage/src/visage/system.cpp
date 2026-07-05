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

// SDF-эффекты текста, кладутся в effect_arena (CPU-скретч). nk userdata команды = offset сюда.
// convert запекает эти поля в gui_draw_command_t -> push-константа -> ui.frag. POD, 16 байт.
struct ui_text_effect {
  float boldness;
  float outline_width;
  uint32_t outline_color; // R8G8B8A8
  float softness;
};

// lua-таблица {r,g,b,a} в 0..1 -> упакованный R8G8B8A8
static uint32_t pack_color01(const sol::table& t) {
  const auto b = [&t](const int i) {
    double v = t.get_or(i, 0.0);
    v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return uint32_t(v * 255.0 + 0.5);
  };
  return b(1) | (b(2) << 8) | (b(3) << 16) | (b(4) << 24);
}

// lua-таблица {r,g,b,a} в 0..1 -> nk_color (байты). a по умолчанию 1.0 (непрозрачный).
static nk_color table_to_nk_color(const sol::table& t) {
  const auto b = [&t](const int i, const double def) {
    double v = t.get_or(i, def);
    v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    return nk_byte(v * 255.0 + 0.5);
  };
  nk_color c; c.r = b(1, 0.0); c.g = b(2, 0.0); c.b = b(3, 0.0); c.a = b(4, 1.0);
  return c;
}

// Дефолтная тема Nuklear (значения из NK_COLOR_MAP в nuklear.h — макрос виден только в TU
// с NK_IMPLEMENTATION, поэтому дублируем значения по enum-индексу; порядок не важен). Заполняем
// весь массив размера NK_COLOR_COUNT, чтобы nk_style_from_table получил валидную таблицу.
static void seed_default_theme(nk_color* t) {
  auto set = [t](int i, nk_byte r, nk_byte g, nk_byte b, nk_byte a) { t[i] = nk_color{r, g, b, a}; };
  set(NK_COLOR_TEXT, 175,175,175,255); set(NK_COLOR_WINDOW, 45,45,45,255);
  set(NK_COLOR_HEADER, 40,40,40,255);  set(NK_COLOR_BORDER, 65,65,65,255);
  set(NK_COLOR_BUTTON, 50,50,50,255);  set(NK_COLOR_BUTTON_HOVER, 40,40,40,255);
  set(NK_COLOR_BUTTON_ACTIVE, 35,35,35,255); set(NK_COLOR_TOGGLE, 100,100,100,255);
  set(NK_COLOR_TOGGLE_HOVER, 120,120,120,255); set(NK_COLOR_TOGGLE_CURSOR, 45,45,45,255);
  set(NK_COLOR_SELECT, 45,45,45,255);  set(NK_COLOR_SELECT_ACTIVE, 35,35,35,255);
  set(NK_COLOR_SLIDER, 38,38,38,255);  set(NK_COLOR_SLIDER_CURSOR, 100,100,100,255);
  set(NK_COLOR_SLIDER_CURSOR_HOVER, 120,120,120,255); set(NK_COLOR_SLIDER_CURSOR_ACTIVE, 150,150,150,255);
  set(NK_COLOR_PROPERTY, 38,38,38,255); set(NK_COLOR_EDIT, 38,38,38,255);
  set(NK_COLOR_EDIT_CURSOR, 175,175,175,255); set(NK_COLOR_COMBO, 45,45,45,255);
  set(NK_COLOR_CHART, 120,120,120,255); set(NK_COLOR_CHART_COLOR, 45,45,45,255);
  set(NK_COLOR_CHART_COLOR_HIGHLIGHT, 255,0,0,255); set(NK_COLOR_SCROLLBAR, 40,40,40,255);
  set(NK_COLOR_SCROLLBAR_CURSOR, 100,100,100,255); set(NK_COLOR_SCROLLBAR_CURSOR_HOVER, 120,120,120,255);
  set(NK_COLOR_SCROLLBAR_CURSOR_ACTIVE, 150,150,150,255); set(NK_COLOR_TAB_HEADER, 40,40,40,255);
#ifdef NK_COLOR_KNOB
  set(NK_COLOR_KNOB, 38,38,38,255); set(NK_COLOR_KNOB_CURSOR, 100,100,100,255);
  set(NK_COLOR_KNOB_CURSOR_HOVER, 120,120,120,255); set(NK_COLOR_KNOB_CURSOR_ACTIVE, 150,150,150,255);
#endif
}

// Имя элемента стиля Nuklear -> индекс NK_COLOR_* (enum-константы, устойчиво к порядку). -1 = неизвестно.
static int nk_color_index_from_name(std::string_view n) {
  struct entry { std::string_view name; int idx; };
  static const entry table[] = {
    {"text", NK_COLOR_TEXT}, {"window", NK_COLOR_WINDOW}, {"header", NK_COLOR_HEADER},
    {"border", NK_COLOR_BORDER}, {"button", NK_COLOR_BUTTON}, {"button_hover", NK_COLOR_BUTTON_HOVER},
    {"button_active", NK_COLOR_BUTTON_ACTIVE}, {"toggle", NK_COLOR_TOGGLE},
    {"toggle_hover", NK_COLOR_TOGGLE_HOVER}, {"toggle_cursor", NK_COLOR_TOGGLE_CURSOR},
    {"select", NK_COLOR_SELECT}, {"select_active", NK_COLOR_SELECT_ACTIVE},
    {"slider", NK_COLOR_SLIDER}, {"slider_cursor", NK_COLOR_SLIDER_CURSOR},
    {"slider_cursor_hover", NK_COLOR_SLIDER_CURSOR_HOVER}, {"slider_cursor_active", NK_COLOR_SLIDER_CURSOR_ACTIVE},
    {"property", NK_COLOR_PROPERTY}, {"edit", NK_COLOR_EDIT}, {"edit_cursor", NK_COLOR_EDIT_CURSOR},
    {"combo", NK_COLOR_COMBO}, {"chart", NK_COLOR_CHART}, {"chart_color", NK_COLOR_CHART_COLOR},
    {"chart_color_highlight", NK_COLOR_CHART_COLOR_HIGHLIGHT}, {"scrollbar", NK_COLOR_SCROLLBAR},
    {"scrollbar_cursor", NK_COLOR_SCROLLBAR_CURSOR}, {"scrollbar_cursor_hover", NK_COLOR_SCROLLBAR_CURSOR_HOVER},
    {"scrollbar_cursor_active", NK_COLOR_SCROLLBAR_CURSOR_ACTIVE}, {"tab_header", NK_COLOR_TAB_HEADER},
  };
  for (const auto& e : table) if (e.name == n) return e.idx;
  return -1;
}

static void simple_hook(lua_State *L, lua_Debug *ar) {
  system::instruction_counter += system::hook_after_instructions_count;
  auto cur_tp = std::chrono::steady_clock::now();
  const auto mcs = utils::count_mcs(system::start_tp, cur_tp);
  if (mcs < int64_t(system::seconds10)) return;
  const double s = double(std::abs(mcs)) / double(utils::app_clock::resolution());

  lua_getinfo(L, "nSl", ar);
  std::string_view source(ar->source, ar->srclen);
  std::string_view name(ar->name ? ar->name : "<unknown>");
  utils::error{}("Called Lua hook after {} instructions. Exit lua script after {} mcs ({} seconds). Context: {}:{}:{}", system::instruction_counter, mcs, s, source, name, ar->currentline);
}

system::system(const font_t* default_font) : default_font(default_font), effect_arena(64 * 1024, 16) {
  fonts_.emplace_back("default", default_font); // базовый шрифт под именем "default" (шаг 2b)
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

  // Динамический размер шрифта. Регистрируем тут (а не в bindings): нужен доступ к font_t, а
  // bindings не может зависеть от visage (циклическая зависимость). Мапится на встроенный
  // стек шрифтов nuklear; один атлас годится для всех размеров (MSDF масштабируется в шейдере).
  // lua обязан балансировать push/pop в пределах кадра.
  // push_font(arg): arg = число (только размер) или таблица {size=, bold=, softness=, outline={color={r,g,b,a}, width=}}.
  // Меняет и размер (nk_style_push_font), и SDF-эффекты (через userdata = offset в effect_arena).
  sol::table nk_tbl = env["nk"];
  nk_tbl.set_function("push_font", [this](const sol::object arg) {
    const font_t* base = this->default_font;
    float size = float(base->nkfont->height);
    ui_text_effect eff{0.0f, 0.0f, 0u, 0.0f};

    if (arg.is<double>()) {
      size = float(arg.as<double>());
    } else if (arg.is<sol::table>()) {
      const sol::table t = arg.as<sol::table>();
      // выбор базового шрифта по имени (шаг 2b). Неизвестное имя ⇒ default (resolve_font).
      const sol::optional<std::string> font_name = t["font"];
      if (font_name.has_value()) base = this->resolve_font(font_name.value());
      size = float(t.get_or("size", double(base->nkfont->height)));
      eff.boldness = float(t.get_or("bold", 0.0));
      eff.softness = float(t.get_or("softness", 0.0));
      const sol::optional<sol::table> outline = t["outline"];
      if (outline.has_value()) {
        const sol::table o = outline.value();
        eff.outline_width = float(o.get_or("width", 0.0));
        const sol::optional<sol::table> col = o["color"];
        if (col.has_value()) eff.outline_color = pack_color01(col.value());
      }
    }

    // эффект в арену, offset -> userdata (его подхватят последующие text-команды)
    auto* e = effect_arena.create<ui_text_effect>(eff);
    nk_handle h; h.id = int(effect_arena.offset_of(e));
    userdata_stack.push_back(ctx->userdata.id); // для восстановления на pop
    nk_set_user_data(ctx.get(), h);
    nk_style_push_font(ctx.get(), this->sized_font(base, size));
  });
  nk_tbl.set_function("pop_font", [this]() {
    nk_style_pop_font(ctx.get());
    if (!userdata_stack.empty()) {
      nk_handle h; h.id = userdata_stack.back();
      userdata_stack.pop_back();
      nk_set_user_data(ctx.get(), h);
    }
  });

  // Стилизация Nuklear (шаг 2c). Скрипт грузит тему из lua. style_default() — сброс к дефолту;
  // style_from_table{ text={r,g,b,a}, window=..., button=... } — тема из именованных цветов
  // (неуказанные берут дефолт). Это «загрузка темы», а не мутация одного цвета: nk_style_from_table
  // применяет цвета во все под-стили разом.
  nk_tbl.set_function("style_default", [this]() { nk_style_from_table(ctx.get(), nullptr); });
  nk_tbl.set_function("style_from_table", [this](const sol::table& colors) {
    nk_color table[NK_COLOR_COUNT];
    seed_default_theme(table);
    for (const auto& kv : colors) {
      if (!kv.first.is<std::string>() || !kv.second.is<sol::table>()) continue;
      const int idx = nk_color_index_from_name(kv.first.as<std::string>());
      if (idx < 0) { utils::warn("nk.style_from_table: unknown element '{}'", kv.first.as<std::string>()); continue; }
      table[idx] = table_to_nk_color(kv.second.as<sol::table>());
    }
    nk_style_from_table(ctx.get(), table);
  });
}

system::~system() noexcept {
  bindings::setup_nk_context(nullptr);
  if (cmds) nk_buffer_free(cmds.get());
  if (ctx) nk_free(ctx.get());
}

void system::add_font(const std::string& name, const font_t* f) {
  if (f == nullptr) return;
  for (auto& [n, ptr] : fonts_) if (n == name) { ptr = f; return; } // перерегистрация имени
  fonts_.emplace_back(name, f);
}

const font_t* system::resolve_font(std::string_view name) const {
  for (const auto& [n, ptr] : fonts_) if (n == name) return ptr;
  return default_font;
}

bool system::is_font_texture(int id) const {
  if (id == 0) return false; // id 0 = "нет текстуры" (фигуры/solid), а не незагруженный атлас
  for (const auto& [n, ptr] : fonts_) {
    if (ptr != nullptr && ptr->nkfont && ptr->nkfont->texture.id == id) return true;
  }
  return false;
}

nk_user_font* system::sized_font(const font_t* base, float height) {
  if (base == nullptr) base = default_font;
  const int tex_id = base->nkfont->texture.id;
  for (auto &[b, h, uf] : sized_fonts_) {
    if (b == base && h == height) { uf->texture.id = tex_id; return uf.get(); }
  }
  // копируем базовый nkfont (query/width/userdata=font_t*/texture) и меняем только height
  auto uf = std::make_unique<nk_user_font>(*base->nkfont);
  uf->height = height;
  nk_user_font* ptr = uf.get();
  sized_fonts_.emplace_back(base, height, std::move(uf));
  return ptr;
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

void system::set_env_number(const std::string& name, const double value) {
  env[name] = value;
}

void system::update(const size_t time, const size_t timestamp, const uint64_t rng_state) {
  instruction_counter = 0;
  start_tp = clock_t::now();

  // сброс арены эффектов на кадр. offset 0 = дефолтный эффект (без жирности/контура): его берёт
  // весь текст без push_font (у него nk userdata.id = 0). Реальные push_font получают offset > 0.
  effect_arena.clear();
  userdata_stack.clear();
  effect_arena.create<ui_text_effect>(ui_text_effect{0.0f, 0.0f, 0u, 0.0f});
  nk_handle zero; zero.id = 0;
  nk_set_user_data(ctx.get(), zero);

  // защищённый вызов: ошибка в lua не должна ронять движок (panic->abort) и, что важнее,
  // не должна оставлять nk в рассинхроне (незакрытое окно -> ассерт на следующем nk_begin).
  sol::protected_function pf = entry;
  const auto r = pf(time, timestamp, bindings::rng_state{rng_state});
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

  // По texture.id отличаем текст от фигур: фигуры nuklear идут с null-текстурой (id 0), текст —
  // с атласом ОДНОГО ИЗ зарегистрированных шрифтов (is_font_texture), остальное — image.

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
    if (is_font_texture(int(cmd->texture.id))) out.mode = gui_draw_mode::msdf;
    else if (cmd->texture.id == 0)             out.mode = gui_draw_mode::solid;
    else                                       out.mode = gui_draw_mode::image;

    // запекаем SDF-эффект по userdata (offset в арене; 0 = дефолт). Гард по границам арены.
    ui_text_effect eff{0.0f, 0.0f, 0u, 0.0f};
    const size_t eoff = size_t(cmd->userdata.id);
    if (eoff + sizeof(ui_text_effect) <= effect_arena.size()) {
      eff = *reinterpret_cast<const ui_text_effect*>(effect_arena.at(eoff));
    }
    out.boldness = eff.boldness;
    out.outline_width = eff.outline_width;
    out.outline_color = eff.outline_color;
    out.softness = eff.softness;
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
