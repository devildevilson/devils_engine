#include "system.h"

#include <cstring>
#include <algorithm>
#include <bit>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/time-utils.hpp"
#include "devils_engine/bindings/env.h"
#include "devils_engine/bindings/nuklear_bindings.h"
#include "header.h"
#include "font.h"
#include "image.h"

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

// Параметры image-эффектов (cooldown/mix), кладутся в тот же effect_arena; convert читает по ТИПУ
// (из tex_id) и запекает в payload draw-команды. mask/comp — индексы в bindless tex[] (или RGBA8-цвет).
struct ui_image_effect {
  uint32_t mask_index = 0;      // слот маски-трафарета
  float    fill = 0.0f;         // cooldown: доля заполнения [0,1]
  uint32_t comp[4] = {0, 0, 0, 0}; // mix: слот картинки (если бит is_image) или packed RGBA8-цвет
  uint32_t is_image = 0;        // mix: биты 0..3 — какой comp это картинка
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

// placement/mirror-флаги картинки (битовая маска, зеркалит таблицу nk.placement ниже). mirror-биты
// извлекаются в nk.image и пакуются в id текстуры (tex_id::pack) — тип картинки отличает её от фигуры,
// см. render_output.h (раньше это делал одиночный бит-флаг image_id_flag, теперь — поле type).
namespace img_placement {
  enum : uint32_t {
    fill        = 0,       // растянуть на весь виджет (дефолт; синоним stretch)
    scale_ratio = 1u << 0, // вписать сохранив пропорции (fit)
    center      = 1u << 1, // выравнивание по центру (дефолт при scale_ratio — флаг для явности)
    left        = 1u << 2,
    right       = 1u << 3,
    top         = 1u << 4,
    bottom      = 1u << 5,
    mirror_u    = 1u << 6, // флип по u (пакуется в id)
    mirror_v    = 1u << 7, // флип по v
  };
}

// целевой прямоугольник картинки внутри bounds по placement-флагам. Без scale_ratio — весь bounds
// (stretch). Со scale_ratio — вписать по min-масштабу (сохранить аспект) и выровнять (дефолт — центр).
static struct nk_rect image_placement_rect(const struct nk_rect& b, const float iw, const float ih, const uint32_t flags) {
  if (!(flags & img_placement::scale_ratio) || iw <= 0.0f || ih <= 0.0f) return b;
  const float s = std::min(b.w / iw, b.h / ih);
  const float w = iw * s, h = ih * s;
  float x, y;
  if (flags & img_placement::left)  x = b.x;
  else if (flags & img_placement::right) x = b.x + b.w - w;
  else x = b.x + (b.w - w) * 0.5f; // center по умолчанию
  if (flags & img_placement::top)   y = b.y;
  else if (flags & img_placement::bottom) y = b.y + b.h - h;
  else y = b.y + (b.h - h) * 0.5f;
  return nk_rect(x, y, w, h);
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

  // Картинки (шаг: image + placement). Регистрируем тут (а не в bindings — тот не зависит от visage;
  // + нужен nk_context). visage::image — POD-хендл (слот текстуры + регион), строит хост (app.image),
  // позже будет отдавать demiurg-ресурс. nk.image перекрывает мёртвую заглушку из bindings::nk_functions.
  lua.new_usertype<visage::image>("image",
    sol::no_constructor, // конструируется хостом (app.image), не из lua
    "texture_id", &visage::image::texture_id,
    "w", &visage::image::w,
    "h", &visage::image::h);

  // placement-флаги (битовая маска, комбинируются через '|'); зеркалит img_placement выше.
  sol::table placement = nk_tbl.create_named("placement");
  placement["fill"]        = uint32_t(img_placement::fill);
  placement["stretch"]     = uint32_t(img_placement::fill);
  placement["scale_ratio"] = uint32_t(img_placement::scale_ratio);
  placement["center"]      = uint32_t(img_placement::center);
  placement["left"]        = uint32_t(img_placement::left);
  placement["right"]       = uint32_t(img_placement::right);
  placement["top"]         = uint32_t(img_placement::top);
  placement["bottom"]      = uint32_t(img_placement::bottom);
  placement["mirror_u"]    = uint32_t(img_placement::mirror_u);
  placement["mirror_v"]    = uint32_t(img_placement::mirror_v);

