#include "nuklear_bindings.h"

#include "visage/header.h"
#include "utils/core.h"

// как сюда передать контекст?
// по идее у нас для этого есть общий сол стейт
// но лучше бы какнибудь через юзердату
// через статический класс? а может ли у нас быть несколько nk_context?
// вообще потенциально может
// например я хочу сделать небольшие интерфейсики над юнитами в игре
// их можно запараллелить... хотяяяяя
// в этом подходе в основном трудности
// интерфейсики придется делать отдельно в одном потоке походу

namespace devils_engine {
namespace bindings {
struct nk {
  static nk_context* ctx_ptr;
  static std::unique_ptr<nk_text_edit> default_editor;
  static std::string default_text_editor_buffer;
  static sol::function text_edit_filter;

  // еще нужно добавить работу со шрифтом
  // это должен быть отдельный тип с некоторым количеством данных
  // с ним должно быть легко взаимодействовать + нужно его откуда то получать
  // + передавать в наклир
  // шрифт как и остальные вещи мы получим из ресурсов
  // тут мы можем определить юзердату для этого шрифта
  // в принципе мы даже можем в какой то мере сделать в итоге дистанс филд фонтс

  // как сделать nk_color хорошо? тут беда вот какая:
  // либо цвет задан как 255,255,255,255
  // либо как 1.0f, 1.0f, 1.0f, 1.0f
  // имеет смысл выбрать что то одно
  static struct nk_color to_color(const sol::table &t) {
    struct nk_color c = {};
    c.r = nk_byte(float(t.get_or(1, 0.0f)) * 255.0f);
    c.g = nk_byte(float(t.get_or(2, 0.0f)) * 255.0f);
    c.b = nk_byte(float(t.get_or(3, 0.0f)) * 255.0f);
    c.a = nk_byte(float(t.get_or(4, 0.0f)) * 255.0f);
    return c;
  }

  static struct nk_colorf to_colorf(const sol::table &t) {
    struct nk_colorf c = {};
    c.r = t.get_or(1, 0.0f);
    c.g = t.get_or(2, 0.0f);
    c.b = t.get_or(3, 0.0f);
    c.a = t.get_or(4, 0.0f);
    return c;
  }

  static struct nk_rect to_rect(const sol::table &t) {
    struct nk_rect c = {};
    c.x = t.get_or(1, 0.0f);
    c.y = t.get_or(2, 0.0f);
    c.w = t.get_or(3, 0.0f);
    c.h = t.get_or(4, 0.0f);
    return c;
  }

  static struct nk_vec2 to_vec2(const sol::table &t) {
    struct nk_vec2 c = {};
    c.x = t.get_or(1, 0.0f);
    c.y = t.get_or(2, 0.0f);
    return c;
  }

  static bool begin(const char* title, const sol::table &rect, const uint32_t flags) {
    const auto r = to_rect(rect);
    return nk_begin(ctx_ptr, title, r, flags);
  }

  static bool begin_titled(const char* name, const char* title, const sol::table &rect, const uint32_t flags) {
    const auto r = to_rect(rect);
    return nk_begin_titled(ctx_ptr, name, title, r, flags);
  }

  static void end() {
    nk_end(ctx_ptr);
  }

  //nk_window_find
  // window

  static auto window_get_bounds() {
    const auto &r = nk_window_get_bounds(ctx_ptr);
    return sol::as_table(std::initializer_list{r.x, r.y, r.w, r.h});
  }

