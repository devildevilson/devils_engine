#ifndef DEVILS_ENGINE_PLAYGROUND_CLI_H
#define DEVILS_ENGINE_PLAYGROUND_CLI_H

#include <optional>

#include <CLI/CLI.hpp>

// Общая граница CLI лабораторий. Схема аргументов и предметные ограничения принадлежат
// executable; здесь только вывод CLI11 и коды завершения. Возврат вместо exit сохраняет
// обычный lifetime локальных объектов main, в том числе при запросе справки.
namespace devils_engine::playground {

inline std::optional<int> parse_cli(CLI::App& app, const int argc, const char* const* argv) {
  try {
    app.parse(argc, argv);
    return std::nullopt;
  } catch (const CLI::ParseError& error) {
    app.exit(error);
    return error.get_exit_code() == 0 ? 0 : 2;
  }
}

} // namespace devils_engine::playground

#endif
