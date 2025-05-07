#include "key_names.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <X11/XKBlib.h>
#endif

namespace devils_engine {
namespace input {
size_t get_key_name(const int32_t scancode, char* buffer, const size_t max_size) {
#ifdef _WIN32
  const size_t wchar_buf_size = 256;
  wchar_t buf[wchar_buf_size]{};
  const size_t count = GetKeyNameTextW(LONG(scancode) << 16, buf, wchar_buf_size);
  //const auto err = _wcslwr_s(buf, wchar_buf_size); // переводим в нижний регистр, нужно ли?
  return wcstombs(buffer, buf, max_size);
#else
  // для линукса должно быть что то вроде вот этого, нужно передать X11 дисплей
  // glfwGetX11Display()
  // XKeysymToString( XkbKeycodeToKeysym(QX11Info::display(), code, 0, 0) );
  // glfwGetWaylandDisplay()
  // для wayland'a : xkb_state_key_get_one_sym xkb_keysym_get_name
  // а может и для иксов тоже годится
  // нужно получить xkb_state
  // смотреть https://git.sr.ht/~sircmpwn/wev
  // Х11 https://gitlab.freedesktop.org/xorg/app/xev
  // для MacOS говорят можно использовать UCKeyTranslate (???)
  // MacOS как обычно самая непонятная штука на свете
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
}
}