  static auto window_get_position() {
    const auto &p = nk_window_get_position(ctx_ptr);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  static auto window_get_size() {
    const auto &p = nk_window_get_size(ctx_ptr);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  static float window_get_width() {
    return nk_window_get_width(ctx_ptr);
  }

  static float window_get_height() {
    return nk_window_get_height(ctx_ptr);
  }

  //nk_window_get_panel

  static auto window_get_content_region() {
    const auto &r = nk_window_get_content_region(ctx_ptr);
    return sol::as_table(std::initializer_list{r.x, r.y, r.w, r.h});
  }

  static auto window_get_content_region_min() {
    const auto &p = nk_window_get_content_region_min(ctx_ptr);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  static auto window_get_content_region_max() {
    const auto &p = nk_window_get_content_region_max(ctx_ptr);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  static auto window_get_content_region_size() {
    const auto &p = nk_window_get_content_region_size(ctx_ptr);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  //nk_window_get_canvas - не уверен что потребуется

  static std::tuple<uint32_t, uint32_t> window_get_scroll() {
    uint32_t x, y;
    nk_window_get_scroll(ctx_ptr, &x, &y);
    return std::make_tuple(x, y);
  }

  static bool window_has_focus() {
    return nk_window_has_focus(ctx_ptr);
  }

  static bool window_is_hovered() {
    return nk_window_is_hovered(ctx_ptr);
  }

  static bool window_is_any_hovered() {
    return nk_window_is_any_hovered(ctx_ptr);
  }

  static bool item_is_any_active() {
    return nk_item_is_any_active(ctx_ptr);
  }

  static bool window_is_collapsed(const char* name) {
    return nk_window_is_collapsed(ctx_ptr, name);
  }

  static bool window_is_closed(const char* name) {
    return nk_window_is_closed(ctx_ptr, name);
  }

  static bool window_is_hidden(const char* name) {
    return nk_window_is_hidden(ctx_ptr, name);
  }

  static bool window_is_active(const char* name) {
    return nk_window_is_active(ctx_ptr, name);
  }

  static void window_set_bounds(const char* name, const sol::table &rect) {
    const auto r = to_rect(rect);
    nk_window_set_bounds(ctx_ptr, name, r);
  }

  static void window_set_position(const char* name, const sol::table &rect) {
    const auto r = to_vec2(rect);
    nk_window_set_position(ctx_ptr, name, r);
  }

  static void window_set_size(const char* name, const sol::table &rect) {
    const auto r = to_vec2(rect);
    nk_window_set_size(ctx_ptr, name, r);
  }

  static void window_set_focus(const char* name) {
    nk_window_set_focus(ctx_ptr, name);
  }

  static void window_set_scroll(const uint32_t x, const uint32_t y) {
    nk_window_set_scroll(ctx_ptr, x, y);
  }

  static void window_close(const char *name) {
    nk_window_close(ctx_ptr, name);
  }

  static void window_collapse(const char *name, const int32_t state) {
    nk_window_collapse(ctx_ptr, name, nk_collapse_states(state));
  }

  static void window_collapse_if(const char *name, const int32_t state, const bool cond) {
    nk_window_collapse_if(ctx_ptr, name, nk_collapse_states(state), cond);
  }

  static void window_show(const char *name, const int32_t state, const bool cond) {
    nk_window_show(ctx_ptr, name, nk_show_states(state));
  }

  static void window_show_if(const char *name, const int32_t state, const bool cond) {
    nk_window_show_if(ctx_ptr, name, nk_show_states(state), cond);
  }

  static void rule_horizontal(const sol::table &color, const bool rounding) {
    const auto c = to_color(color);
    nk_rule_horizontal(ctx_ptr, c, rounding);
  }

  // layout

  static void layout_set_min_row_height(const float height) {
    nk_layout_set_min_row_height(ctx_ptr, height);
  }

  static void layout_reset_min_row_height() {
    nk_layout_reset_min_row_height(ctx_ptr);
  }

  static auto layout_widget_bounds() {
    const auto &r = nk_layout_widget_bounds(ctx_ptr);
    return sol::as_table(std::initializer_list{r.x, r.y, r.w, r.h});
  }

  static float layout_ratio_from_pixel(const float pixel_width) {
    return nk_layout_ratio_from_pixel(ctx_ptr, pixel_width);
  }

  static void layout_row_dynamic(const float height, const int32_t cols) {
    nk_layout_row_dynamic(ctx_ptr, height, cols);
  }

  static void layout_row_static(const float height, const int32_t item_width, const int32_t cols) {
    nk_layout_row_static(ctx_ptr, height, item_width, cols);
  }

  static void layout_row_begin(const int32_t fmt, const float height, const int32_t cols) {
    nk_layout_row_begin(ctx_ptr, nk_layout_format(fmt), height, cols);
  }

  static void layout_row_push(const float height) {
    nk_layout_row_push(ctx_ptr, height);
  }

  static void layout_row_end() {
    nk_layout_row_end(ctx_ptr);
  }

  static void layout_row(const int32_t fmt, const float height, const sol::table &ratios) {
    float ratios_buffer[64]{0.0f};
    int32_t counter = 0;
    for (const auto &[key, value] : ratios) {
      if (key.is<int>() && value.is<float>() && key.as<int>() == DEVILS_ENGINE_TO_LUA_INDEX(counter) && counter < 64) {
        ratios_buffer[counter] = value.as<float>();
        counter += 1;
      }
    }

    nk_layout_row(ctx_ptr, nk_layout_format(fmt), height, counter, ratios_buffer);
  }

  static void layout_row_template_begin(const float height) {
    nk_layout_row_template_begin(ctx_ptr, height);
  }

  static void layout_row_template_push_dynamic() {
    nk_layout_row_template_push_dynamic(ctx_ptr);
  }

  static void layout_row_template_push_variable(const float min_width) {
    nk_layout_row_template_push_variable(ctx_ptr, min_width);
  }

  static void layout_row_template_push_static(const float width) {
    nk_layout_row_template_push_static(ctx_ptr, width);
  }

  static void layout_row_template_end() {
    nk_layout_row_template_end(ctx_ptr);
  }

  static void layout_space_begin(const int32_t fmt, const float height, const int32_t widget_count) {
    nk_layout_space_begin(ctx_ptr, nk_layout_format(fmt), height, widget_count);
  }

  static void layout_space_push(const sol::table &rect) {
    struct nk_rect r = {};
    r.x = rect.get_or(1, 0.0f);
    r.y = rect.get_or(2, 0.0f);
    r.w = rect.get_or(3, 0.0f);
    r.h = rect.get_or(4, 0.0f);

    nk_layout_space_push(ctx_ptr, r);
  }

  static void layout_space_end() {
    nk_layout_space_end(ctx_ptr);
  }

  static auto layout_space_bounds() {
    const auto &r = nk_layout_space_bounds(ctx_ptr);
    return sol::as_table(std::initializer_list{r.x, r.y, r.w, r.h});
  }

  static auto layout_space_to_screen(const sol::table &vec) {
    const auto v = to_vec2(vec);
    const auto &p = nk_layout_space_to_screen(ctx_ptr, v);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  static auto layout_space_to_local(const sol::table &vec) {
    const auto v = to_vec2(vec);
    const auto &p = nk_layout_space_to_local(ctx_ptr, v);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  static auto layout_space_rect_to_screen(const sol::table &rect) {
    const auto r = to_rect(rect);
    const auto &p = nk_layout_space_rect_to_screen(ctx_ptr, r);
    return sol::as_table(std::initializer_list{p.x, p.y, p.w, p.h});
  }

  static auto layout_space_rect_to_local(const sol::table &rect) {
    const auto r = to_rect(rect);
    const auto &p = nk_layout_space_rect_to_local(ctx_ptr, r);
    return sol::as_table(std::initializer_list{p.x, p.y, p.w, p.h});
  }

  static void spacer() {
    nk_spacer(ctx_ptr);
  }

  // group

  static bool group_begin(const char* title, const uint32_t flags) {
    return nk_group_begin(ctx_ptr, title, flags);
  }

  static bool group_begin_titled(const char* name, const char* title, const uint32_t flags) {
    return nk_group_begin_titled(ctx_ptr, name, title, flags);
  }

  static void group_end() {
    nk_group_end(ctx_ptr);
  }

  static std::tuple<uint32_t*, uint32_t*> get_or_create_scroll(const char* id, const nk_scroll &scroll) {
    auto win = ctx_ptr->current;
    const int32_t id_len = nk_strlen(id);
    const nk_hash id_hash = nk_murmur_hash(id, id_len, NK_PANEL_GROUP);
    uint32_t* x_offset = nk_find_value_g(win, id_hash);
    uint32_t* y_offset = nullptr;
    if (!x_offset) {
        x_offset = nk_add_value_g(ctx_ptr, win, id_hash, scroll.x);
        y_offset = nk_add_value_g(ctx_ptr, win, id_hash+1, scroll.y);
        if (x_offset == nullptr || y_offset == nullptr) utils::error("Could not create scroll memory by id '{}'", id);
        if (x_offset == nullptr || y_offset == nullptr) return std::make_tuple(nullptr, nullptr);
        *x_offset = scroll.x; // ???
        *y_offset = scroll.y;
    } else y_offset = nk_find_value_g(win, id_hash+1);

    return std::make_tuple(x_offset, y_offset);
  }

  // эта функция нужна чтобы указать значения по умолчанию для полос прокрутки
  static bool group_scrolled_begin(const char* title, const sol::table &t, const uint32_t flags) {
    struct nk_scroll r = {};
    r.x = t.get_or(1, 0u);
    r.y = t.get_or(2, 0u);
    const auto [x_offset, y_offset] = get_or_create_scroll(title, r);
    return nk_group_scrolled_offset_begin(ctx_ptr, x_offset, y_offset, title, flags);
  }

  static void group_scrolled_end() {
    nk_group_scrolled_end(ctx_ptr);
  }

  static std::tuple<uint32_t, uint32_t> group_get_scroll(const char* title) {
    uint32_t x = 0,y = 0;
    nk_group_get_scroll(ctx_ptr, title, &x, &y);
    return std::make_tuple(x, y);
  }

  static void group_set_scroll(const char* id, const sol::table &t) {
    struct nk_scroll r = {};
    r.x = t.get_or(1, 0u);
    r.y = t.get_or(2, 0u);
    nk_group_set_scroll(ctx_ptr, id, r.x, r.y);
  }

  // tree

  static bool tree_push(sol::this_state s, const char* title, const int32_t type, const int32_t state) {
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);
    return nk_tree_push_hashed(ctx_ptr, nk_tree_type(type), title, nk_collapse_states(state), ar.source, ar.srclen, ar.currentline);
  }

  static bool tree_push_id(sol::this_state s, const char* title, const int32_t type, const int32_t state, const int32_t id) {
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);
    return nk_tree_push_hashed(ctx_ptr, nk_tree_type(type), title, nk_collapse_states(state), ar.source, ar.srclen, id);
  }

  static bool tree_push_hashed(const char* title, const int32_t type, const int32_t state, const char* hash, const int32_t id) {
    const size_t len = strlen(hash);
    return nk_tree_push_hashed(ctx_ptr, nk_tree_type(type), title, nk_collapse_states(state), hash, len, id);
  }

  // как передать картинку? есть два типа картинок: просто какое то изображение
  // и регион картинки - тут нужно задать размеры + подрегион 
  // в общем то в моем случае практически всегда хватает просто подрегиона в нормализованных координатах
  // короч тут нужно подумать... скорее всего будет несколько функций принимающих ресурс + размеры
  // из которых будет составлена структура для гпу
  static bool tree_image_push(sol::this_state s, const char* title, const int32_t type, const int32_t state) {
    struct nk_image img;
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);
    utils::error("Not implemented yet");
    return nk_tree_image_push_hashed(ctx_ptr, nk_tree_type(type), img, title, nk_collapse_states(state), ar.source, ar.srclen, ar.currentline);
  }

  static bool tree_image_push_id(sol::this_state s, const char* title, const int32_t type, const int32_t state, const int32_t id) {
    struct nk_image img;
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);
    utils::error("Not implemented yet");
    return nk_tree_image_push_hashed(ctx_ptr, nk_tree_type(type), img, title, nk_collapse_states(state), ar.source, ar.srclen, id);
  }

  static bool tree_image_push_hashed(const char* title, const int32_t type, const int32_t state, const char* hash, const int32_t id) {
    struct nk_image img;
    utils::error("Not implemented yet");
    return nk_tree_image_push_hashed(ctx_ptr, nk_tree_type(type), img, title, nk_collapse_states(state), hash, strlen(hash), id);
  }

  static void tree_pop() {
    nk_tree_pop(ctx_ptr);
  }

  static bool tree_state_push(sol::this_state s, const char* title, const int32_t type, const int32_t init_state) {
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);

    const auto tree_hash = nk_murmur_hash(title, strlen(title), ar.currentline);
    auto win = ctx_ptr->current;
    uint32_t* state = nk_find_value_g(win, tree_hash);
    if (state == nullptr) {
      state = nk_add_value_g(ctx_ptr, win, tree_hash, 0);
      *state = init_state;
    }
    return nk_tree_state_push(ctx_ptr, nk_tree_type(type), title, (enum nk_collapse_states*)state);
  }

  static void tree_state_image_push(sol::this_state s, const char* title, const int32_t type, const int32_t init_state) {
    struct nk_image img;
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);

    const auto tree_hash = nk_murmur_hash(title, strlen(title), ar.currentline);
    auto win = ctx_ptr->current;
    uint32_t* state = nk_find_value_g(win, tree_hash);
    if (state == nullptr) {
      state = nk_add_value_g(ctx_ptr, win, tree_hash, 0);
      *state = init_state;
    }
    utils::error("Not implemented yet");
    nk_tree_state_image_push(ctx_ptr, nk_tree_type(type), img, title, (enum nk_collapse_states*)state);
  }

  static void tree_state_pop() {
    nk_tree_state_pop(ctx_ptr);
  }
  
  static std::tuple<bool, bool> tree_element_push(sol::this_state s, const char* title, const int32_t type, const int32_t init_state, nk_bool sel) {
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);
    
    const auto ret = nk_tree_element_push_hashed(ctx_ptr, nk_tree_type(type), title, nk_collapse_states(init_state), &sel, ar.source, ar.srclen, ar.currentline);
    return std::make_tuple(ret, sel);
  }

  static std::tuple<bool, bool> tree_element_push_id(sol::this_state s, const char* title, const int32_t type, const int32_t init_state, nk_bool sel, const int32_t id) {
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);

    const auto ret = nk_tree_element_push_hashed(ctx_ptr, nk_tree_type(type), title, nk_collapse_states(init_state), &sel, ar.source, ar.srclen, id);
    return std::make_tuple(ret, sel);
  }

  static std::tuple<bool, bool> tree_element_push_hashed(sol::this_state s, const char* title, const int32_t type, const int32_t init_state, nk_bool sel, const char* hash, const int32_t id) {
    const auto ret = nk_tree_element_push_hashed(ctx_ptr, nk_tree_type(type), title, nk_collapse_states(init_state), &sel, hash, strlen(hash), id);
    return std::make_tuple(ret, sel);
  }

  static std::tuple<bool, bool> tree_element_image_push(sol::this_state s, const char* title, const int32_t type, const int32_t init_state, nk_bool sel) {
    struct nk_image img;
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);
    utils::error("Not implemented yet");
    const auto ret = nk_tree_element_image_push_hashed(ctx_ptr, nk_tree_type(type), img, title, nk_collapse_states(init_state), &sel, ar.source, ar.srclen, ar.currentline);
    return std::make_tuple(ret, sel);
  }

