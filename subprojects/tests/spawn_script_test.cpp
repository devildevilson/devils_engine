#include <string>
#include <string_view>

#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>
#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include "core/spawn_scope.h"

// Примитивный ds-спавн: натив spawn_at(prefab, x, y) над spawn_scope (несёт мутабельный sink).
// Проверяем, что натив регистрируется, парсится и на исполнении зовёт sink с (имя, точка). Мок-sink,
// без слайса/мира. (Спавнеры-энтити, filter/pick, засев spawn_scope в живые скрипты — тех-долг.)

using namespace tile_frontier::core;
namespace ds = devils_script;

namespace {
struct mock_sink : public spawn_sink {
  std::string last_name;
  glm::vec2 last_pos{0.0f, 0.0f};
  std::string last_group;
  int calls = 0;
  devils_engine::aesthetics::entityid_t spawn_prefab(std::string_view name, glm::vec2 pos) override {
    last_name = std::string(name);
    last_pos = pos;
    ++calls;
    return devils_engine::aesthetics::entityid_t(42);
  }
  devils_engine::aesthetics::entityid_t spawn_prefab_at_point(
    std::string_view name, std::string_view point_group) override {
    last_name = std::string(name);
    last_group = std::string(point_group);
    ++calls;
    return devils_engine::aesthetics::entityid_t(43);
  }
};
} // namespace

TEST_CASE("devils_script: spawn_at натив спавнит через sink [prefab]") {
  ds::system sys;
  sys.init_basic_functions();
  sys.init_math();
  sys.register_function<&scope_spawn_at>("spawn_at");

  // prefab — bareword-строка скрипта; x/y — числа. Аргументы в { } (как effect_sum = { 2, 3 }).
  const auto cont = sys.parse<void, spawn_scope>("spawn", "spawn_at = { food, 3, 4 }");

  mock_sink sink;
  ds::context ctx;
  ctx.set_arg(0, spawn_scope{&sink}); // root-скоуп несёт способность спавна
  cont.process(&ctx);

  CHECK(sink.calls == 1);
  CHECK(sink.last_name == "food");
  CHECK(sink.last_pos.x == 3.0f);
  CHECK(sink.last_pos.y == 4.0f);
}

TEST_CASE("devils_script: spawn resolves a semantic point group through sink [prefab]") {
  ds::system sys;
  sys.init_basic_functions();
  sys.init_math();
  sys.register_function<&scope_spawn>("spawn");

  const auto cont = sys.parse<void, spawn_scope>("spawn", "spawn = { food, food }");

  mock_sink sink;
  ds::context ctx;
  ctx.set_arg(0, spawn_scope{&sink});
  cont.process(&ctx);

  CHECK(sink.calls == 1);
  CHECK(sink.last_name == "food");
  CHECK(sink.last_group == "food");
}
