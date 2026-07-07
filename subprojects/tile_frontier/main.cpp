#include <clocale>
#include <locale>

#ifdef _WIN32
#include <windows.h>
#endif

#include "simulation.h"

using namespace tile_frontier;

int main() {
  // Полная независимость от системной локали (Windows и Linux одинаково).
  // UTF-8 — это БАЙТОВАЯ кодировка, а не локаль: исходники в UTF-8, и пока поток не делает
  // локале-зависимой codecvt-конвертации, байты литералов уходят на выход как есть.
  // Поэтому ставим классическую "C" локаль (она ничего не конвертирует) и НЕ imbue'им
  // именованную локаль вроде ".UTF8" — это Windows-only имя, на Linux std::locale(".UTF8")
  // бросает std::runtime_error.
  std::setlocale(LC_ALL, "C");              // C-локаль (printf и пр.)
  std::locale::global(std::locale::classic()); // C++-локаль (iostreams)

#ifdef _WIN32
  // Консоль Windows по умолчанию в OEM/ANSI-кодировке — переключаем ввод/вывод на UTF-8,
  // чтобы UTF-8-байты рендерились корректно. Сами потоки локалью не трогаем.
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  core::simulation s;
  s.init();
  s.run(0);
  return 0;
}
