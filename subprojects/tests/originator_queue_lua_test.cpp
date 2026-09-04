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
  extent = [ grid_width, grid_width ]
}
// Состояние между шагами — обычный буфер на один элемент. Он же служит СЧЁТЧИКОМ: столько элементов
// обработает следующий вызов, и в очереди за этим числом возвращаться в lua не надо.
{
  name = state
  format = [ used = ui1 ]
  layout = soa
  size = single
}
)";

constexpr size_t grid_width = 32;
constexpr size_t grid_count = grid_width * grid_width;

originator::size_table make_sizes() {
  originator::size_table sizes;
  sizes.set("grid_width", grid_width);
  sizes.set("single", 1);
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
  step.writes.push_back("state");
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

  originator.value_noise{ outputs = { height }, params = { frequency = 0.05 } }
  originator.value_noise{ outputs = { moisture }, params = { frequency = 0.09 } }

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

  originator.value_noise{ outputs = { height }, params = { frequency = 0.05 } }
  originator.value_noise{ outputs = { moisture }, params = { frequency = 0.09 } }

  local report = originator.queue{
    originator.queue.box_blur{
      inputs = { height },
      outputs = { smoothed },
      params = { radius = step.params.radius },
    },
    originator.queue.classify{
      inputs = { smoothed, moisture },
      outputs = { biome },
      params = { sea_level = step.params.sea_level, dry = step.params.dry, wet = step.params.wet },
    },
    output = { smoothed, biome },
  }

  assert(report.calls == 2, "the queue reports what it declared")
  -- box_blur это gather: ему нужен весь предыдущий проход, поэтому группа слияния на нём рвётся.
  assert(report.passes == 2, "a gather breaks the fusion group")
  assert(report.fused == 0, "nothing to fuse in this pair")
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

TEST_CASE("originator queue in lua counts elements from a field") {
  originator::tool_registry tools;
  tools.add_standard_tools();

  // Столько элементов, сколько скажет буфер. Возврата в дирижёра за этим числом нет — именно это и
  // удлиняет очередь, а короткая очередь бессмысленна.
  static constexpr std::string_view counted_body = R"lua(
    return function(step)
      local cells = step.writes.cells
      local state = step.writes.state
      local used = state:field("used")
      local height = cells:field("height")
      local smoothed = cells:field("smoothed")
      local moisture = cells:field("moisture")

      originator.value_noise{ outputs = { height }, params = { frequency = 0.05 } }
      for i = 0, cells:count() - 1 do smoothed:set(i, -1.0) end
      used:set(0, 17)

      local report = originator.queue{
        originator.queue.remap{
          inputs = { height },
          outputs = { smoothed },
          params = { scale = 2.0 },
          range = { count = used },
        },
        originator.queue.remap{
          inputs = { smoothed },
          outputs = { moisture },
          params = { scale = 1.0 },
          range = { count = used },
        },
        output = { smoothed, moisture },
      }

      assert(report.clamped == 0, "17 fits the buffer")
      -- Оба вызова смотрят в ОДИН счётчик, поэтому равенство диапазонов держится по построению.
      assert(report.fused == 2, "one counter, one group")
      assert(report.passes == 1, "one traversal")

      for i = 0, 16 do
        assert(smoothed:get(i) == height:get(i) * 2.0, "counted element " .. i)
      end
      assert(smoothed:get(17) == -1.0, "element past the count is untouched")
    end
  )lua";

  originator::script_host host(tools, nullptr);
  originator::pipeline p(make_description("queued"), make_sizes(), 5);
  run_body(host, counted_body, p);
}

TEST_CASE("originator queue in lua reports a clamped count") {
  originator::tool_registry tools;
  tools.add_standard_tools();

  static constexpr std::string_view clamped_body = R"lua(
    return function(step)
      local cells = step.writes.cells
      local used = step.writes.state:field("used")
      used:set(0, cells:count() * 4)

      local report = originator.queue{
        originator.queue.remap{
          inputs = { cells:field("height") },
          outputs = { cells:field("smoothed") },
          params = { scale = 2.0 },
          range = { count = used },
        },
        output = { cells:field("smoothed") },
      }

      -- Отказать нельзя: на устройстве бросить нечем, и два поведения у одного объявления были бы
      -- хуже. Поэтому зажим, а факт зажима обязан прочитать ХОСТ — то есть вот это тело.
      assert(report.clamped == 1, "the queue says it hit the cap")
    end
  )lua";

  originator::script_host host(tools, nullptr);
  originator::pipeline p(make_description("queued"), make_sizes(), 5);
  run_body(host, clamped_body, p);
}

TEST_CASE("originator queue in lua refuses a count it cannot trust") {
  originator::tool_registry tools;
  tools.add_standard_tools();

  const auto fails = [&](const std::string_view& body) {
    originator::script_host host(tools, nullptr);
    originator::pipeline p(make_description("queued"), make_sizes(), 1);
    host.load_body("queued", body, "test/body");
    CHECK_THROWS_AS(p.run(host.invoker()), std::runtime_error);
  };

  SUBCASE("a plain number where a counter belongs") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 2.0 },
            range = { count = 17 },
          },
          output = { cells:field("smoothed") },
        }
      end
    )lua");
  }

  SUBCASE("a fractional field as the counter") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 2.0 },
            range = { count = cells:field("height") },
          },
          output = { cells:field("smoothed") },
        }
      end
    )lua");
  }

  SUBCASE("a counter next to explicit bounds") {
    fails(R"lua(
      return function(step)
        local cells = step.writes.cells
        originator.queue{
          originator.queue.remap{
            inputs = { cells:field("height") },
            outputs = { cells:field("smoothed") },
            params = { scale = 2.0 },
            range = { 0, 10, count = step.writes.state:field("used") },
          },
          output = { cells:field("smoothed") },
        }
      end
    )lua");
  }
}
