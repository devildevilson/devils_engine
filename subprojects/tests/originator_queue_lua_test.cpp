#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/script_host.h"

using namespace devils_engine;

// Lua-поверхность очереди вычислений. Проверяется здесь и то, ради чего этот файл вообще появился:
// САМО СОЗДАНИЕ И РАЗРУШЕНИЕ ХОСТА. Регистрация неймспейса `originator.queue` однажды уже портила
// память — две лямбды с одинаковым списком параметров в одной функции получали у sol2 общий `__gc`
// (см. комментарий в script_host.cpp), и падало это при закрытии стейта lua, то есть далеко от
// причины. Такую ошибку по результату генерации не видно, значит её обязан ловить тест.

namespace {
constexpr std::string_view buffers_config = R"(
{
  name = cells
  format = [ height = v1, smoothed = v1, moisture = v1, biome = ub1 ]
  layout = soa
  size = cell_count
}
)";

constexpr size_t grid_width = 32;
constexpr size_t grid_count = grid_width * grid_width;

originator::size_table make_sizes() {
  originator::size_table sizes;
  sizes.set("cell_count", grid_count);
  return sizes;
}

originator::pipeline_description make_description(const std::string_view& step_name) {
  originator::pipeline_description description;
  description.name = "queue_lua";
  description.buffers = originator::parse_buffers(buffers_config, "buffers");

  originator::step_description step;
  step.name.assign(step_name);
  step.body = "gen/body";
  step.writes.push_back("cells");
  step.params.set_number("width", double(grid_width));
  step.params.set_number("radius", 2);
  step.params.set_number("sea_level", 0.5);
  step.params.set_number("dry", 0.35);
  step.params.set_number("wet", 0.65);
  description.steps.push_back(std::move(step));
  return description;
}

// Прогоняет одно тело шага и возвращает буфер, чтобы результат можно было сравнить с эталоном.
void run_body(originator::script_host& host, const std::string_view& source, originator::pipeline& p) {
  host.load_body("queued", source, "test/body");
  p.run(host.invoker());
}

double biome_at(originator::pipeline& p, const size_t index) {
  auto* cells = p.find_buffer("cells");
  return cells->field(cells->find_field("biome")).get(index);
}

// Одно и то же поле, посчитанное вне очереди: с ним сверяется результат очереди.
constexpr std::string_view separate_body = R"lua(
return function(step)
  local cells = step.writes.cells
  local height = cells:field("height")
  local smoothed = cells:field("smoothed")
  local moisture = cells:field("moisture")
  local biome = cells:field("biome")

  originator.value_noise{ outputs = { height }, params = { width = step.params.width, frequency = 0.05 } }
  originator.value_noise{ outputs = { moisture }, params = { width = step.params.width, frequency = 0.09 } }

  originator.box_blur{
    inputs = { height },
    outputs = { smoothed },
    params = { width = step.params.width, radius = step.params.radius },
  }
  originator.classify{
    inputs = { smoothed, moisture },
    outputs = { biome },
    params = { sea_level = step.params.sea_level, dry = step.params.dry, wet = step.params.wet },
  }
end
)lua";

constexpr std::string_view queued_body = R"lua(
return function(step)
  local cells = step.writes.cells
  local height = cells:field("height")
  local smoothed = cells:field("smoothed")
  local moisture = cells:field("moisture")
  local biome = cells:field("biome")

  originator.value_noise{ outputs = { height }, params = { width = step.params.width, frequency = 0.05 } }
  originator.value_noise{ outputs = { moisture }, params = { width = step.params.width, frequency = 0.09 } }

  local report = originator.queue{
    originator.queue.box_blur{
      inputs = { height },
      outputs = { smoothed },
      params = { width = step.params.width, radius = step.params.radius },
    },
    originator.queue.classify{
      inputs = { smoothed, moisture },
      outputs = { biome },
      params = { sea_level = step.params.sea_level, dry = step.params.dry, wet = step.params.wet },
    },
    output = { smoothed, biome },
  }

  assert(report.calls == 2, "the queue reports what it declared")
  assert(report.passes == 2, "one pass per call until fusion lands")
end
)lua";
} // namespace