  static std::tuple<bool, bool> tree_element_image_push_id(sol::this_state s, const char* title, const int32_t type, const int32_t init_state, nk_bool sel, const int32_t id) {
    struct nk_image img;
    lua_Debug ar;
    lua_getstack(s.L, 1, &ar); // where function was called
    lua_getinfo(s.L, "nSl", &ar);
    utils::error("Not implemented yet");
    const auto ret = nk_tree_element_image_push_hashed(ctx_ptr, nk_tree_type(type), img, title, nk_collapse_states(init_state), &sel, ar.source, ar.srclen, id);
    return std::make_tuple(ret, sel);
  }

  static std::tuple<bool, bool> tree_element_image_push_hashed(sol::this_state s, const char* title, const int32_t type, const int32_t init_state, nk_bool sel, const char* hash, const int32_t id) {
    struct nk_image img;
    utils::error("Not implemented yet");
    const auto ret = nk_tree_element_image_push_hashed(ctx_ptr, nk_tree_type(type), img, title, nk_collapse_states(init_state), &sel, hash, strlen(hash), id);
    return std::make_tuple(ret, sel);
  }

  // надо переделать с nil аргументом

  // list view ???

  //nk_widget_layout_states
  // widget

  static sol::table widget_state(sol::this_state s, const sol::table &rect) {
    sol::state_view lua(s);
    auto r = to_rect(rect);
    const auto ls = nk_widget(&r, ctx_ptr);
    auto table = lua.create_table_with("layout_states", int32_t(ls));
    table[1] = r.x;
    table[2] = r.y;
    table[3] = r.w;
    table[4] = r.h;
    return table;
  }

  static sol::table widget_fitting_state(sol::this_state s, const sol::table &rect, const sol::table &vec) {
    sol::state_view lua(s);
    auto r = to_rect(rect);
    const auto v = to_vec2(vec);

    const auto ls = nk_widget_fitting(&r, ctx_ptr, v);
    auto table = lua.create_table_with("layout_states", int32_t(ls));
    table[1] = r.x;
    table[2] = r.y;
    table[3] = r.w;
    table[4] = r.h;
    return table;
  }

  static auto widget_bounds() {
    const auto r = nk_widget_bounds(ctx_ptr);
    return sol::as_table(std::initializer_list{r.x, r.y, r.w, r.h});
  }

  static auto widget_position() {
    const auto p = nk_widget_position(ctx_ptr);
    return sol::as_table(std::initializer_list{p.x, p.y});
  }

  static auto widget_size() {
    const auto s = nk_widget_size(ctx_ptr);
    return sol::as_table(std::initializer_list{s.x, s.y});
  }

  static float widget_width() {
    return nk_widget_width(ctx_ptr);
  }

  static float widget_height() {
    return nk_widget_height(ctx_ptr);
  }

  static bool widget_is_hovered() {
    return nk_widget_is_hovered(ctx_ptr);
  }

  static bool widget_is_mouse_clicked(const int32_t buttons) {
    return nk_widget_is_mouse_clicked(ctx_ptr, nk_buttons(buttons));
  }

  static bool widget_has_mouse_click_down(const int32_t buttons, const bool down) {
    return nk_widget_has_mouse_click_down(ctx_ptr, nk_buttons(buttons), down);
  }

  static void widget_disable_begin() {
    nk_widget_disable_begin(ctx_ptr);
  }

  static void widget_disable_end() {
    nk_widget_disable_end(ctx_ptr);
  }

  static void spacing(const int32_t cols) {
    nk_spacing(ctx_ptr, cols);
  }

  // basic

  static void text(const char* txt, const sol::optional<sol::table> &opt_color, const uint32_t flags) {
    if (opt_color.has_value()) {
      auto t = opt_color.value();
      const auto c = to_color(t);
      nk_text_colored(ctx_ptr, txt, strlen(txt), flags, c); 
    } else {
      nk_text(ctx_ptr, txt, strlen(txt), flags); 
    }
  }

  static void text_wrap(const char* txt, const sol::optional<sol::table> &opt_color) {
    if (opt_color.has_value()) {
      auto t = opt_color.value();
      const auto c = to_color(t);
      nk_text_wrap_colored(ctx_ptr, txt, strlen(txt), c); 
    } else {
      nk_text_wrap(ctx_ptr, txt, strlen(txt)); 
    }
  }

  static void label(const char* txt, const sol::optional<sol::table> &opt_color, const uint32_t flags) {
    if (opt_color.has_value()) {
      auto t = opt_color.value();
      const auto c = to_color(t);
      nk_label_colored(ctx_ptr, txt, flags, c); 
    } else {
      nk_label(ctx_ptr, txt, flags);
    }
  }

  static void label_wrap(const char* txt, const sol::optional<sol::table> &opt_color) {
    if (opt_color.has_value()) {
      auto t = opt_color.value();
      const auto c = to_color(t);
      nk_label_colored_wrap(ctx_ptr, txt, c); 
    } else {
      nk_label_wrap(ctx_ptr, txt);
    }
  }

  static void image(const struct nk_image* img, const sol::optional<sol::table> &opt_color) {
    if (opt_color.has_value()) {
      auto t = opt_color.value();
      const auto c = to_color(t);
      nk_image_color(ctx_ptr, *img, c); 
    } else {
      nk_image(ctx_ptr, *img);
    }
  }

  // button

  // наверное имеет смысл сократить функции кнопки (или использовать call?)
  // и по приходящему типу посмотреть просто

  static bool button(sol::object o) {
    bool ret = false;
    if (o.is<std::string_view>()) {
      const auto txt = o.as<std::string_view>();
      ret = nk_button_text(ctx_ptr, txt.data(), txt.size());
    } else if (o.is<sol::table>()) {
      const auto t = o.as<sol::table>();
      const auto c = to_color(t);
      ret = nk_button_color(ctx_ptr, c);
    } else if (o.is<uint32_t>()) {
      const auto t = o.as<uint32_t>();
      ret = nk_button_symbol(ctx_ptr, nk_symbol_type(t));
    } else if (o.is<const struct nk_image*>()) {
      const auto img = o.as<const struct nk_image*>();
      ret = nk_button_image(ctx_ptr, *img);
    } else {
      utils::error("'nk.button' input must be either string, table, symbol or image");
    }
    return ret;
  }

  static bool button(sol::object o, const std::string_view &txt, const uint32_t flags) {
    bool ret = false;
    if (!o.valid()) {
      ret = nk_button_text(ctx_ptr, txt.data(), txt.size());
    } else {
      if (o.is<uint32_t>()) {
        const auto t = o.as<uint32_t>();
        ret = nk_button_symbol_text(ctx_ptr, nk_symbol_type(t), txt.data(), txt.size(), flags);
      } else if (o.is<const struct nk_image*>()) {
        const auto img = o.as<const struct nk_image*>();
        ret = nk_button_image_text(ctx_ptr, *img, txt.data(), txt.size(), flags);
      } else {
        utils::error("'nk.button' first argument must be either symbol or image");
      }
    }

    return ret;
  }

  static bool button_text(const char* txt) {
    return nk_button_text(ctx_ptr, txt, strlen(txt));
  }

  /*static bool button_label(const char* txt) {
    return nk_button_label(ctx_ptr, txt);
  }*/

  static bool button_color(const sol::table &t) {
    const auto c = to_color(t);
    return nk_button_color(ctx_ptr, c);
  }

  static bool button_symbol(const uint32_t type) {
    return nk_button_symbol(ctx_ptr, nk_symbol_type(type));
  }

  static bool button_image(const struct nk_image* img) {
    return nk_button_image(ctx_ptr, *img);
  }

  /*static bool button_symbol_label(const uint32_t type, const char* txt, const uint32_t flags) {
    return nk_button_symbol_label(ctx_ptr, nk_symbol_type(type), txt, flags);
  }*/

  static bool button_symbol_text(const uint32_t type, const char* txt, const uint32_t flags) {
    return nk_button_symbol_text(ctx_ptr, nk_symbol_type(type), txt, strlen(txt), flags);
  }

  /*static bool button_image_label(const struct nk_image* img, const char* txt, const uint32_t flags) {
    return nk_button_image_label(ctx_ptr, *img, txt, flags);
  }*/

  static bool button_image_text(const struct nk_image* img, const char* txt, const uint32_t flags) {
    return nk_button_image_text(ctx_ptr, *img, txt, strlen(txt), flags);
  }

  // TODO: сделать функции с учетом стиля (вообще нужно стиль сделать в наклире)

  static void button_set_behavior(const uint32_t enm) {
    nk_button_set_behavior(ctx_ptr, nk_button_behavior(enm));
  }

  static bool button_push_behavior(const uint32_t enm) {
    return nk_button_push_behavior(ctx_ptr, nk_button_behavior(enm));
  }

  static bool button_pop_behavior() {
    return nk_button_pop_behavior(ctx_ptr);
  }

  // checkbox

  static bool check(const char* txt, const bool active) {
    return nk_check_text(ctx_ptr, txt, strlen(txt), active);
  }

  static bool check_align(const char* txt, const bool active, const uint32_t widget_align, const uint32_t text_align) {
    return nk_check_text_align(ctx_ptr, txt, strlen(txt), active, widget_align, text_align);
  }

  static uint32_t check_flags(const char* txt, const uint32_t flags, const uint32_t value) {
    return nk_check_flags_text(ctx_ptr, txt, strlen(txt), flags, value);
  }

  static bool check2(const char* txt, sol::table val) {
    nk_bool active = val.get_or(1, false);
    const bool ret = nk_checkbox_text(ctx_ptr, txt, strlen(txt), &active);
    val[1] = bool(active);
    return ret;
  }

