#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
// Без LEAN_AND_MEAN windows.h тянет objbase.h с макросом `interface`, а он ломает simul::interface.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
// minwindef.h определяет IN/OUT (параметры шаблона devils_script::subblock) и near/far.
#  undef IN
#  undef OUT
#  undef near
#  undef far
#endif

#include <devils_engine/simul/launch_arguments.h>

#include "core/launch_options.h"
#include "core/runtime.h"

using namespace frontier_online;

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

  // Аргументы проверяются здесь, до старта: ошибка в них — сообщение и выход, а не критическая
  // ошибка изнутри запуска. В bootstrap они попадут через app_runtime::run.
  devils_engine::simul::launch_arguments arguments(argc, argv);
  {
    core::launch_options probe;
    std::string problem;
    if (!core::parse_launch_options(arguments, probe, problem)) {
      std::fprintf(stderr, "frontier_online: %s\n", problem.c_str());
      return EXIT_FAILURE;
    }
  }

  core::runtime app;
  return app.run(std::move(arguments));
}
