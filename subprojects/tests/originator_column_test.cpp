#include <atomic>
#include <thread>
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
  "  pow(abs(a), 1.25) + 0x10 - 0x01\n"
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
  const auto vm = originator::script_program::compile("vm", source, names, originator::script_program::result_kind::number, {}, {},
    originator::script_backend::virtual_machine);
  const std::vector<originator::field_ref> inputs{{&cells, nullptr, 0}, {&cells, nullptr, 1}, {&cells, nullptr, 2}};
  const std::vector<originator::field_ref> outputs{{&cells, &cells, 3}};
  originator::dispatch_script(*vm, inputs, outputs, originator::parameters{}, 1, 0, count, "column", nullptr);

  const std::vector<std::string> columns = {"a", "b"};
  const std::vector<std::string> scalars = {"c"};
  const auto column = originator::column_program::compile("column", source, columns, scalars);
  CHECK(column->used_columns()[0] == 1);
  std::vector<double> got(count);
  originator::column_workspace workspace;
  for (size_t begin = 0; begin < count; begin += originator::column_program::lanes) {
    const size_t n = std::min(originator::column_program::lanes, count - begin);
    const std::array<const double*, 2> pointers{a.data() + begin, b.data() + begin};
    const std::array<double, 1> constants{c};
    column->run(n, pointers, constants, got.data() + begin, workspace);
  }
  size_t differ = 0;
  for (size_t i = 0; i < count; ++i) differ += std::abs(got[i] - cells.field(3).get(i)) > 1e-12;
  CHECK(differ == 0);
}

// Условие считается как ds с ожидаемым bool: блок и `sequence` накапливают И с единицей «истина».
// Прежде столбцы складывали их с нулём, и `sequence` предиката давал 2 там, где ВМ давала 1, и 0
// там, где первое условие ложно, а ВМ — «истину».
TEST_CASE("column predicates combine blocks and sequences as the ds virtual machine does") {
  const std::vector<field_pair> fields = {{"a", "d1"}, {"b", "d1"}, {"out", "d1"}};
  constexpr size_t n = 64;
  originator::buffer cells("cells", originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells"), n);
  std::vector<double> a(n), b(n);
  for (size_t i = 0; i < n; ++i) {
    a[i] = double(i % 4);
    b[i] = double((i / 4) % 5);
    cells.field(0).set(i, a[i]);
    cells.field(1).set(i, b[i]);
  }
  const std::vector<std::string> names = {"a", "b"};
  const std::vector<originator::field_ref> inputs{{&cells, nullptr, 0}, {&cells, nullptr, 1}};
  const std::vector<originator::field_ref> outputs{{&cells, &cells, 2}};
  for (const std::string_view predicate : {
         std::string_view{"{ sequence = { { condition = a < 2, b < 3 }, { condition = b < 4, a < 2 } } }"},
         std::string_view{"{ a < 3, b > 1 }"},
         std::string_view{"{ select = { { condition = a < 1, b > 2 }, { b < 2, a > 1 } } }"},
         std::string_view{"{ sequence = { { condition = a > 0, b > 1, a < 3 }, { condition = b > 2, a < 3 } } }"}}) {
    CAPTURE(predicate);
    const auto vm = originator::script_program::compile("vm", predicate, names, originator::script_program::result_kind::predicate,
      {}, {}, originator::script_backend::virtual_machine);
    originator::dispatch_script(*vm, inputs, outputs, originator::parameters{}, 1, 0, n, "predicate", nullptr);
    const auto column = originator::column_program::compile("column", predicate, names, {}, true);
    std::vector<double> got(n);
    originator::column_workspace workspace;
    const std::array<const double*, 2> pointers{a.data(), b.data()};
    column->run(n, pointers, {}, got.data(), workspace);
    size_t differ = 0;
    for (size_t i = 0; i < n; ++i) differ += got[i] != cells.field(2).get(i);
    CHECK(differ == 0);
  }
}

TEST_CASE("column program refuses what it cannot compute") {
  const std::vector<std::string> columns = {"a"};
  const std::vector<std::string> none;
  const auto refusal = originator::column_program::try_compile("random", "{ chance < 0.5 }", columns, none);
  REQUIRE_FALSE(refusal.has_value());
  CHECK_FALSE(refusal.error().empty());
  CHECK_THROWS(originator::column_program::compile("unknown", "{ a + nope }", columns, none));
  CHECK_THROWS(originator::column_program::compile("random", "{ chance < 0.5 }", columns, none));
  CHECK_THROWS(originator::column_program::compile("branch", "{ value_or = { a > 0, { ctx_save = { q = 1 }, ctx:saved:q }, 2 } }",
                                                   columns, none));
}

TEST_CASE("column parser joins top level rows with the same semantics as a block") {
  const std::vector<std::string> columns{"a"};
  const std::vector<std::string> scalars;
  auto program = originator::column_program::compile("multiline", "ctx_save = { v = a * 2 }\nctx:saved:v + 3\n", columns, scalars);
  const double values[]{1, 4, -2};
  const double* inputs[]{values};
  double output[3]{};
  originator::column_workspace workspace;
  program->run(3, inputs, {}, output, workspace);
  CHECK(output[0] == 5);
  CHECK(output[1] == 11);
  CHECK(output[2] == -1);
}

TEST_CASE("caller column workspaces are reusable concurrent and explicitly releasable") {
  auto program = originator::column_program::compile("workspace", "a * 2 + 1", std::vector<std::string>{"a"}, {});
  std::array<originator::column_workspace, 8> contexts;
  std::array<std::thread, 8> workers;
  std::atomic<int> wrong = 0;
  for (size_t i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
    const double input[]{double(i), double(i + 1)};
    const double* columns[]{input};
    double output[2]{};
    for (size_t n = 0; n < 200; ++n) {
      program->run(2, columns, {}, output, contexts[i]);
      if (output[0] != 2 * i + 1 || output[1] != 2 * (i + 1) + 1) ++wrong;
    }
  });
  for (auto& worker : workers) worker.join();
  CHECK(wrong == 0);
  for (auto& context : contexts) {
    CHECK(context.retained_bytes() > 0);
    context.release();
    CHECK(context.retained_bytes() == 0);
  }
}