  // nk.image(img [, placement_flags] [, color]) — берёт слот виджета (nk_widget), считает целевой
  // прямоугольник по placement (вписать/растянуть/выровнять) и рисует nk_draw_image. mode=image в
  // convert() выставится сам (ненулевая не-шрифтовая текстура), ui.frag mode 2 сэмплит.
  nk_tbl.set_function("image",
    [this](const visage::image& img, sol::optional<uint32_t> placement_flags, sol::optional<sol::table> color) {
      if (ctx->current == nullptr) return; // не внутри begin/end окна — рисовать некуда
      struct nk_rect bounds;
      const auto state = nk_widget(&bounds, ctx.get()); // продвигает layout + отдаёт bounds
      if (state == NK_WIDGET_INVALID) return;           // виджет вне видимой области

      // регион: если задан (region[2]!=0) — суб-прямоугольник, иначе вся картинка
      const struct nk_rect region = img.region[2] != 0
        ? nk_rect(float(img.region[0]), float(img.region[1]), float(img.region[2]), float(img.region[3]))
        : nk_rect(0.0f, 0.0f, float(img.w), float(img.h));
      const uint32_t flags = placement_flags.value_or(uint32_t(img_placement::fill));
      // пакуем тип(image)+mirror+индекс в id: тип отличает картинку от фигуры (слот 0 больше не путается),
      // mirror и индекс декодит шейдер (см. tex_id в render_output.h).
      const uint32_t packed = tex_id::pack(gui_draw_mode::image, img.texture_id,
        (flags & img_placement::mirror_u) != 0u, (flags & img_placement::mirror_v) != 0u);
      const struct nk_image nkimg = nk_subimage_id(int(packed), img.w, img.h, region);
      const struct nk_rect target = image_placement_rect(bounds, region.w, region.h, flags);
      const struct nk_color c = color.has_value() ? table_to_nk_color(color.value()) : nk_rgba(255, 255, 255, 255);
      nk_draw_image(&ctx->current->buffer, target, &nkimg, c);
    });

  // Стенсил-эффект COOLDOWN: картинка проявляется по градиент-маске на долю fill (незаполненное
  // затемнено). nk.image_gradient{ img=<image>, mask=<image>, fill=0..1, placement=? }. Параметры —
  // в effect_arena (offset в userdata), тип cooldown — в id; шейдер сэмплит img + mask, ревил по fill.
  nk_tbl.set_function("image_gradient", [this](sol::table t) {
    if (ctx->current == nullptr) return;
    const sol::optional<visage::image> img = t["img"];
    const sol::optional<visage::image> mask = t["mask"];
    if (!img || !mask) return;
    const float fill = float(t.get_or("fill", 1.0));
    const uint32_t flags = uint32_t(t.get_or("placement", double(img_placement::fill)));

    struct nk_rect bounds;
    if (nk_widget(&bounds, ctx.get()) == NK_WIDGET_INVALID) return;
    const struct nk_rect region = img->region[2] != 0
      ? nk_rect(float(img->region[0]), float(img->region[1]), float(img->region[2]), float(img->region[3]))
      : nk_rect(0.0f, 0.0f, float(img->w), float(img->h));
    const struct nk_rect target = image_placement_rect(bounds, region.w, region.h, flags);

    auto* e = effect_arena.create<ui_image_effect>(ui_image_effect{ mask->texture_id, fill, {0, 0, 0, 0}, 0 });
    nk_handle h; h.id = int(effect_arena.offset_of(e));
    const int prev_ud = ctx->userdata.id;
    nk_set_user_data(ctx.get(), h);
    const uint32_t packed = tex_id::pack(gui_draw_mode::cooldown, img->texture_id,
      (flags & img_placement::mirror_u) != 0u, (flags & img_placement::mirror_v) != 0u);
    const struct nk_image nkimg = nk_subimage_id(int(packed), img->w, img->h, region);
    nk_draw_image(&ctx->current->buffer, target, &nkimg, nk_rgba(255, 255, 255, 255));
    nk_handle ph; ph.id = prev_ud; nk_set_user_data(ctx.get(), ph); // вернуть прежний userdata
  });

