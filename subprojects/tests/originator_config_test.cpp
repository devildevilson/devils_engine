#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <doctest/doctest.h>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_path.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_resource.h"

using namespace devils_engine;

namespace {

// Точка входа: ОДИН документ. Шаги лежат внутри `steps`, а не блоками верхнего уровня — рядом со
// строкой `values = ...` блок верхнего уровня перестаёт быть шагом и становится лишним значением
// структуры.
constexpr std::string_view entry_config = R"(
name = planet

values  = ./values
buffers = ./buffers

steps = [
  {
    name = topology
    body = ./scripts/S01_topology.lua
    writes = [ cells ]
  }
  {
    name = climate
    body = ./scripts/S02_climate.lua
    reads = [ cells ]
    writes = [ state ]
    params = { radius = 2 }
    programs = {
      land = ./scripts/S02_land.ds
    }
  }
]
)";

constexpr std::string_view values_config = R"(
numbers = { sea_level = 0.5, features = 5 }
ranges = { sea_level = [0.1, 0.9, 0.05] }
)";

constexpr std::string_view buffers_config = R"(
{
  name = cells
  format = [ height = v1, biome = ub1 ]
  layout = soa
  size = cell_count
}
{
  name = state
  format = [ land_cells = ui1 ]
  size = single
}
)";

struct module_fixture {
  std::filesystem::path root;

  explicit module_fixture(const std::string& name) : root(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "core" / "generator" / "scripts");
  }

  ~module_fixture() {
    std::filesystem::remove_all(root);
  }

  void write(const std::string& relative, const std::string_view& text) const {
    std::ofstream out(root / "core" / relative);
    out << text;
  }

  void write_default_generator() const {
    write("generator/planet.tavl", entry_config);
    write("generator/values.tavl", values_config);
    write("generator/buffers.tavl", buffers_config);
    write("generator/scripts/S01_topology.lua", "return function(step) end\n");
    write("generator/scripts/S02_climate.lua", "return function(step) end\n");
    write("generator/scripts/S02_land.ds", "{ value_or = { height < 0.5, 0, 1 } }\n");
  }

  void parse(demiurg::module_system& modules, demiurg::resource_system& resources) const {
    modules.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
  }
};

} // namespace

TEST_CASE("originator entry document names its parts and carries its steps [originator]") {
  const auto entry = originator::parse_entry(entry_config, "planet");

  CHECK(entry.name == "planet");
  CHECK(entry.values == "./values");
  CHECK(entry.buffers == "./buffers");
  REQUIRE(entry.steps.size() == 2);
  CHECK(entry.steps[0].name == "topology");
  CHECK(entry.steps[0].writes.size() == 1);
  CHECK(entry.steps[1].reads.size() == 1);
  CHECK(entry.steps[1].params.integer("radius") == 2);
  REQUIRE(entry.steps[1].programs.size() == 1);
  CHECK(entry.steps[1].programs[0].first == "land");
}

// Ровно та форма, которая читалась не тем обходом и не давала при этом никакой диагностики: строка
// верхнего уровня рядом с блоками. Отказ обязан быть громким и называть обе формы.
TEST_CASE("originator rejects a steps document that starts with a row of its own [originator]") {
  constexpr std::string_view mixed = R"(
values = values.tavl
buffers = buffers.tavl

{
  name = topology
  body = scripts/S01_topology
  writes = [ cells ]
}
)";

  CHECK_THROWS_AS(originator::parse_steps(mixed, "steps"), std::runtime_error);
  // И обратная путаница: голый список блоков — не точка входа.
  CHECK_THROWS_AS(originator::parse_entry(buffers_config, "entry"), std::runtime_error);
}

TEST_CASE("originator entry demands the buffers document [originator]") {
  constexpr std::string_view no_buffers = R"(
name = planet
values = ./values
steps = [ { name = a, body = ./a, writes = [ cells ] } ]
)";
  CHECK_THROWS_AS(originator::parse_entry(no_buffers, "entry"), std::runtime_error);
}

