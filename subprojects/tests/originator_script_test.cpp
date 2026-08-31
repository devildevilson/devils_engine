#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/script_program.h"
#include "devils_engine/thread/atomic_pool.h"

using namespace devils_engine;

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t count = 4096;

originator::buffer make_cells() {
  const std::vector<field_pair> fields = {
    {"height", "v1"},
    {"moisture", "v1"},
    {"biome", "ub1"},
    {"scripted", "v1"},
  };
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout), count);

  auto height = cells.field(cells.find_field("height"));
  auto moisture = cells.field(cells.find_field("moisture"));
  for (size_t i = 0; i < count; ++i) {
    height.set(i, double(i % 100) / 100.0);
    moisture.set(i, double(i % 37) / 37.0);
  }
  return cells;
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}
} // namespace

TEST_CASE("originator script reads buffer fields by their declared names") {
  auto cells = make_cells();

  const std::vector<std::string> names = {"height", "moisture"};
  const auto program = originator::script_program::compile("mix", "height * 2 + moisture", names,
                                                           originator::script_program::result_kind::number);

  CHECK(program->input_count() == 2);
  // Апертура не выбрана автором, а следует из того, что зарегистрировано.
  CHECK(program->shape() == originator::aperture::pointwise);

  const std::vector<originator::field_ref> inputs{readable(cells, "height"), readable(cells, "moisture")};
  const std::vector<originator::field_ref> outputs{writable(cells, "scripted")};

  const originator::parameters no_params;
  originator::dispatch_script(*program, inputs, outputs, no_params, 1, 0, count, "test", nullptr);

  const auto height = cells.field(cells.find_field("height"));
  const auto moisture = cells.field(cells.find_field("moisture"));
  const auto scripted = cells.field(cells.find_field("scripted"));

  for (size_t i = 0; i < count; i += 71) {
    CHECK(scripted.get(i) == doctest::Approx(height.get(i) * 2.0 + moisture.get(i)));
  }
}

TEST_CASE("originator script classifies with the same rule as the native tool") {
  auto cells = make_cells();

  originator::tool_registry tools;
  tools.add_standard_tools();

  originator::parameters params;
  params.set_number("sea_level", 0.5);
  params.set_number("dry", 0.35);
  params.set_number("wet", 0.65);

  const std::vector<originator::field_ref> inputs{readable(cells, "height"), readable(cells, "moisture")};
  const std::vector<originator::field_ref> native_out{writable(cells, "biome")};
  originator::dispatch(*tools.find("classify"), inputs, native_out, params, 1, 0, count, "native", nullptr);

  const std::vector<std::string> names = {"height", "moisture"};
  // Инфиксное сравнение в `condition` у select эта версия devils_script не принимает (там ждут
  // строчную форму и ищут ФУНКЦИЮ '<'), а value_or принимает его как обычное булево выражение.
  const auto program = originator::script_program::compile("biome", R"(
    { value_or = { height < 0.5, 0,
      value_or = { moisture < 0.35, 1,
      value_or = { moisture < 0.65, 2, 3 } } } }
  )", names, originator::script_program::result_kind::number);

  const std::vector<originator::field_ref> script_out{writable(cells, "scripted")};
  originator::dispatch_script(*program, inputs, script_out, params, 1, 0, count, "script", nullptr);

  const auto native = cells.field(cells.find_field("biome"));
  const auto scripted = cells.field(cells.find_field("scripted"));
  for (size_t i = 0; i < count; ++i) {
    CHECK(native.get(i) == scripted.get(i));
  }
}

TEST_CASE("originator script predicate writes one or zero") {
  auto cells = make_cells();

  const std::vector<std::string> names = {"height"};
  const auto program = originator::script_program::compile("is_land", "height > 0.5", names,
                                                           originator::script_program::result_kind::predicate);

  const std::vector<originator::field_ref> inputs{readable(cells, "height")};
  const std::vector<originator::field_ref> outputs{writable(cells, "scripted")};
  const originator::parameters no_params;
  originator::dispatch_script(*program, inputs, outputs, no_params, 1, 0, count, "test", nullptr);

  const auto height = cells.field(cells.find_field("height"));
  const auto scripted = cells.field(cells.find_field("scripted"));
  for (size_t i = 0; i < count; i += 13) {
    CHECK(scripted.get(i) == (height.get(i) > 0.5 ? 1.0 : 0.0));
  }
}