TEST_CASE("originator script host builds and closes with the queue namespace registered") {
  originator::tool_registry tools;
  tools.add_standard_tools();

  // Разрушение хоста — часть проверки: порча памяти при регистрации функций всплывала именно здесь.
  for (int repeat = 0; repeat < 3; ++repeat) {
    originator::script_host host(tools, nullptr);
    const sol::object queue = host.env()["originator"]["queue"];
    CHECK(queue.get_type() == sol::type::table);

    // Неймспейс это ТАБЛИЦА, которую можно ВЫЗВАТЬ: одно имя, два действия.
    const sol::table table = queue.as<sol::table>();
    CHECK(table["box_blur"].get_type() == sol::type::function);
    CHECK(table["run_script"].get_type() == sol::type::function);
  }
}

TEST_CASE("originator queue in lua computes what the same calls compute one by one") {
  originator::tool_registry tools;
  tools.add_standard_tools();

  originator::script_host queued_host(tools, nullptr);
  originator::pipeline queued(make_description("queued"), make_sizes(), 4242);
  run_body(queued_host, queued_body, queued);

  originator::script_host separate_host(tools, nullptr);
  originator::pipeline separate(make_description("queued"), make_sizes(), 4242);
  run_body(separate_host, separate_body, separate);

  size_t differences = 0;
  for (size_t i = 0; i < grid_count; ++i) {
    differences += size_t(biome_at(queued, i) != biome_at(separate, i));
  }
  CHECK(differences == 0);
}

TEST_CASE("originator queue refuses in lua before it computes anything") {
  originator::tool_registry tools;
  tools.add_standard_tools();

  const auto fails = [&](const std::string_view& body) {
    originator::script_host host(tools, nullptr);
    originator::pipeline p(make_description("queued"), make_sizes(), 1);
    host.load_body("queued", body, "test/body");
    CHECK_THROWS_AS(p.run(host.invoker()), std::runtime_error);
  };

  SUBCASE("a declared call that never reaches a queue") {
    // Самая тихая из ошибок: тело написало вызов, ничего не посчиталось, и по результату этого не
    // видно. Поэтому объявления считаются, и расхождение проваливает шаг.
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue.remap{
          inputs = { cells:field("height") },
          outputs = { cells:field("smoothed") },
          params = { scale = 2.0 },
        }
      end
    )lua");
  }

  SUBCASE("the same declared call handed to a queue twice") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        local call = originator.queue.remap{
          inputs = { cells:field("height") },
          outputs = { cells:field("smoothed") },
          params = { scale = 2.0 },
        }
        originator.queue{ call, call, output = { cells:field("smoothed") } }
      end
    )lua");
  }

  SUBCASE("an immediate call passed as a queue element") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 2.0 },
          },
          output = { cells:field("smoothed") },
        }
      end
    )lua");
  }

  SUBCASE("a hole in the element list") {
    // `#` на таблице с дыркой не определён, поэтому по нему элемент мог бы просто исчезнуть, а
    // очередь на один проход короче отличается от правильной только результатом.
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        local first = originator.queue.remap{
          inputs = { cells:field("height") },
          outputs = { cells:field("smoothed") },
          params = { scale = 2.0 },
        }
        local second = originator.queue.remap{
          inputs = { cells:field("smoothed") },
          outputs = { cells:field("moisture") },
          params = { scale = 2.0 },
        }
        originator.queue{ first, nil, second, output = { cells:field("moisture") } }
      end
    )lua");
  }

  SUBCASE("a plain value in the element list") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 2.0 },
          },
          42,
          output = { cells:field("smoothed") },
        }
      end
    )lua");
  }

  SUBCASE("a misspelled key instead of output") {
    // `outputs` вместо `output` иначе означал бы очередь без объявленной границы, и жаловалась бы
    // она не на опечатку, а на отсутствие выхода.
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 2.0 },
          },
          outputs = { cells:field("smoothed") },
        }
      end
    )lua");
  }

  SUBCASE("a reduction declared inside the queue") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.reduce_max{ inputs = { cells:field("height") } },
          output = { cells:field("smoothed") },
        }
      end
    )lua");
  }

  SUBCASE("a scatter declared inside the queue") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue.group_by{
          inputs = { cells:field("biome") },
          outputs = { cells:field("smoothed"), cells:field("moisture") },
        }
      end
    )lua");
  }

  SUBCASE("a queue without a declared output") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 2.0 },
          },
        }
      end
    )lua");
  }

  SUBCASE("a pass nothing reads and output does not name") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("moisture") },
            params = { scale = 2.0 },
          },
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 3.0 },
          },
          output = { cells:field("smoothed") },
        }
      end
    )lua");
  }
}
