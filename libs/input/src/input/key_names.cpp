#include "key_names.h"

#include <array>
#include <cstdlib>

#include "GLFW/glfw3.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace devils_engine {
namespace input {
namespace {
struct key_name_entry {
  int32_t glfw_key;
  std::string_view canonical;
  std::string_view us_layout;
};

constexpr std::array key_names = {
  key_name_entry{GLFW_KEY_ESCAPE, "escape", "Esc"},

  key_name_entry{GLFW_KEY_F1, "f1", "F1"},
  key_name_entry{GLFW_KEY_F2, "f2", "F2"},
  key_name_entry{GLFW_KEY_F3, "f3", "F3"},
  key_name_entry{GLFW_KEY_F4, "f4", "F4"},
  key_name_entry{GLFW_KEY_F5, "f5", "F5"},
  key_name_entry{GLFW_KEY_F6, "f6", "F6"},
  key_name_entry{GLFW_KEY_F7, "f7", "F7"},
  key_name_entry{GLFW_KEY_F8, "f8", "F8"},
  key_name_entry{GLFW_KEY_F9, "f9", "F9"},
  key_name_entry{GLFW_KEY_F10, "f10", "F10"},
  key_name_entry{GLFW_KEY_F11, "f11", "F11"},
  key_name_entry{GLFW_KEY_F12, "f12", "F12"},
  key_name_entry{GLFW_KEY_F13, "f13", "F13"},
  key_name_entry{GLFW_KEY_F14, "f14", "F14"},
  key_name_entry{GLFW_KEY_F15, "f15", "F15"},
  key_name_entry{GLFW_KEY_F16, "f16", "F16"},
  key_name_entry{GLFW_KEY_F17, "f17", "F17"},
  key_name_entry{GLFW_KEY_F18, "f18", "F18"},
  key_name_entry{GLFW_KEY_F19, "f19", "F19"},
  key_name_entry{GLFW_KEY_F20, "f20", "F20"},
  key_name_entry{GLFW_KEY_F21, "f21", "F21"},
  key_name_entry{GLFW_KEY_F22, "f22", "F22"},
  key_name_entry{GLFW_KEY_F23, "f23", "F23"},
  key_name_entry{GLFW_KEY_F24, "f24", "F24"},
  key_name_entry{GLFW_KEY_F25, "f25", "F25"},

  key_name_entry{GLFW_KEY_GRAVE_ACCENT, "grave_accent", "`"},
  key_name_entry{GLFW_KEY_1, "key_1", "1"},
  key_name_entry{GLFW_KEY_2, "key_2", "2"},
  key_name_entry{GLFW_KEY_3, "key_3", "3"},
  key_name_entry{GLFW_KEY_4, "key_4", "4"},
  key_name_entry{GLFW_KEY_5, "key_5", "5"},
  key_name_entry{GLFW_KEY_6, "key_6", "6"},
  key_name_entry{GLFW_KEY_7, "key_7", "7"},
  key_name_entry{GLFW_KEY_8, "key_8", "8"},
  key_name_entry{GLFW_KEY_9, "key_9", "9"},
  key_name_entry{GLFW_KEY_0, "key_0", "0"},
  key_name_entry{GLFW_KEY_MINUS, "minus", "-"},
  key_name_entry{GLFW_KEY_EQUAL, "equal", "="},
  key_name_entry{GLFW_KEY_BACKSPACE, "backspace", "Backspace"},

  key_name_entry{GLFW_KEY_TAB, "tab", "Tab"},
  key_name_entry{GLFW_KEY_Q, "key_q", "Q"},
  key_name_entry{GLFW_KEY_W, "key_w", "W"},
  key_name_entry{GLFW_KEY_E, "key_e", "E"},
  key_name_entry{GLFW_KEY_R, "key_r", "R"},
  key_name_entry{GLFW_KEY_T, "key_t", "T"},
  key_name_entry{GLFW_KEY_Y, "key_y", "Y"},
  key_name_entry{GLFW_KEY_U, "key_u", "U"},
  key_name_entry{GLFW_KEY_I, "key_i", "I"},
  key_name_entry{GLFW_KEY_O, "key_o", "O"},
  key_name_entry{GLFW_KEY_P, "key_p", "P"},
  key_name_entry{GLFW_KEY_LEFT_BRACKET, "left_bracket", "["},
  key_name_entry{GLFW_KEY_RIGHT_BRACKET, "right_bracket", "]"},
  key_name_entry{GLFW_KEY_BACKSLASH, "backslash", "\\"},

  key_name_entry{GLFW_KEY_CAPS_LOCK, "caps_lock", "Caps Lock"},
  key_name_entry{GLFW_KEY_A, "key_a", "A"},
  key_name_entry{GLFW_KEY_S, "key_s", "S"},
  key_name_entry{GLFW_KEY_D, "key_d", "D"},
  key_name_entry{GLFW_KEY_F, "key_f", "F"},
  key_name_entry{GLFW_KEY_G, "key_g", "G"},
  key_name_entry{GLFW_KEY_H, "key_h", "H"},
  key_name_entry{GLFW_KEY_J, "key_j", "J"},
  key_name_entry{GLFW_KEY_K, "key_k", "K"},
  key_name_entry{GLFW_KEY_L, "key_l", "L"},
  key_name_entry{GLFW_KEY_SEMICOLON, "semicolon", ";"},
  key_name_entry{GLFW_KEY_APOSTROPHE, "apostrophe", "'"},
  key_name_entry{GLFW_KEY_ENTER, "enter", "Enter"},

  key_name_entry{GLFW_KEY_LEFT_SHIFT, "left_shift", "Left Shift"},
  key_name_entry{GLFW_KEY_Z, "key_z", "Z"},
  key_name_entry{GLFW_KEY_X, "key_x", "X"},
  key_name_entry{GLFW_KEY_C, "key_c", "C"},
  key_name_entry{GLFW_KEY_V, "key_v", "V"},
  key_name_entry{GLFW_KEY_B, "key_b", "B"},
  key_name_entry{GLFW_KEY_N, "key_n", "N"},
  key_name_entry{GLFW_KEY_M, "key_m", "M"},
  key_name_entry{GLFW_KEY_COMMA, "comma", ","},
  key_name_entry{GLFW_KEY_PERIOD, "period", "."},
  key_name_entry{GLFW_KEY_SLASH, "slash", "/"},
  key_name_entry{GLFW_KEY_RIGHT_SHIFT, "right_shift", "Right Shift"},

  key_name_entry{GLFW_KEY_LEFT_CONTROL, "left_control", "Left Ctrl"},
  key_name_entry{GLFW_KEY_LEFT_SUPER, "left_super", "Left Super"},
  key_name_entry{GLFW_KEY_LEFT_ALT, "left_alt", "Left Alt"},
  key_name_entry{GLFW_KEY_SPACE, "space", "Space"},
  key_name_entry{GLFW_KEY_RIGHT_ALT, "right_alt", "Right Alt"},
  key_name_entry{GLFW_KEY_RIGHT_SUPER, "right_super", "Right Super"},
  key_name_entry{GLFW_KEY_MENU, "menu", "Menu"},
  key_name_entry{GLFW_KEY_RIGHT_CONTROL, "right_control", "Right Ctrl"},

  key_name_entry{GLFW_KEY_PRINT_SCREEN, "print_screen", "Print Screen"},
  key_name_entry{GLFW_KEY_SCROLL_LOCK, "scroll_lock", "Scroll Lock"},
  key_name_entry{GLFW_KEY_PAUSE, "pause", "Pause"},
  key_name_entry{GLFW_KEY_INSERT, "insert", "Insert"},
  key_name_entry{GLFW_KEY_DELETE, "delete", "Delete"},
  key_name_entry{GLFW_KEY_HOME, "home", "Home"},
  key_name_entry{GLFW_KEY_END, "end", "End"},
  key_name_entry{GLFW_KEY_PAGE_UP, "page_up", "Page Up"},
  key_name_entry{GLFW_KEY_PAGE_DOWN, "page_down", "Page Down"},
  key_name_entry{GLFW_KEY_UP, "up", "Up"},
  key_name_entry{GLFW_KEY_DOWN, "down", "Down"},
  key_name_entry{GLFW_KEY_LEFT, "left", "Left"},
  key_name_entry{GLFW_KEY_RIGHT, "right", "Right"},

  key_name_entry{GLFW_KEY_NUM_LOCK, "num_lock", "Num Lock"},
  key_name_entry{GLFW_KEY_KP_DIVIDE, "kp_divide", "Num /"},
  key_name_entry{GLFW_KEY_KP_MULTIPLY, "kp_multiply", "Num *"},
  key_name_entry{GLFW_KEY_KP_SUBTRACT, "kp_subtract", "Num -"},
  key_name_entry{GLFW_KEY_KP_ADD, "kp_add", "Num +"},
  key_name_entry{GLFW_KEY_KP_ENTER, "kp_enter", "Num Enter"},
  key_name_entry{GLFW_KEY_KP_DECIMAL, "kp_decimal", "Num ."},
  key_name_entry{GLFW_KEY_KP_0, "kp_0", "Num 0"},
  key_name_entry{GLFW_KEY_KP_1, "kp_1", "Num 1"},
  key_name_entry{GLFW_KEY_KP_2, "kp_2", "Num 2"},
  key_name_entry{GLFW_KEY_KP_3, "kp_3", "Num 3"},
  key_name_entry{GLFW_KEY_KP_4, "kp_4", "Num 4"},
  key_name_entry{GLFW_KEY_KP_5, "kp_5", "Num 5"},
  key_name_entry{GLFW_KEY_KP_6, "kp_6", "Num 6"},
  key_name_entry{GLFW_KEY_KP_7, "kp_7", "Num 7"},
  key_name_entry{GLFW_KEY_KP_8, "kp_8", "Num 8"},
  key_name_entry{GLFW_KEY_KP_9, "kp_9", "Num 9"},
};

const key_name_entry* find_key_name_entry(const int32_t scancode) noexcept {
  if (scancode == GLFW_KEY_UNKNOWN) return nullptr;

  for (const auto& entry : key_names) {
    const int32_t entry_scancode = glfwGetKeyScancode(entry.glfw_key);
    if (entry_scancode != GLFW_KEY_UNKNOWN && entry_scancode == scancode) return &entry;
  }

  return nullptr;
}

const key_name_entry* find_glfw_key_name_entry(const int32_t key) noexcept {
  if (key == GLFW_KEY_UNKNOWN) return nullptr;

  for (const auto& entry : key_names) {
    if (entry.glfw_key == key) return &entry;
  }

  return nullptr;
}

const key_name_entry* find_canonical_key_name_entry(const std::string_view &name) noexcept {
  if (name.empty()) return nullptr;

  for (const auto& entry : key_names) {
    if (entry.canonical == name) return &entry;
  }

  return nullptr;
}
}

size_t get_key_name(const int32_t scancode, char* buffer, const size_t max_size) {
  if (buffer == nullptr || max_size == 0) return 0;

#ifdef _WIN32
  const size_t wchar_buf_size = 256;
  wchar_t buf[wchar_buf_size]{};
  const size_t count = GetKeyNameTextW(LONG(scancode) << 16, buf, wchar_buf_size);
  //const auto err = _wcslwr_s(buf, wchar_buf_size); // переводим в нижний регистр, нужно ли?
  return wcstombs(buffer, buf, max_size);
#else
  (void)scancode;
  // On Linux, local layout names currently come from glfwGetKeyName() in
  // get_key_name_local(). A native fallback would need backend-specific state.
  buffer[0] = '\0';
  return 0;
#endif
}

size_t get_key_name(const int32_t scancode, key_name_buffer& buffer) {
  return get_key_name(scancode, buffer, 256);
}

std::string get_key_name(const int32_t scancode) {
  char buf[512] {};
  const size_t count = get_key_name(scancode, buf, 512);
  return std::string(buf, count);
}

std::string_view get_key_name_canonical(const int32_t scancode) noexcept {
  const auto entry = find_key_name_entry(scancode);
  return entry == nullptr ? std::string_view() : entry->canonical;
}

std::string_view get_key_name_us_layout(const int32_t scancode) noexcept {
  const auto entry = find_key_name_entry(scancode);
  return entry == nullptr ? std::string_view() : entry->us_layout;
}

std::string get_key_name_local(const int32_t scancode) {
  const char* glfw_name = glfwGetKeyName(GLFW_KEY_UNKNOWN, scancode);
  if (glfw_name != nullptr) return std::string(glfw_name);

  return get_key_name(scancode);
}

std::string_view get_glfw_key_name_canonical(const int32_t key) noexcept {
  const auto entry = find_glfw_key_name_entry(key);
  return entry == nullptr ? std::string_view() : entry->canonical;
}

std::string_view get_glfw_key_name_us_layout(const int32_t key) noexcept {
  const auto entry = find_glfw_key_name_entry(key);
  return entry == nullptr ? std::string_view() : entry->us_layout;
}

std::string get_glfw_key_name_local(const int32_t key) {
  const char* glfw_name = glfwGetKeyName(key, GLFW_KEY_UNKNOWN);
  if (glfw_name != nullptr) return std::string(glfw_name);

  const int32_t scancode = glfwGetKeyScancode(key);
  return scancode == GLFW_KEY_UNKNOWN ? std::string() : get_key_name(scancode);
}

int32_t get_glfw_key_from_canonical(const std::string_view &name) noexcept {
  const auto entry = find_canonical_key_name_entry(name);
  return entry == nullptr ? GLFW_KEY_UNKNOWN : entry->glfw_key;
}

std::tuple<int32_t, int32_t> get_key_from_canonical(const std::string_view &name) noexcept {
  const auto entry = find_canonical_key_name_entry(name);
  if (entry == nullptr) return std::make_tuple(GLFW_KEY_UNKNOWN, GLFW_KEY_UNKNOWN);

  return std::make_tuple(entry->glfw_key, glfwGetKeyScancode(entry->glfw_key));
}
}
}