TEST_CASE("originator script gives the same bytes at any number of threads") {
  const std::vector<std::string> names = {"height", "moisture"};
  const auto program = originator::script_program::compile("mix", "height * 3 - moisture", names,
                                                           originator::script_program::result_kind::number);

  auto reference = make_cells();
  {
    const std::vector<originator::field_ref> inputs{readable(reference, "height"), readable(reference, "moisture")};
    const std::vector<originator::field_ref> outputs{writable(reference, "scripted")};
    const originator::parameters no_params;
    originator::dispatch_script(*program, inputs, outputs, no_params, 99, 0, count, "test", nullptr);
  }
  const auto expected = reference.field(reference.find_field("scripted"));

  for (const size_t threads : {size_t(1), size_t(3), size_t(7)}) {
    thread::atomic_pool pool(threads);
    auto cells = make_cells();
    const std::vector<originator::field_ref> inputs{readable(cells, "height"), readable(cells, "moisture")};
    const std::vector<originator::field_ref> outputs{writable(cells, "scripted")};
    const originator::parameters no_params;
    originator::dispatch_script(*program, inputs, outputs, no_params, 99, 0, count, "test", &pool);

    const auto actual = cells.field(cells.find_field("scripted"));
    bool identical = true;
    for (size_t i = 0; i < count; ++i) {
      identical = identical && actual.get(i) == expected.get(i);
    }
    CHECK(identical);
  }
}

TEST_CASE("originator script refuses bindings it was not compiled against") {
  auto cells = make_cells();

  const std::vector<std::string> names = {"height", "moisture"};
  const auto program = originator::script_program::compile("mix", "height + moisture", names,
                                                           originator::script_program::result_kind::number);

  const std::vector<originator::field_ref> outputs{writable(cells, "scripted")};

  // Порядок привязок перепутан: скрипт считал бы одно, а получал другое.
  const std::vector<originator::field_ref> swapped{readable(cells, "moisture"), readable(cells, "height")};
  const originator::parameters no_params;
  CHECK_THROWS_AS(originator::dispatch_script(*program, swapped, outputs, no_params, 1, 0, count, "test", nullptr),
                  std::runtime_error);

  // Не то число входов.
  const std::vector<originator::field_ref> short_list{readable(cells, "height")};
  CHECK_THROWS_AS(originator::dispatch_script(*program, short_list, outputs, no_params, 1, 0, count, "test", nullptr),
                  std::runtime_error);

  // Выход привязан на чтение.
  const std::vector<originator::field_ref> inputs{readable(cells, "height"), readable(cells, "moisture")};
  const std::vector<originator::field_ref> read_only{readable(cells, "scripted")};
  CHECK_THROWS_AS(originator::dispatch_script(*program, inputs, read_only, no_params, 1, 0, count, "test", nullptr),
                  std::runtime_error);

  // Два входа с одинаковым именем неразличимы внутри скрипта.
  const std::vector<std::string> duplicated = {"height", "height"};
  CHECK_THROWS_AS(originator::script_program::compile("bad", "height", duplicated,
                                                      originator::script_program::result_kind::number),
                  std::runtime_error);
}

TEST_CASE("originator script reads its thresholds from step parameters") {
  auto cells = make_cells();

  const std::vector<std::string> names = {"height", "moisture"};
  // Правило живёт в скрипте, числа — в конфиге. Скрипт не хардкодит пороги, которые автор хочет крутить.
  const auto program = originator::script_program::compile("biome", R"(
    { value_or = { height < ctx:arg:sea_level, 0,
      value_or = { moisture < ctx:arg:dry, 1,
      value_or = { moisture < ctx:arg:wet, 2, 3 } } } }
  )", names, originator::script_program::result_kind::number);

  REQUIRE(program->argument_names().size() == 3);

  const std::vector<originator::field_ref> inputs{readable(cells, "height"), readable(cells, "moisture")};
  const std::vector<originator::field_ref> outputs{writable(cells, "scripted")};

  originator::parameters params;
  params.set_number("sea_level", 0.5);
  params.set_number("dry", 0.35);
  params.set_number("wet", 0.65);

  originator::dispatch_script(*program, inputs, outputs, params, 1, 0, count, "climate", nullptr);

  const auto height = cells.field(cells.find_field("height"));
  const auto moisture = cells.field(cells.find_field("moisture"));
  const auto biome = cells.field(cells.find_field("scripted"));

  const auto expected = [](const double h, const double m) {
    if (h < 0.5) return 0.0;
    if (m < 0.35) return 1.0;
    if (m < 0.65) return 2.0;
    return 3.0;
  };
  for (size_t i = 0; i < count; ++i) {
    CHECK(biome.get(i) == expected(height.get(i), moisture.get(i)));
  }

  // Другие пороги — другой результат, при том же скрипте и том же буфере.
  originator::parameters raised;
  raised.set_number("sea_level", 2.0);
  raised.set_number("dry", 0.35);
  raised.set_number("wet", 0.65);
  originator::dispatch_script(*program, inputs, outputs, raised, 1, 0, count, "climate", nullptr);
  for (size_t i = 0; i < count; i += 97) {
    CHECK(biome.get(i) == 0.0); // порог выше любой высоты => всё вода
  }

  // Забытый параметр — громкая ошибка с именем аргумента, а не молчаливый нуль.
  originator::parameters incomplete;
  incomplete.set_number("sea_level", 0.5);
  CHECK_THROWS_AS(originator::dispatch_script(*program, inputs, outputs, incomplete, 1, 0, count, "climate", nullptr),
                  std::runtime_error);
}
