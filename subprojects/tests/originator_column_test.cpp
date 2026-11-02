#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/buffer.h"
#include "devils_engine/originator/script_column.h"
#include "devils_engine/originator/script_program.h"

using namespace devils_engine;

// Колоночный исполнитель ds против виртуальной машины ds: одна программа, одни данные, один ответ.
namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t count = 1000;

// Формула, похожая на форму поля: расстояние, ветвление, локаль, блок-сумма и набор функций.
constexpr std::string_view source =
  "{\n"
  "  ctx_save = { d = sqrt((a - c) * (a - c) + b * b) }\n"
  "  ctx_save = { s = smoothstep(0, 4, ctx:saved:d) }\n"
  "  value_or = { ctx:saved:d < c, min(ctx:saved:d - c, ctx:saved:s), max(ctx:saved:d * 0.5, abs(b)) }\n"
  "  select = { { condition = a > 2 and b < 1, clamp(a, 0, 1) }, { floor(b) } }\n"
  "  -1 * ctx:saved:s\n"
  "}";
} // namespace

TEST_CASE("column program computes what the ds virtual machine computes") {
  const std::vector<field_pair> fields = {{"a", "d1"}, {"b", "d1"}, {"c", "d1"}, {"out", "d1"}};
  originator::buffer cells("cells", originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells"), count);
  std::vector<double> a(count), b(count);
  constexpr double c = 1.75;
  for (size_t i = 0; i < count; ++i) {
    a[i] = std::sin(double(i) * 0.37) * 5.0;
    b[i] = std::cos(double(i) * 0.11) * 3.0;
    cells.field(0).set(i, a[i]);
    cells.field(1).set(i, b[i]);
    cells.field(2).set(i, c);
  }
  const std::vector<std::string> names = {"a", "b", "c"};
  const auto vm = originator::script_program::compile("vm", source, names, originator::script_program::result_kind::number);
  const std::vector<originator::field_ref> inputs{{&cells, nullptr, 0}, {&cells, nullptr, 1}, {&cells, nullptr, 2}};
  const std::vector<originator::field_ref> outputs{{&cells, &cells, 3}};
  originator::dispatch_script(*vm, inputs, outputs, originator::parameters{}, 1, 0, count, "column", nullptr);

  const std::vector<std::string> columns = {"a", "b"};
  const std::vector<std::string> scalars = {"c"};
  const auto column = originator::column_program::compile("column", source, columns, scalars);
  CHECK(column->used_columns()[0] == 1);
  std::vector<double> got(count);
  for (size_t begin = 0; begin < count; begin += originator::column_program::lanes) {
    const size_t n = std::min(originator::column_program::lanes, count - begin);
    const std::array<const double*, 2> pointers{a.data() + begin, b.data() + begin};
    const std::array<double, 1> constants{c};
    column->run(n, pointers, constants, got.data() + begin);
  }
  size_t differ = 0;
  for (size_t i = 0; i < count; ++i) differ += std::abs(got[i] - cells.field(3).get(i)) > 1e-12;
  CHECK(differ == 0);
}

TEST_CASE("column program refuses what it cannot compute") {
  const std::vector<std::string> columns = {"a"};
  const std::vector<std::string> none;
  CHECK_THROWS(originator::column_program::compile("unknown", "{ a + nope }", columns, none));
  CHECK_THROWS(originator::column_program::compile("random", "{ chance < 0.5 }", columns, none));
  CHECK_THROWS(originator::column_program::compile("branch", "{ value_or = { a > 0, { ctx_save = { q = 1 }, ctx:saved:q }, 2 } }",
                                                   columns, none));
}