  // Стенсил-эффект MIX (4-blend): до 4 компонентов (картинки/цвета) смешиваются по каналам маски
  // (R/G/B = вес comp0..2, comp3 = 1-R-G-B, альфа = непрозрачность). nk.image_mix{ comps={…≤4}, mask= }.
  // Каждый comp — visage::image (картинка) ИЛИ таблица цвета {r,g,b,a}. Заливает весь слот виджета.
  nk_tbl.set_function("image_mix", [this](sol::table t) {
    if (ctx->current == nullptr) return;
    const sol::optional<visage::image> mask = t["mask"];
    if (!mask) return;
    const sol::optional<sol::table> comps = t["comps"];

    ui_image_effect eff{};
    eff.mask_index = mask->texture_id;
    if (comps) {
      for (int i = 0; i < 4; ++i) {
        sol::object c = (*comps)[i + 1]; // lua 1-based
        if (c.is<visage::image>()) { eff.comp[i] = c.as<visage::image>().texture_id; eff.is_image |= (1u << i); }
        else if (c.is<sol::table>()) { eff.comp[i] = pack_color01(c.as<sol::table>()); } // RGBA8-цвет
        // иначе comp остаётся 0 (чёрный цвет, вес по маске)
      }
    }

    struct nk_rect bounds;
    if (nk_widget(&bounds, ctx.get()) == NK_WIDGET_INVALID) return;

    auto* e = effect_arena.create<ui_image_effect>(eff);
    nk_handle h; h.id = int(effect_arena.offset_of(e));
    const int prev_ud = ctx->userdata.id;
    nk_set_user_data(ctx.get(), h);
    // тип mix; index не сэмплится шейдером (используются comps+mask). uv [0,1] на весь виджет (w=h=1).
    const struct nk_image nkimg = nk_subimage_id(int(tex_id::pack(gui_draw_mode::mix, 0)), 1, 1, nk_rect(0.0f, 0.0f, 1.0f, 1.0f));
    nk_draw_image(&ctx->current->buffer, bounds, &nkimg, nk_rgba(255, 255, 255, 255));
    nk_handle ph; ph.id = prev_ud; nk_set_user_data(ctx.get(), ph);
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

void system::set_entry_point(const sol::object& value) {
  if (!value.is<sol::function>()) utils::error{}("visage: entry point must be a lua function");
  entry = value.as<sol::function>();
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
    // id УПАКОВАН (tex_id: тип+mirror+индекс) на стороне продюсера: nk.image пакует type=image,
    // font_t::set_texture_id — type=msdf, фигуры nuklear идут с texture.id==0 (type=0=solid). Поэтому
    // здесь просто passthrough — декод (тип/mirror/индекс) делает шейдер. Больше нет эвристик.
    out.texture_id = uint32_t(cmd->texture.id);

    // payload запекаем ПО ТИПУ (из tex_id): msdf → SDF-поля; cooldown/mix → ui_image_effect. Оба
    // структа лежат в одном effect_arena по offset из nk userdata (producer пишет нужный). Гард границ.
    for (auto& p : out.payload) p = 0u;
    const uint32_t type = tex_id::type_of(out.texture_id);
    const size_t eoff = size_t(cmd->userdata.id);
    if (type == gui_draw_mode::msdf) {
      ui_text_effect eff{0.0f, 0.0f, 0u, 0.0f};
      if (eoff + sizeof(ui_text_effect) <= effect_arena.size()) eff = *reinterpret_cast<const ui_text_effect*>(effect_arena.at(eoff));
      out.payload[0] = std::bit_cast<uint32_t>(eff.boldness);
      out.payload[1] = std::bit_cast<uint32_t>(eff.outline_width);
      out.payload[2] = eff.outline_color;
      out.payload[3] = std::bit_cast<uint32_t>(eff.softness);
    } else if (type == gui_draw_mode::cooldown || type == gui_draw_mode::mix) {
      ui_image_effect eff{};
      if (eoff + sizeof(ui_image_effect) <= effect_arena.size()) eff = *reinterpret_cast<const ui_image_effect*>(effect_arena.at(eoff));
      out.payload[0] = eff.mask_index;
      if (type == gui_draw_mode::cooldown) {
        out.payload[1] = std::bit_cast<uint32_t>(eff.fill);
      } else { // mix
        out.payload[1] = eff.comp[0];
        out.payload[2] = eff.comp[1];
        out.payload[3] = eff.comp[2];
        out.payload[4] = eff.comp[3];
        out.payload[5] = eff.is_image;
      }
    }
    // image(2)/solid(0): payload остаётся нулевым
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
