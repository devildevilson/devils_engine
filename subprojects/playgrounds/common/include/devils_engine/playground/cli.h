#ifndef DEVILS_ENGINE_PLAYGROUND_CLI_H
#define DEVILS_ENGINE_PLAYGROUND_CLI_H

#include <array>
#include <charconv>
#include <cstddef>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

// Общая граница CLI лабораторий. Схема аргументов и предметные ограничения принадлежат
// executable; здесь только вывод CLI11 и коды завершения. Возврат вместо exit сохраняет
// обычный lifetime локальных объектов main, в том числе при запросе справки.
namespace devils_engine::playground {

template <typename Number>
bool cli_number(std::string_view text, Number& out) {
  if (text.starts_with('+')) text.remove_prefix(1);
  if (text.empty()) return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc{} && end == text.data() + text.size() &&
    (!std::is_floating_point_v<Number> || std::isfinite(out));
}

inline const CLI::Validator finite_number{[](std::string& text) {
  double number = 0;
  return cli_number(text, number) ? std::string{} : std::string{"expected a finite number"};
}, "FINITE"};

// One delimited argv value, not a greedy list: negative coordinates and exact arity matter.
template <typename Number, size_t Count, typename Predicate = std::nullptr_t>
CLI::Option* add_tuple(CLI::App& app, std::string name, std::array<Number, Count>& values,
                       char delimiter, Predicate valid = nullptr) {
  std::ostringstream defaults;
  for (size_t i = 0; i < Count; ++i) {
    if (i) defaults << delimiter;
    defaults << values[i];
  }
  const auto decode = [delimiter, valid](std::string_view rest, std::array<Number, Count>& out) -> std::string {
    std::array<Number, Count> candidate{};
    for (size_t i = 0; i < Count; ++i) {
      const size_t split = rest.find(delimiter);
      if ((i + 1 < Count) != (split != std::string_view::npos) ||
          !cli_number(rest.substr(0, split), candidate[i]))
        return "expected exactly " + std::to_string(Count) + " numbers separated by '" + delimiter + "'";
      if (split != std::string_view::npos) rest.remove_prefix(split + 1);
    }
    if constexpr (!std::is_same_v<Predicate, std::nullptr_t>) {
      if (!valid(candidate)) return "coordinates are outside the allowed range";
    }
    out = candidate;
    return {};
  };
  return app.add_option_function<std::string>(name, [&values, decode](const std::string& text) {
    decode(text, values); // CLI11 validated the unchanged argument before invoking this callback.
  }, "Delimited coordinates")->check(CLI::Validator{[decode](std::string& text) {
    std::array<Number, Count> unused{};
    return decode(text, unused);
  }, "COORDINATES"})->default_str(defaults.str());
}

inline CLI::Option* add_overrides(CLI::App& app, std::vector<std::string>& values) {
  return app.add_option("--set", values, "Override NAME=VALUE (repeatable)")->expected(1)->take_all()->check(
    CLI::Validator{[](std::string& text) {
      const auto equals = text.find('=');
      double value = 0;
      if (equals == std::string::npos || equals == 0 || !cli_number(std::string_view(text).substr(equals + 1), value))
        return std::string{"expected NAME=finite-number"};
      return std::string{};
    }, "NAME=VALUE"});
}

inline std::vector<std::pair<std::string, double>> numeric_overrides(const std::vector<std::string>& values) {
  std::vector<std::pair<std::string, double>> result;
  for (const auto& text : values) {
    const auto equals = text.find('=');
    double value = 0;
    cli_number(std::string_view(text).substr(equals + 1), value); // validated by CLI11
    result.emplace_back(text.substr(0, equals), value);
  }
  return result;
}

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