  static bool check_align2(const char* txt, sol::table val, const uint32_t widget_align, const uint32_t text_align) {
    nk_bool active = val.get_or(1, false);
    const bool ret = nk_checkbox_text_align(ctx_ptr, txt, strlen(txt), &active, widget_align, text_align);
    val[1] = bool(active);
    return ret;
  }

  static bool check_flags2(const char* txt, sol::table val, const uint32_t value) {
    uint32_t flags = val.get_or(1, 0u);
    const bool ret = nk_checkbox_flags_text(ctx_ptr, txt, strlen(txt), &flags, value);
    val[1] = flags;
    return ret;
  }

  // option

  static bool option(const char* txt, const bool active) {
    return nk_option_text(ctx_ptr, txt, strlen(txt), active);
  }

  static bool option_align(const char* txt, const bool active, const uint32_t widget_align, const uint32_t text_align) {
    return nk_option_text_align(ctx_ptr, txt, strlen(txt), active, widget_align, text_align);
  }

  static bool radio(const char* txt, sol::table val) {
    nk_bool active = val.get_or(1, false);
    const bool ret = nk_radio_text(ctx_ptr, txt, strlen(txt), &active);
    val[1] = bool(active);
    return ret;
  }

  static bool radio_align(const char* txt, sol::table val, const uint32_t widget_align, const uint32_t text_align) {
    nk_bool active = val.get_or(1, false);
    const bool ret = nk_radio_text_align(ctx_ptr, txt, strlen(txt), &active, widget_align, text_align);
    val[1] = bool(active);
    return ret;
  }

  // select

  static bool select(const char* txt, const uint32_t align, const bool value) {
    return nk_select_text(ctx_ptr, txt, strlen(txt), align, value);
  }

  static bool select_image(const struct nk_image* img, const char* txt, const uint32_t align, const bool value) {
    return nk_select_image_text(ctx_ptr, *img, txt, strlen(txt), align, value);
  }

  static bool select_symbol(const uint32_t symb, const char* txt, const uint32_t align, const bool value) {
    return nk_select_symbol_text(ctx_ptr, nk_symbol_type(symb), txt, strlen(txt), align, value);
  }

  static bool select2(const sol::object &o, const char* txt, const uint32_t align, const bool value) {
    bool ret = false;
    if (o.valid()) {
      if (o.is<uint32_t>()) {
        const uint32_t symb = o.as<uint32_t>();
        ret = nk_select_symbol_text(ctx_ptr, nk_symbol_type(symb), txt, strlen(txt), align, value);
      } else if (o.is<const struct nk_image*>()) {
        const struct nk_image* img = o.as<const struct nk_image*>();
        ret = nk_select_image_text(ctx_ptr, *img, txt, strlen(txt), align, value);
      } else {
        utils::error("Function 'nk.select' expects symbol or image as first argument");
      }
    } else {
      ret = nk_select_text(ctx_ptr, txt, strlen(txt), align, value);
    }
    return ret;
  }

  static bool selectable(const sol::object &o, const char* txt, const uint32_t align, sol::table val) {
    nk_bool value = val.get_or(1, false);
    bool ret = false;
    if (o.valid()) {
      if (o.is<uint32_t>()) {
        const uint32_t symb = o.as<uint32_t>();
        ret = nk_selectable_symbol_text(ctx_ptr, nk_symbol_type(symb), txt, strlen(txt), align, &value);
      } else if (o.is<const struct nk_image*>()) {
        const struct nk_image* img = o.as<const struct nk_image*>();
        ret = nk_selectable_image_text(ctx_ptr, *img, txt, strlen(txt), align, &value);
      } else {
        utils::error("Function 'nk.selectable' expects symbol or image as first argument");
      }
    } else {
      ret = nk_selectable_text(ctx_ptr, txt, strlen(txt), align, &value);
    }
    val[1] = bool(value);
    return ret;
  }

  // slider
  
  // нужен ли мне еще один слайдер?
  static double slider_float(const double min, const double val, const double max, const double step) {
    return nk_slide_float(ctx_ptr, min, val, max, step);
  }

  static int64_t slider_int(const int64_t min, const int64_t val, const int64_t max, const int64_t step) {
    return nk_slide_int(ctx_ptr, min, val, max, step);
  }

  static bool slider_float2(const double min, sol::table val, const double max, const double step) {
    float cur = val.get_or(1, min); // min ?
    const bool ret = nk_slider_float(ctx_ptr, min, &cur, max, step);
    val[1] = cur;
    return ret;
  }

  static bool slider_int2(const int64_t min, sol::table val, const int64_t max, const int64_t step) {
    int32_t cur = val.get_or(1, min); // min ?
    const bool ret = nk_slider_int(ctx_ptr, min, &cur, max, step);
    val[1] = cur;
    return ret;
  }

  // knob

  static float knob_float(const float min, float val, const float max, const float step, const uint32_t zero_direction, const float dead_zone_degrees) {
    const bool ret = nk_knob_float(ctx_ptr, min, &val, max, step, nk_heading(zero_direction), dead_zone_degrees);
    (void)ret; // че делать с этим?
    return val;
  }

  static int32_t knob_int(const int32_t min, int32_t val, const int32_t max, const int32_t step, const uint32_t zero_direction, const float dead_zone_degrees) {
    const bool ret = nk_knob_int(ctx_ptr, min, &val, max, step, nk_heading(zero_direction), dead_zone_degrees);
    (void)ret; // че делать с этим?
    return val;
  }

  static bool knob_float2(const float min, sol::table val, const float max, const float step, const uint32_t zero_direction, const float dead_zone_degrees) {
    float cur = val.get_or(1, min); // min ?
    const bool ret = nk_knob_float(ctx_ptr, min, &cur, max, step, nk_heading(zero_direction), dead_zone_degrees);
    val[1] = cur;
    return ret;
  }

  static int32_t knob_int2(const int32_t min, sol::table val, const int32_t max, const int32_t step, const uint32_t zero_direction, const float dead_zone_degrees) {
    int32_t cur = val.get_or(1, min); // min ?
    const bool ret = nk_knob_int(ctx_ptr, min, &cur, max, step, nk_heading(zero_direction), dead_zone_degrees);
    val[1] = cur;
    return ret;
  }

  // progressbar

  // все функции в которых изменяется аргумент
  // могут принимать таблицу в которой первым слотом сидит этот аргумент
  // тогда можно легко повторить все наклир функции нормально
  static size_t progress(const size_t cur, const size_t max, const sol::optional<bool> modifyable) {
    return nk_prog(ctx_ptr, cur, max, modifyable.has_value() && modifyable.value());
  }

  static bool progress2(sol::table cur, const size_t max, const sol::optional<bool> modifyable) {
    size_t val = cur.get_or(1, 0ull);
    const auto ret = nk_progress(ctx_ptr, &val, max, modifyable.has_value() && modifyable.value());
    cur[1] = val;
    return ret;
  }

  // color_picker
  
  static bool color_picker(sol::table t, const uint32_t frm) {
    nk_colorf c = {};
    c.r = t.get_or(1, 0.0f);
    c.g = t.get_or(2, 0.0f);
    c.b = t.get_or(3, 0.0f);
    c.a = t.get_or(4, 0.0f);
    const auto ret = nk_color_pick(ctx_ptr, &c, nk_color_format(frm));
    t[1] = c.r;
    t[2] = c.g;
    t[3] = c.b;
    t[4] = c.a;
    return ret;
  }

  // property

  static double property(const char* title, const double min, const double val, const double max, const double step, const double inc_per_pixel) {
    return nk_propertyd(ctx_ptr, title, min, val, max, step, inc_per_pixel);
  }

  static void property2(const char* title, const double min, sol::table val, const double max, const double step, const double inc_per_pixel) {
    double cur = val.get_or(1, min);
    nk_property_double(ctx_ptr, title, min, &cur, max, step, inc_per_pixel);
    val[1] = cur;
  }

  // editor

  static nk_bool custom_filter(const nk_text_edit*, const nk_rune unicode) {
    const auto ret = text_edit_filter(unicode);
    sol_lua_check_error(ret);
    return bool(ret);
  }

  static std::string edit_string(const char* str, const size_t max, const uint32_t flags, sol::object filter) {
    default_text_editor_buffer.resize(max, '\0');
    int32_t size = strlen(str);
    memcpy(default_text_editor_buffer.data(), str, size);

    nk_plugin_filter f = nullptr;
    if (!filter.valid()) {
      f = &nk_filter_default;
    } else if (filter.is<std::string_view>()) {
      const auto name = filter.as<std::string_view>();
           if (name == "default") f = &nk_filter_default;
      else if (name == "ascii") f = &nk_filter_ascii;
      else if (name == "float") f = &nk_filter_float;
      else if (name == "decimal") f = &nk_filter_decimal;
      else if (name == "hex") f = &nk_filter_hex;
      else if (name == "oct") f = &nk_filter_oct;
      else if (name == "binary") f = &nk_filter_binary;
      else utils::error("Could not find filter by name '{}'", name);
    } else if (filter.is<sol::function>()) {
      const auto fil = filter.as<sol::function>();
      text_edit_filter = fil;
      f = &custom_filter;
    } else {
      utils::error("'nk.edit_string' unsupported text editor filter type");
    }

    nk_edit_string(ctx_ptr, flags, default_text_editor_buffer.data(), &size, max, f);
    std::string final_str(default_text_editor_buffer.data(), size);
    default_text_editor_buffer.clear();
    text_edit_filter = sol::nil;

    return final_str;
  }

