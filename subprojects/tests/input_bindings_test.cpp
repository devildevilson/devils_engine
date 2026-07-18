// input key-mapping как данные: apply/collect bindings_config, перепривязка целиком, отвязка,
// scancode_N-fallback и tavl-roundtrip. Клавиатурные canonical-имена требуют живого GLFW
// (glfwGetKeyScancode), поэтому здесь только мышь (синтетические сканкоды) и raw-сканкоды —
// клавиатурный путь покрывается вживую через bind_default_actions + settings.input.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <devils_engine/input/bindings.h>
#include <devils_engine/input/events.h>
#include <tavl/deserialize.h>
#include <tavl/serialize.h>

using namespace devils_engine;
using input::events;

TEST_CASE("binding names round-trip without GLFW") {
  // мышь: имя → синтетический сканкод → имя
  {
    const auto [glfw_key, scancode] = input::binding_key_from_name("mouse_left");
    CHECK(glfw_key == -1);
    CHECK(scancode == events::mouse_button_scancode(0));
    CHECK(input::binding_name_from_scancode(scancode) == "mouse_left");
  }
  // raw-fallback: scancode_N парсится и печатается без потерь
  {
    const auto [glfw_key, scancode] = input::binding_key_from_name("scancode_777");
    CHECK(glfw_key == -1);
    CHECK(scancode == 777);
    CHECK(input::binding_name_from_scancode(777) == "scancode_777");
  }
  // мусор не распознаётся
  {
    const auto [glfw_key, scancode] = input::binding_key_from_name("no_such_key");
    CHECK(glfw_key == -1);
    CHECK(scancode == -1);
    const auto [g2, s2] = input::binding_key_from_name("scancode_oops");
    CHECK(s2 == -1);
    static_cast<void>(g2);
  }
  CHECK(input::binding_name_from_scancode(-1).empty());
}

TEST_CASE("apply/collect bindings against live events") {
  input::bindings_config cfg;
  cfg.actions["attack"] = {"mouse_left", "mouse_right"};
  cfg.actions["exotic"] = {"scancode_777"};
  cfg.actions["typo"] = {"no_such_key"}; // warn + пропуск: событие не создаётся
  input::apply_bindings(cfg);
  CHECK(events::has_bindings());

  // привязка живая: нажатие кнопки мыши видно через событие
  events::update_mouse_button(0, 1);
  CHECK(events::is_pressed(std::string_view("attack")));
  events::update_mouse_button(0, 0);
  events::update_mouse_button(1, 1);
  CHECK(events::is_pressed(std::string_view("attack")));
  events::update_mouse_button(1, 0);
  CHECK(events::is_released(std::string_view("attack")));

  // выгрузка эффективной карты: имена восстановились, typo не попал
  const auto live = input::collect_bindings();
  CHECK(live.actions.at("attack") == std::vector<std::string>{"mouse_left", "mouse_right"});
  CHECK(live.actions.at("exotic") == std::vector<std::string>{"scancode_777"});
  CHECK(live.actions.find("typo") == live.actions.end());

  // перепривязка целиком: хвостовой слот чистится, старая кнопка больше не двигает событие
  input::bindings_config narrower;
  narrower.actions["attack"] = {"mouse_middle"};
  input::apply_bindings(narrower);
  events::update_mouse_button(1, 1); // бывшая right-привязка
  CHECK(events::is_released(std::string_view("attack")));
  events::update_mouse_button(1, 0);
  events::update_mouse_button(2, 1);
  CHECK(events::is_pressed(std::string_view("attack")));
  events::update_mouse_button(2, 0);

  // пустой список = полная отвязка; действие остаётся в выгрузке с пустыми кнопками
  input::bindings_config unbind;
  unbind.actions["attack"] = {};
  input::apply_bindings(unbind);
  events::update_mouse_button(2, 1);
  CHECK(events::is_released(std::string_view("attack")));
  events::update_mouse_button(2, 0);
  CHECK(input::collect_bindings().actions.at("attack").empty());
}

TEST_CASE("bindings_config survives tavl round-trip") {
  input::bindings_config src;
  src.actions["attack"] = {"mouse_left", "mouse_right"};
  src.actions["camera_up"] = {"key_w"}; // имя не резолвится без GLFW, но как строка живёт в конфиге
  src.actions["unbound"] = {};

  std::string data;
  REQUIRE(tavl::serialize(src, data));

  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(data);
  parser.finish();

  input::bindings_config parsed;
  tavl::ct_context ctx;
  tavl::deserialize(parser, ctx, parsed);
  CHECK(ctx.diagnostics.empty());
  CHECK(parsed.actions == src.actions);
}
