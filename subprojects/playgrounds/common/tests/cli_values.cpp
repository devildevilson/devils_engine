#include "devils_engine/playground/cli.h"
#include <cstdint>
#include <iostream>
int main() {
  CLI::App app;
  std::array<int64_t, 3> point{};
  std::vector<std::string> overrides;
  uint64_t seed = 0;
  size_t route = 9;
  devils_engine::playground::add_tuple(app, "--point", point, ':');
  devils_engine::playground::add_overrides(app, overrides);
  app.add_option("--seed", seed);
  app.add_option("--on-route", route)->expected(0,1)->default_str("0");
  const char* args[]{"cli_values", "--point=-9223372036854775808:2:-3", "--seed", "18446744073709551615",
    "--set=a=-0.25", "--set", "b=2e3", "--on-route"};
  if (auto code = devils_engine::playground::parse_cli(app, int(std::size(args)), args)) return *code;
  const auto parsed = devils_engine::playground::numeric_overrides(overrides);
  if (point != std::array<int64_t,3>{INT64_MIN,2,-3} || seed != UINT64_MAX || route != 0 ||
      parsed != std::vector<std::pair<std::string,double>>{{"a",-.25},{"b",2000}}) return 1;
  // A failed tuple must leave the caller's previous coordinates intact.
  CLI::App invalid;
  devils_engine::playground::add_tuple(invalid, "--point", point, ':');
  try { invalid.parse(std::vector<std::string>{"--point=1:2:3:4"}); return 1; }
  catch (const CLI::ParseError&) {}
  if (point != std::array<int64_t,3>{INT64_MIN,2,-3}) return 1;
  std::cout << "CLI values: exact u64, signed coordinates, repeated overrides and optional route passed\n";
}
