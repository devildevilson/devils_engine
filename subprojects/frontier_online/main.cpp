#include <charconv>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <string>
#include <string_view>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "core/frame_capture.h"
#include "runtime.h"

using namespace frontier_online;

namespace {

// Разбор аргументов клиента. Их ровно два и оба — инструмент разработчика: снять кадр и выйти.
// Неопознанный аргумент отвергается, а не игнорируется: молча запущенный не тот режим потом
// объясняют часами.
bool parse_arguments(const int argc, char** argv) {
  auto& capture = frontier_online::core::capture_request();
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg.starts_with("--dump=")) {
      capture.path = std::string(arg.substr(std::string_view("--dump=").size()));
      if (capture.at_frame == 0) capture.at_frame = 1;
      continue;
    }
    if (arg.starts_with("--frames=")) {
      const auto text = arg.substr(std::string_view("--frames=").size());
      uint64_t value = 0;
      const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0) {
        std::fprintf(stderr, "frontier_online: bad --frames value\n");
        return false;
      }
      capture.at_frame = value;
      continue;
    }
    if (arg == "--stay") { // снять кадр и продолжать жить (смотреть глазами)
      capture.exit_after = false;
      continue;
    }
    std::fprintf(stderr, "frontier_online: unknown argument '%.*s'\n", int(arg.size()), arg.data());
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  // Полная независимость от системной локали (Windows и Linux одинаково).
  // UTF-8 — это БАЙТОВАЯ кодировка, а не локаль: исходники в UTF-8, и пока поток не делает
  // локале-зависимой codecvt-конвертации, байты литералов уходят на выход как есть.
  // Поэтому ставим классическую "C" локаль (она ничего не конвертирует) и НЕ imbue'им
  // именованную локаль вроде ".UTF8" — это Windows-only имя, на Linux std::locale(".UTF8")
  // бросает std::runtime_error.
  std::setlocale(LC_ALL, "C");                 // C-локаль (printf и пр.)
  std::locale::global(std::locale::classic()); // C++-локаль (iostreams)

#ifdef _WIN32
  // Консоль Windows по умолчанию в OEM/ANSI-кодировке — переключаем ввод/вывод на UTF-8,
  // чтобы UTF-8-байты рендерились корректно. Сами потоки локалью не трогаем.
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  if (!parse_arguments(argc, argv)) {
    return EXIT_FAILURE;
  }

  core::runtime app;
  return app.run();
}