  static uint32_t edit_string2(sol::table t, const size_t max, const uint32_t flags, sol::object filter) {
    //auto str = t.get_or(1, std::string()); // блин очень плохо =(
    //str.resize(std::max(str.size(), max));
    auto str = t.get_or(1, std::string_view());
    default_text_editor_buffer.resize(max, '\0');
    if (!str.empty()) memcpy(default_text_editor_buffer.data(), str.data(), str.size());

    nk_plugin_filter f = nullptr;
    if (!filter.valid()) {
      f = &nk_filter_default;
    } else if (filter.is<std::string_view>()) {
      const auto name = filter.as<std::string_view>();
           if (name == "default") f = &nk_filter_default;
      else if (name == "ascii") f = &nk_filter_ascii;
      else if (name == "float") f = &nk_filter_float;
      else if (name == "decimal") f = &nk_filter_decimal;
      else if (name == "hex") f = &nk_filter_hex;
      else if (name == "oct") f = &nk_filter_oct;
      else if (name == "binary") f = &nk_filter_binary;
      else utils::error("Could not find filter by name '{}'", name);
    } else if (filter.is<sol::function>()) {
      const auto fil = filter.as<sol::function>();
      text_edit_filter = fil;
      f = &custom_filter;
    } else {
      utils::error("'nk.edit_string2' unsupported text editor filter type");
    }

    int32_t size = default_text_editor_buffer.size();
    const uint32_t ret_flags = nk_edit_string(ctx_ptr, flags, default_text_editor_buffer.data(), &size, max, f);
    std::string final_str(default_text_editor_buffer.data(), size);
    default_text_editor_buffer.clear();
    text_edit_filter = sol::nil;
    t[1] = final_str;

    return ret_flags;
  }

  static void edit_focus(const uint32_t flags) {
    nk_edit_focus(ctx_ptr, flags);
  }

  static void edit_unfocus() {
    nk_edit_unfocus(ctx_ptr);
  }

  // chart
  
  static bool chart_begin(const uint32_t type, const int32_t count, const float min, const float max) {
    return nk_chart_begin(ctx_ptr, nk_chart_type(type), count, min, max);
  }

  static bool chart_begin_colored(const uint32_t type, const sol::table col1, const sol::table col2, const int32_t count, const float min, const float max) {
    const auto c1 = to_color(col1);
    const auto c2 = to_color(col2);
    return nk_chart_begin_colored(ctx_ptr, nk_chart_type(type), c1, c2, count, min, max);
  }

  static void chart_add_slot(const uint32_t type, const int32_t count, const float min, const float max) {
    nk_chart_add_slot(ctx_ptr, nk_chart_type(type), count, min, max);
  }

  static void chart_add_slot_colored(const uint32_t type, const sol::table col1, const sol::table col2, const int32_t count, const float min, const float max) {
    const auto c1 = to_color(col1);
    const auto c2 = to_color(col2);
    nk_chart_add_slot_colored(ctx_ptr, nk_chart_type(type), c1, c2, count, min, max);
  }

  static uint32_t chart_push(const float value) {
    return nk_chart_push(ctx_ptr, value);
  }

  static uint32_t chart_push_slot(const float value, const int32_t slot) {
    return nk_chart_push_slot(ctx_ptr, value, slot);
  }

  static void chart_end(const uint32_t type, const int32_t count, const float min, const float max) {
    nk_chart_end(ctx_ptr);
  }

  static float get_plot_value_from_table(void* user, int32_t index) {
    const auto t_p = reinterpret_cast<sol::table*>(user);
    return float((*t_p)[DEVILS_ENGINE_TO_LUA_INDEX(index)]);
  }

  static float get_plot_value_from_function(void* user, int32_t index) {
    const auto t_p = reinterpret_cast<sol::function*>(user);
    const auto ret = (*t_p)(DEVILS_ENGINE_TO_LUA_INDEX(index));
    sol_lua_check_error(ret);
    return float(ret);
  }

  static void plot(const uint32_t type, sol::object v, const int32_t count, const int32_t offset) {
    if (v.is<sol::table>()) {
      auto t = v.as<sol::table>();
      nk_plot_function(ctx_ptr, nk_chart_type(type), &t, &get_plot_value_from_table, count, offset);
    } else if (v.is<sol::function>()) {
      auto f = v.as<sol::function>();
      nk_plot_function(ctx_ptr, nk_chart_type(type), &f, &get_plot_value_from_function, count, offset);
    } else {
      utils::error("'nk.plot' expects table or function as input");
    }
  }

  // popup

  static bool popup_begin(const uint32_t type, const char* title, const uint32_t flags, const sol::table &rect) {
    const auto r = to_rect(rect);
    return nk_popup_begin(ctx_ptr, nk_popup_type(type), title, flags, r);
  }

  static void popup_close() {
    nk_popup_close(ctx_ptr);
  }

  static void popup_end() {
    nk_popup_end(ctx_ptr);
  }

  static std::tuple<uint32_t, uint32_t> popup_get_scroll() {
    uint32_t x=0,y=0;
    nk_popup_get_scroll(ctx_ptr, &x, &y);
    return std::make_tuple(x, y);
  }

  static void popup_set_scroll(const uint32_t x, const uint32_t y) {
    nk_popup_set_scroll(ctx_ptr, x, y);
  }

  // combobox

  // тут по идее наклир скопирует строчку к себе, а значит скорее всего она будет существовать без проблем в памяти

  static void get_combobox_value_from_table(void* user, int32_t index, const char** ptr) {
    const auto t_p = reinterpret_cast<sol::table*>(user);
    auto str = (*t_p)[DEVILS_ENGINE_TO_LUA_INDEX(index)].get<const char*>();
    *ptr = str;
  }

  static void get_combobox_value_from_function(void* user, int32_t index, const char** ptr) {
    const auto t_p = reinterpret_cast<sol::function*>(user);
    const auto ret = (*t_p)(DEVILS_ENGINE_TO_LUA_INDEX(index));
    sol_lua_check_error(ret);
    auto str = ret.get<const char*>();
    *ptr = str;
  }

  static int32_t combobox(const sol::object &o, const int32_t count, const int32_t selected, const int32_t item_size, const sol::table &vec) {
    int32_t ret = 0;
    const auto v = to_vec2(vec);
    if (o.is<sol::table>()) {
      auto t = o.as<sol::table>();
      ret = nk_combo_callback(ctx_ptr, &get_combobox_value_from_table, &t, selected, count, item_size, v);
    } else if (o.is<sol::function>()) {
      auto f = o.as<sol::function>();
      ret = nk_combo_callback(ctx_ptr, &get_combobox_value_from_function, &f, selected, count, item_size, v);
    } else {
      utils::error("'nk.combobox' expects table of function as input");
    }

    return ret;
  }

  // abstract combobox

  static bool combo_begin(const sol::optional<const char*> &name, const sol::object &symbol_or_img, const sol::table &vec) {
    bool ret = false;
    const auto v = to_vec2(vec);
    if (symbol_or_img.valid()) {
      if (symbol_or_img.is<const struct nk_image*>()) {
        auto img = symbol_or_img.as<const struct nk_image*>();
        if (name.has_value()) {
          auto n = name.value();
          ret = nk_combo_begin_image_text(ctx_ptr, n, strlen(n), *img, v);
        } else {
          ret = nk_combo_begin_image(ctx_ptr, *img, v);
        }
      } else if (symbol_or_img.is<uint32_t>()) {
        const auto s = symbol_or_img.as<uint32_t>();
        if (name.has_value()) {
          auto n = name.value();
          ret = nk_combo_begin_symbol_text(ctx_ptr, n, strlen(n), nk_symbol_type(s), v);
        } else {
          ret = nk_combo_begin_symbol(ctx_ptr, nk_symbol_type(s), v);
        }
      } else {
        utils::error("'nk.combo.begin' either nil, img or symbol must be provided as second argument");
      }
    } else {
      if (name.has_value()) {
        auto n = name.value();
        ret = nk_combo_begin_text(ctx_ptr, n, strlen(n), v);
      } else {
        utils::error("'nk.combo.begin' first argument must be valid if second is nil");
      }
    }

    return ret;
  }

  /*static bool combo_begin_image(const sol::optional<const char*> &name, const struct nk_image *img, const sol::table &vec) {
    bool ret = false;
    const auto v = to_vec2(vec);
    if (name.has_value()) {
      auto n = name.value();
      ret = nk_combo_begin_image_text(ctx_ptr, n, strlen(n), *img, v);
    } else {
      ret = nk_combo_begin_image(ctx_ptr, *img, v);
    }

    return ret;
  }*/

  static bool combo_item(const sol::object &symbol_or_img, const char* name, const uint32_t flags) {
    bool ret = false;
    if (symbol_or_img.valid()) {
      if (symbol_or_img.is<const struct nk_image*>()) {
        auto img = symbol_or_img.as<const struct nk_image*>();
        ret = nk_combo_item_image_text(ctx_ptr, *img, name, strlen(name), flags);
      } else if (symbol_or_img.is<uint32_t>()) {
        const auto s = symbol_or_img.as<uint32_t>();
        ret = nk_combo_item_symbol_text(ctx_ptr, nk_symbol_type(s), name, strlen(name), flags);
      } else {
        utils::error("'nk.combo.item' either nil, img or symbol must be provided as first argument");
      }
    } else {
      ret = nk_combo_item_text(ctx_ptr, name, strlen(name), flags);
    }

    return ret;
  }

  static void combo_close() {
    nk_combo_close(ctx_ptr);
  }

  static void combo_end() {
    nk_combo_end(ctx_ptr);
  }

  // contextual