TEST_CASE("originator loads a generator through demiurg by one entry id [originator][demiurg]") {
  const module_fixture fixture("devils_engine_originator_config_test");
  fixture.write_default_generator();

  demiurg::module_system modules(fixture.root.generic_string() + "/");
  demiurg::resource_system resources;
  fixture.parse(modules, resources);

  const auto config = originator::load_generator(resources, "generator/planet");

  CHECK(config.entry_id == "generator/planet");
  CHECK(config.description.name == "planet");

  // Значения и их диапазоны приехали из документа, названного точкой входа.
  CHECK(config.description.values.number("sea_level") == doctest::Approx(0.5));
  REQUIRE(config.ranges.size() == 1);
  CHECK(config.ranges[0].name == "sea_level");
  CHECK(config.ranges[0].step == doctest::Approx(0.05));

  REQUIRE(config.description.buffers.size() == 2);
  CHECK(config.description.buffers[0].name == "cells");
  CHECK(config.description.buffers[1].size_name == "single");

  // Ссылки переписаны абсолютными id: расширение снято, `./` разрешено от папки точки входа.
  REQUIRE(config.description.steps.size() == 2);
  CHECK(config.description.steps[0].body == "generator/scripts/S01_topology");
  CHECK(config.description.steps[1].programs[0].second == "generator/scripts/S02_land");

  // По этим же id берётся текст — второго написания одного пути в пакете не остаётся.
  CHECK(config.source(config.description.steps[0].body).find("return function") != std::string::npos);
  CHECK(config.source(config.description.steps[1].programs[0].second).find("value_or") != std::string::npos);

  // Значения, буферы и три скрипта — ровно пять текстов, ни одного лишнего чтения (точка входа уже
  // разобрана, её текст в пакете не нужен).
  CHECK(config.sources.size() == 5);
}

TEST_CASE("originator names the referrer when a generator source is missing [originator][demiurg]") {
  const module_fixture fixture("devils_engine_originator_config_missing_test");
  fixture.write_default_generator();
  std::filesystem::remove(fixture.root / "core" / "generator" / "scripts" / "S02_land.ds");

  demiurg::module_system modules(fixture.root.generic_string() + "/");
  demiurg::resource_system resources;
  fixture.parse(modules, resources);

  CHECK_THROWS_AS(originator::load_generator(resources, "generator/planet"), std::runtime_error);
}

// `//---` в файле — это НЕ оформление: demiurg раскладывает такой документ на суб-ресурсы, и id без
// хвоста `:имя` перестаёт существовать. Проверка стоит здесь, потому что ошибка выглядит как опечатка
// в пути, хотя файл лежит на месте.
TEST_CASE("originator entry file must not be split into sub-resources [originator][demiurg]") {
  const module_fixture fixture("devils_engine_originator_config_sections_test");
  fixture.write_default_generator();
  fixture.write("generator/planet.tavl", std::string("//--- planet\n") + std::string(entry_config));

  demiurg::module_system modules(fixture.root.generic_string() + "/");
  demiurg::resource_system resources;
  fixture.parse(modules, resources);

  CHECK(resources.get("generator/planet") == nullptr);
  CHECK(resources.get("generator/planet:planet") != nullptr);
  CHECK_THROWS_AS(originator::load_generator(resources, "generator/planet"), std::runtime_error);
  // Названный полным id суб-ресурс читается как обычно.
  const auto config = originator::load_generator(resources, "generator/planet:planet");
  CHECK(config.description.steps.size() == 2);
}

TEST_CASE("demiurg resource paths drop the extension and resolve dots [demiurg]") {
  CHECK(demiurg::absolute_resource_path("generator/planet", "./values.tavl") == "generator/values");
  CHECK(demiurg::absolute_resource_path("generator/planet", "../shared/noise") == "shared/noise");
  CHECK(demiurg::absolute_resource_path("generator/planet", "generator/values") == "generator/values");
  CHECK(demiurg::absolute_resource_path("generator/planet", "/generator/values.tavl") == "generator/values");
  CHECK(demiurg::absolute_resource_path("generator/planet", "generator/planet:steps") == "generator/planet:steps");
  CHECK(demiurg::absolute_resource_path("generator/planet", "  ") == "");
}