  static bool contextual_begin(const uint32_t flags, const sol::table &size, const sol::table &trigger_bounds) {
    const auto v1 = to_vec2(size);
    const auto v2 = to_rect(trigger_bounds);
    return nk_contextual_begin(ctx_ptr, flags, v1, v2);
  }

  static bool contextual_item(const sol::object &symbol_or_img, const char* name, const uint32_t aligment) {
    bool ret = false;
    if (symbol_or_img.valid()) {
      if (symbol_or_img.is<const struct nk_image*>()) {
        auto img = symbol_or_img.as<const struct nk_image*>();
        ret = nk_contextual_item_image_text(ctx_ptr, *img, name, strlen(name), aligment);
      } else if (symbol_or_img.is<uint32_t>()) {
        const auto s = symbol_or_img.as<uint32_t>();
        ret = nk_contextual_item_symbol_text(ctx_ptr, nk_symbol_type(s), name, strlen(name), aligment);
      } else {
        utils::error("'nk.contextual.item' either nil, img or symbol must be provided as second argument");
      }
    } else {
      ret = nk_contextual_item_text(ctx_ptr, name, strlen(name), aligment);
    }

    return ret;
  }

  static void contextual_close() {
    nk_contextual_close(ctx_ptr);
  }

  static void contextual_end() {
    nk_contextual_end(ctx_ptr);
  }

  // tooltip

  static void tooltip(const char* str) {
    nk_tooltip(ctx_ptr, str);
  }

  static bool tooltip_begin(const float width) {
    return nk_tooltip_begin(ctx_ptr, width);
  }

  static void tooltip_end() {
    nk_tooltip_end(ctx_ptr);
  }

  // menu

  static void menubar_begin() {
    nk_menubar_begin(ctx_ptr);
  }

  static void menubar_end() {
    nk_menubar_end(ctx_ptr);
  }

  static bool menu_begin_text(const char* name, const sol::object &symbol_or_img, const uint32_t align, const sol::table &vec) {
    bool ret = false;
    const auto v1 = to_vec2(vec);
    if (symbol_or_img.valid()) {
      if (symbol_or_img.is<const struct nk_image*>()) {
        auto img = symbol_or_img.as<const struct nk_image*>();
        ret = nk_menu_begin_image_text(ctx_ptr, name, strlen(name), align, *img, v1);
      } else if (symbol_or_img.is<uint32_t>()) {
        const auto s = symbol_or_img.as<uint32_t>();
        ret = nk_menu_begin_symbol_text(ctx_ptr, name, strlen(name), align, nk_symbol_type(s), v1);
      } else {
        utils::error("'nk.menu.begin_text' either nil, img or symbol must be provided as second argument");
      }
    } else {
      ret = nk_menu_begin_text(ctx_ptr, name, strlen(name), align, v1);
    }

    return ret;
  }

  static bool menu_begin(const char* name, const sol::object &symbol_or_img, const sol::table &vec) {
    bool ret = false;
    const auto v1 = to_vec2(vec);
    if (symbol_or_img.is<const struct nk_image*>()) {
      auto img = symbol_or_img.as<const struct nk_image*>();
      ret = nk_menu_begin_image(ctx_ptr, name, *img, v1);
    } else if (symbol_or_img.is<uint32_t>()) {
      const auto s = symbol_or_img.as<uint32_t>();
      ret = nk_menu_begin_symbol(ctx_ptr, name, nk_symbol_type(s), v1);
    } else {
      utils::error("'nk.menu.begin' either img or symbol must be provided as second argument");
    }

    return ret;
  }

  static bool menu_item(const char* name, const sol::object &symbol_or_img, const uint32_t align) {
    bool ret = false;
    if (symbol_or_img.valid()) {
      if (symbol_or_img.is<const struct nk_image*>()) {
        auto img = symbol_or_img.as<const struct nk_image*>();
        ret = nk_menu_item_image_text(ctx_ptr, *img, name, strlen(name), align);
      } else if (symbol_or_img.is<uint32_t>()) {
        const auto s = symbol_or_img.as<uint32_t>();
        ret = nk_menu_item_symbol_text(ctx_ptr, nk_symbol_type(s), name, strlen(name), align);
      } else {
        utils::error("'nk.menu.item' either nil, img or symbol must be provided as second argument");
      }
    } else {
      ret = nk_menu_item_text(ctx_ptr, name, strlen(name), align);
    }

    return ret;
  }

  static void menu_close() {
    nk_menu_close(ctx_ptr);
  }

  static void menu_end() {
    nk_menu_end(ctx_ptr);
  }

  // после чего стили ... 
};

nk_context* nk::ctx_ptr = nullptr;
std::unique_ptr<nk_text_edit> nk::default_editor;
std::string nk::default_text_editor_buffer;
sol::function nk::text_edit_filter;

void setup_nk_context(nk_context* ptr) {
  nk::ctx_ptr = ptr;
}

void nk_functions(sol::table t) {
  auto nk = t.create_named("nk");
  /*{
    auto enm = nk.create_named("window_flags");
    enm.set("dynamic", nk_window_flags::NK_WINDOW_DYNAMIC);
    enm.set("private", nk_window_flags::NK_WINDOW_PRIVATE);
    enm.set("rom", nk_window_flags::NK_WINDOW_ROM);
    enm.set("not_interactive", nk_window_flags::NK_WINDOW_NOT_INTERACTIVE);
    enm.set("hidden", nk_window_flags::NK_WINDOW_HIDDEN);
    enm.set("closed", nk_window_flags::NK_WINDOW_CLOSED);
    enm.set("minimized", nk_window_flags::NK_WINDOW_MINIMIZED);
    enm.set("remove_rom", nk_window_flags::NK_WINDOW_REMOVE_ROM);
    enm.set("no_input", NK_WINDOW_NO_INPUT);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }*/
  {
    auto enm = nk.create_named("panel_flags");
    enm.set("border", nk_panel_flags::NK_WINDOW_BORDER);
    enm.set("movable", nk_panel_flags::NK_WINDOW_MOVABLE);
    enm.set("scalable", nk_panel_flags::NK_WINDOW_SCALABLE);
    enm.set("closable", nk_panel_flags::NK_WINDOW_CLOSABLE);
    enm.set("minimizable", nk_panel_flags::NK_WINDOW_MINIMIZABLE);
    enm.set("no_scrollbar", nk_panel_flags::NK_WINDOW_NO_SCROLLBAR);
    enm.set("title", nk_panel_flags::NK_WINDOW_TITLE);
    enm.set("scroll_auto_hide", nk_panel_flags::NK_WINDOW_SCROLL_AUTO_HIDE);
    enm.set("background", nk_panel_flags::NK_WINDOW_BACKGROUND);
    enm.set("scale_left", nk_panel_flags::NK_WINDOW_SCALE_LEFT);
    enm.set("no_input", nk_panel_flags::NK_WINDOW_NO_INPUT);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("collapse_states");
    enm.set("minimized", nk_collapse_states::NK_MINIMIZED);
    enm.set("maximized", nk_collapse_states::NK_MAXIMIZED);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("modify");
    enm.set("fixed", nk_modify::NK_FIXED);
    enm.set("modifiable", nk_modify::NK_MODIFIABLE);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("orientation");
    enm.set("vertical", nk_orientation::NK_VERTICAL);
    enm.set("horizontal", nk_orientation::NK_HORIZONTAL);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("show_states");
    enm.set("hidden", nk_show_states::NK_HIDDEN);
    enm.set("shown", nk_show_states::NK_SHOWN);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("show_states");
    enm.set("hidden", nk_show_states::NK_HIDDEN);
    enm.set("shown", nk_show_states::NK_SHOWN);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("chart_type");
    enm.set("lines", nk_chart_type::NK_CHART_LINES);
    enm.set("column", nk_chart_type::NK_CHART_COLUMN);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("chart_event");
    enm.set("hovering", nk_chart_event::NK_CHART_HOVERING);
    enm.set("clicked", nk_chart_event::NK_CHART_CLICKED);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("popup_type");
    enm.set("static", nk_popup_type::NK_POPUP_STATIC);
    enm.set("dynamic", nk_popup_type::NK_POPUP_DYNAMIC);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("layout_format");
    enm.set("dynamic", nk_layout_format::NK_DYNAMIC);
    enm.set("static", nk_layout_format::NK_STATIC);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("tree_type");
    enm.set("node", nk_tree_type::NK_TREE_NODE);
    enm.set("tab", nk_tree_type::NK_TREE_TAB);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("color_format");
    enm.set("rgb", nk_color_format::NK_RGB);
    enm.set("rgba", nk_color_format::NK_RGBA);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("symbol_type");
    enm.set("none", nk_symbol_type::NK_SYMBOL_NONE);
    enm.set("x", nk_symbol_type::NK_SYMBOL_X);
    enm.set("underscore", nk_symbol_type::NK_SYMBOL_UNDERSCORE);
    enm.set("circle_solid", nk_symbol_type::NK_SYMBOL_CIRCLE_SOLID);
    enm.set("circle_outline", nk_symbol_type::NK_SYMBOL_CIRCLE_OUTLINE);
    enm.set("rect_solid", nk_symbol_type::NK_SYMBOL_RECT_SOLID);
    enm.set("rect_outline", nk_symbol_type::NK_SYMBOL_RECT_OUTLINE);
    enm.set("triangle_up", nk_symbol_type::NK_SYMBOL_TRIANGLE_UP);
    enm.set("triangle_down", nk_symbol_type::NK_SYMBOL_TRIANGLE_DOWN);
    enm.set("triangle_left", nk_symbol_type::NK_SYMBOL_TRIANGLE_LEFT);
    enm.set("triangle_right", nk_symbol_type::NK_SYMBOL_TRIANGLE_RIGHT);
    enm.set("plus", nk_symbol_type::NK_SYMBOL_PLUS);
    enm.set("minus", nk_symbol_type::NK_SYMBOL_MINUS);
    enm.set("triangle_up_outline", nk_symbol_type::NK_SYMBOL_TRIANGLE_UP_OUTLINE);
    enm.set("triangle_down_outline", nk_symbol_type::NK_SYMBOL_TRIANGLE_DOWN_OUTLINE);
    enm.set("triangle_left_outline", nk_symbol_type::NK_SYMBOL_TRIANGLE_LEFT_OUTLINE);
    enm.set("triangle_right_outline", nk_symbol_type::NK_SYMBOL_TRIANGLE_RIGHT_OUTLINE);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("widget_alignment");
    enm.set("align_left", nk_widget_align::NK_WIDGET_ALIGN_LEFT);
    enm.set("align_centered", nk_widget_align::NK_WIDGET_ALIGN_CENTERED);
    enm.set("align_right", nk_widget_align::NK_WIDGET_ALIGN_RIGHT);
    enm.set("align_top", nk_widget_align::NK_WIDGET_ALIGN_TOP);
    enm.set("align_middle", nk_widget_align::NK_WIDGET_ALIGN_MIDDLE);
    enm.set("align_bottom", nk_widget_align::NK_WIDGET_ALIGN_BOTTOM);
    enm.set("left", nk_widget_alignment::NK_WIDGET_LEFT);
    enm.set("centered", nk_widget_alignment::NK_WIDGET_CENTERED);
    enm.set("right", nk_widget_alignment::NK_WIDGET_RIGHT);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("text_edit_type");
    enm.set("single_line", nk_text_edit_type::NK_TEXT_EDIT_SINGLE_LINE);
    enm.set("multi_line", nk_text_edit_type::NK_TEXT_EDIT_MULTI_LINE);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("text_edit_mode");
    enm.set("view", nk_text_edit_mode::NK_TEXT_EDIT_MODE_VIEW);
    enm.set("insert", nk_text_edit_mode::NK_TEXT_EDIT_MODE_INSERT);
    enm.set("replace", nk_text_edit_mode::NK_TEXT_EDIT_MODE_REPLACE);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("edit_types");
    enm.set("default", nk_edit_flags::NK_EDIT_DEFAULT);
    enm.set("read_only", nk_edit_flags::NK_EDIT_READ_ONLY);
    enm.set("auto_select", nk_edit_flags::NK_EDIT_AUTO_SELECT);
    enm.set("sig_enter", nk_edit_flags::NK_EDIT_SIG_ENTER);
    enm.set("allow_tab", nk_edit_flags::NK_EDIT_ALLOW_TAB);
    enm.set("no_cursor", nk_edit_flags::NK_EDIT_NO_CURSOR);
    enm.set("selectable", nk_edit_flags::NK_EDIT_SELECTABLE);
    enm.set("clipboard", nk_edit_flags::NK_EDIT_CLIPBOARD);
    enm.set("ctrl_enter_new_line", nk_edit_flags::NK_EDIT_CTRL_ENTER_NEWLINE);
    enm.set("no_horizontal_scroll", nk_edit_flags::NK_EDIT_NO_HORIZONTAL_SCROLL);
    enm.set("always_insert_mode", nk_edit_flags::NK_EDIT_ALWAYS_INSERT_MODE);
    enm.set("multiline", nk_edit_flags::NK_EDIT_MULTILINE);
    enm.set("goto_end_on_activate", nk_edit_flags::NK_EDIT_GOTO_END_ON_ACTIVATE);

    enm.set("simple", nk_edit_types::NK_EDIT_SIMPLE);
    enm.set("field", nk_edit_types::NK_EDIT_FIELD);
    enm.set("box", nk_edit_types::NK_EDIT_BOX);
    enm.set("editor", nk_edit_types::NK_EDIT_EDITOR);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("edit_events");
    enm.set("active", nk_edit_events::NK_EDIT_ACTIVE);
    enm.set("inactive", nk_edit_events::NK_EDIT_INACTIVE);
    enm.set("activated", nk_edit_events::NK_EDIT_ACTIVATED);
    enm.set("deactivated", nk_edit_events::NK_EDIT_DEACTIVATED);
    enm.set("commited", nk_edit_events::NK_EDIT_COMMITED);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("style_colors");
    enm.set("text", nk_style_colors::NK_COLOR_TEXT);
    enm.set("window", nk_style_colors::NK_COLOR_WINDOW);
    enm.set("header", nk_style_colors::NK_COLOR_HEADER);
    enm.set("border", nk_style_colors::NK_COLOR_BORDER);
    enm.set("button", nk_style_colors::NK_COLOR_BUTTON);
    enm.set("button_hover", nk_style_colors::NK_COLOR_BUTTON_HOVER);
    enm.set("button_active", nk_style_colors::NK_COLOR_BUTTON_ACTIVE);
    enm.set("toggle", nk_style_colors::NK_COLOR_TOGGLE);
    enm.set("toggle_hover", nk_style_colors::NK_COLOR_TOGGLE_HOVER);
    enm.set("toggle_cursor", nk_style_colors::NK_COLOR_TOGGLE_CURSOR);
    enm.set("select", nk_style_colors::NK_COLOR_SELECT);
    enm.set("select_active", nk_style_colors::NK_COLOR_SELECT_ACTIVE);
    enm.set("slider", nk_style_colors::NK_COLOR_SLIDER);
    enm.set("slider_cursor", nk_style_colors::NK_COLOR_SLIDER_CURSOR);
    enm.set("slider_hover", nk_style_colors::NK_COLOR_SLIDER_CURSOR_HOVER);
    enm.set("slider_active", nk_style_colors::NK_COLOR_SLIDER_CURSOR_ACTIVE);
    enm.set("property", nk_style_colors::NK_COLOR_PROPERTY);
    enm.set("edit", nk_style_colors::NK_COLOR_EDIT);
    enm.set("edit_cursor", nk_style_colors::NK_COLOR_EDIT_CURSOR);
    enm.set("combo", nk_style_colors::NK_COLOR_COMBO);
    enm.set("chart", nk_style_colors::NK_COLOR_CHART);
    enm.set("chart_color", nk_style_colors::NK_COLOR_CHART_COLOR);
    enm.set("chart_highlight", nk_style_colors::NK_COLOR_CHART_COLOR_HIGHLIGHT);
    enm.set("scrollbar", nk_style_colors::NK_COLOR_SCROLLBAR);
    enm.set("scrollbar_cursor", nk_style_colors::NK_COLOR_SCROLLBAR_CURSOR);
    enm.set("scrollbar_hover", nk_style_colors::NK_COLOR_SCROLLBAR_CURSOR_HOVER);
    enm.set("scrollbar_active", nk_style_colors::NK_COLOR_SCROLLBAR_CURSOR_ACTIVE);
    enm.set("tab_header", nk_style_colors::NK_COLOR_TAB_HEADER);
    enm.set("knob", nk_style_colors::NK_COLOR_KNOB);
    enm.set("knob_cursor", nk_style_colors::NK_COLOR_KNOB_CURSOR);
    enm.set("knob_hover", nk_style_colors::NK_COLOR_KNOB_CURSOR_HOVER);
    enm.set("knob_active", nk_style_colors::NK_COLOR_KNOB_CURSOR_ACTIVE);
    enm.set("count", nk_style_colors::NK_COLOR_COUNT);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto enm = nk.create_named("style_cursor");
    enm.set("arrow", nk_style_cursor::NK_CURSOR_ARROW);
    enm.set("text", nk_style_cursor::NK_CURSOR_TEXT);
    enm.set("move", nk_style_cursor::NK_CURSOR_MOVE);
    enm.set("resize_vertical", nk_style_cursor::NK_CURSOR_RESIZE_VERTICAL);
    enm.set("resize_horizontal", nk_style_cursor::NK_CURSOR_RESIZE_HORIZONTAL);
    enm.set("top_left_down_right", nk_style_cursor::NK_CURSOR_RESIZE_TOP_LEFT_DOWN_RIGHT);
    enm.set("top_right_down_left", nk_style_cursor::NK_CURSOR_RESIZE_TOP_RIGHT_DOWN_LEFT);
    enm.set("count", nk_style_cursor::NK_CURSOR_COUNT);
    enm.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }

  nk.set_function("begin", &nk::begin);
  nk.set_function("begin_titled", &nk::begin_titled);
  nk.set_function("endf", &nk::end);
  nk.set_function("item_is_any_active", &nk::item_is_any_active);
  nk.set_function("rule_horizontal", &nk::rule_horizontal);
  nk.set_function("spacer", &nk::spacer);
  nk.set_function("spacing", &nk::spacing);
  nk.set_function("text", &nk::text);
  nk.set_function("label", &nk::label);
  nk.set_function("text_wrap", &nk::text_wrap);
  nk.set_function("label_wrap", &nk::label_wrap);
  nk.set_function("image", &nk::image);
  nk.set_function("checkbox", &nk::check);
  nk.set_function("checkbox_align", &nk::check_align);
  nk.set_function("checkbox_flags", &nk::check_flags);
  nk.set_function("checkbox2", &nk::check2);
  nk.set_function("checkbox_align2", &nk::check_align2);
  nk.set_function("checkbox_flags2", &nk::check_flags2);
  nk.set_function("option", &nk::option);
  nk.set_function("option_align", &nk::option_align);
  nk.set_function("radio", &nk::radio);
  nk.set_function("radio_align", &nk::radio_align);
  nk.set_function("slider", &nk::slider_float);
  nk.set_function("slider2", &nk::slider_float2);
  nk.set_function("progress", &nk::progress);
  nk.set_function("progress2", &nk::progress2);
  nk.set_function("color_picker", &nk::color_picker);
  nk.set_function("property", &nk::property);
  nk.set_function("property2", &nk::property2);
  nk.set_function("knob", &nk::knob_float);
  nk.set_function("knob2", &nk::knob_float2);

  // селект напрашивается на отдельную штуку
  //nk.set_function("select", &nk::select);
  //nk.set_function("select_image", &nk::select_image);
  //nk.set_function("select_symbol", &nk::select_symbol);
  nk.set_function("select", &nk::select2);
  nk.set_function("selectable", &nk::selectable);

  nk.set_function("edit_string", &nk::edit_string);
  nk.set_function("edit_string2", &nk::edit_string2);
  nk.set_function("edit_focus", &nk::edit_focus);
  nk.set_function("edit_unfocus", &nk::edit_unfocus);

  nk.set_function("plot", &nk::plot);
  nk.set_function("combobox", &nk::combobox);

  nk.set_function("tooltip", &nk::tooltip);
  nk.set_function("tooltip_begin", &nk::tooltip_begin);
  nk.set_function("tooltip_end", &nk::tooltip_end);

  {
    auto w = nk.create_named("window");
    w.set_function("get_bounds", &nk::window_get_bounds);
    w.set_function("get_position", &nk::window_get_position);
    w.set_function("get_size", &nk::window_get_size);
    w.set_function("get_width", &nk::window_get_width);
    w.set_function("get_height", &nk::window_get_height);
    w.set_function("get_content_region", &nk::window_get_content_region);
    w.set_function("get_content_region_min", &nk::window_get_content_region_min);
    w.set_function("get_content_region_max", &nk::window_get_content_region_max);
    w.set_function("get_content_region_size", &nk::window_get_content_region_size);
    w.set_function("get_scroll", &nk::window_get_scroll);
    w.set_function("has_focus", &nk::window_has_focus);
    w.set_function("is_hovered", &nk::window_is_hovered);
    w.set_function("is_any_hovered", &nk::window_is_any_hovered);
    w.set_function("is_collapsed", &nk::window_is_collapsed);
    w.set_function("is_closed", &nk::window_is_closed);
    w.set_function("is_hidden", &nk::window_is_hidden);
    w.set_function("is_active", &nk::window_is_active);
    w.set_function("set_bounds", &nk::window_set_bounds);
    w.set_function("set_position", &nk::window_set_position);
    w.set_function("set_size", &nk::window_set_size);
    w.set_function("set_focus", &nk::window_set_focus);
    w.set_function("set_scroll", &nk::window_set_scroll);
    w.set_function("close", &nk::window_close);
    w.set_function("collapse", &nk::window_collapse);
    w.set_function("collapse_if", &nk::window_collapse_if);
    w.set_function("show", &nk::window_show);
    w.set_function("show_if", &nk::window_show_if);
    w.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto l = nk.create_named("layout");
    l.set_function("set_min_row_height", &nk::layout_set_min_row_height);
    l.set_function("reset_min_row_height", &nk::layout_reset_min_row_height);
    l.set_function("widget_bounds", &nk::layout_widget_bounds);
    l.set_function("ratio_from_pixel", &nk::layout_ratio_from_pixel);
    l.set_function("row_dynamic", &nk::layout_row_dynamic);
    l.set_function("row_static", &nk::layout_row_static);
    l.set_function("row_begin", &nk::layout_row_begin);
    l.set_function("row_push", &nk::layout_row_push);
    l.set_function("row_end", &nk::layout_row_end); // ???
    l.set_function("row", &nk::layout_row);
    l.set_function("row_template_begin", &nk::layout_row_template_begin);
    l.set_function("row_template_push_dynamic", &nk::layout_row_template_push_dynamic);
    l.set_function("row_template_push_variable", &nk::layout_row_template_push_variable);
    l.set_function("row_template_push_static", &nk::layout_row_template_push_static);
    l.set_function("row_template_end", &nk::layout_row_template_end);
    l.set_function("space_begin", &nk::layout_space_begin);
    l.set_function("space_push", &nk::layout_space_push);
    l.set_function("space_end", &nk::layout_space_end);
    l.set_function("space_bounds", &nk::layout_space_bounds);
    l.set_function("space_to_screen", &nk::layout_space_to_screen);
    l.set_function("space_to_local", &nk::layout_space_to_local);
    l.set_function("space_rect_to_screen", &nk::layout_space_rect_to_screen);
    l.set_function("space_rect_to_local", &nk::layout_space_rect_to_local);
    l.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto g = nk.create_named("group");
    g.set_function("begin", &nk::group_begin);
    g.set_function("begin_titled", &nk::group_begin_titled);
    g.set_function("endf", &nk::group_end);
    g.set_function("scrolled_begin", &nk::group_scrolled_begin);
    g.set_function("scrolled_end", &nk::group_scrolled_end);
    g.set_function("get_scroll", &nk::group_get_scroll);
    g.set_function("set_scroll", &nk::group_set_scroll);
    g.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto t = nk.create_named("tree");
    t.set_function("push", &nk::tree_push);
    t.set_function("push_id", &nk::tree_push_id);
    t.set_function("push_hashed", &nk::tree_push_hashed);
    t.set_function("image_push", &nk::tree_image_push);
    t.set_function("image_push_id", &nk::tree_image_push_id);
    t.set_function("image_push_hashed", &nk::tree_image_push_hashed);
    t.set_function("pop", &nk::tree_pop);
    t.set_function("state_push", &nk::tree_state_push);
    t.set_function("state_image_push", &nk::tree_state_image_push);
    t.set_function("state_pop", &nk::tree_state_pop);
    t.set_function("element_push", &nk::tree_element_push);
    t.set_function("element_push_id", &nk::tree_element_push_id);
    t.set_function("element_push_hashed", &nk::tree_element_push_hashed);
    t.set_function("element_image_push", &nk::tree_element_image_push);
    t.set_function("element_image_push_id", &nk::tree_element_image_push_id);
    t.set_function("element_image_push_hashed", &nk::tree_element_image_push_hashed);
    t.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto w = nk.create_named("widget");
    w.set_function("state", &nk::widget_state);
    w.set_function("fitting_state", &nk::widget_fitting_state);
    w.set_function("bounds", &nk::widget_bounds);
    w.set_function("position", &nk::widget_position);
    w.set_function("size", &nk::widget_size);
    w.set_function("width", &nk::widget_width);
    w.set_function("height", &nk::widget_height);
    w.set_function("is_hovered", &nk::widget_is_hovered);
    w.set_function("is_mouse_clicked", &nk::widget_is_mouse_clicked);
    w.set_function("has_mouse_click_down", &nk::widget_has_mouse_click_down);
    w.set_function("disable_begin", &nk::widget_disable_begin);
    w.set_function("disable_end", &nk::widget_disable_end);
    w.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    using bf1_t = bool(*)(sol::object);
    using bf2_t = bool(*)(sol::object, const std::string_view &, const uint32_t);
    bf1_t f1 = &nk::button;
    bf2_t f2 = &nk::button;
    auto b = nk.create_named("button");
    b.set_function(sol::meta_function::call, sol::overload(f1, f2));
    /*
    b.set_function("text", &nk::button_text);
    b.set_function("color", &nk::button_color);
    b.set_function("symbol", &nk::button_symbol);
    b.set_function("image", &nk::button_image);
    b.set_function("symbol_text", &nk::button_symbol_text);
    b.set_function("image_text", &nk::button_image_text);
    */
    b.set_function("set_behavior", &nk::button_set_behavior);
    b.set_function("push_behavior", &nk::button_push_behavior);
    b.set_function("pop_behavior", &nk::button_pop_behavior);
    b.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto b = nk.create_named("chart");
    b.set_function("begin", &nk::chart_begin);
    b.set_function("begin_colored", &nk::chart_begin_colored);
    b.set_function("add_slot", &nk::chart_add_slot);
    b.set_function("add_slot_colored", &nk::chart_add_slot_colored);
    b.set_function("push", &nk::chart_push);
    b.set_function("push_slot", &nk::chart_push_slot);
    b.set_function("endf", &nk::chart_end);
    b.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto b = nk.create_named("popup");
    b.set_function("begin", &nk::popup_begin);
    b.set_function("close", &nk::popup_close);
    b.set_function("endf", &nk::popup_end);
    b.set_function("get_scroll", &nk::popup_get_scroll);
    b.set_function("set_scroll", &nk::popup_set_scroll);
    b.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto b = nk.create_named("combo");
    b.set_function("begin", &nk::combo_begin);
    b.set_function("item", &nk::combo_item);
    b.set_function("close", &nk::combo_close);
    b.set_function("endf", &nk::combo_end);
    b.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto b = nk.create_named("contextual");
    b.set_function("begin", &nk::contextual_begin);
    b.set_function("item", &nk::contextual_item);
    b.set_function("close", &nk::contextual_close);
    b.set_function("endf", &nk::contextual_end);
    b.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }
  {
    auto b1 = nk.create_named("menubar");
    b1.set_function("begin", &nk::menubar_begin);
    b1.set_function("end", &nk::menubar_end);
    b1.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);

    auto b = nk.create_named("menu");
    b.set_function("begin_text", &nk::menu_begin_text);
    b.set_function("begin", &nk::menu_begin);
    b.set_function("item", &nk::menu_item);
    b.set_function("close", &nk::menu_close);
    b.set_function("endf", &nk::menu_end);
    b.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
  }

  nk.set_function(sol::meta_function::new_index, sol::detail::fail_on_newindex);
}
}
}
