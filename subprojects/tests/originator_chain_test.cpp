#include <array>
#include <atomic>
#include <mutex>
#include "devils_engine/thread/atomic_pool.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/script_shape.h"
#include "devils_engine/originator/value_noise.h"

using namespace devils_engine;

// Цепочка из трёх генераторов:
//   grid    нативный, ключ (x, z): высота клетки, её адрес и список соседей «слева и сверху»;
//   tile    lua, ключ (x, z): высота своей клетки плюс суммы соседних плиток — fan-in на СЕБЯ, рекурсия
//           сходится по данным (соседи только с меньшими координатами);
//   column  lua, ключ (x): плитка (x, x) проекцией своего ключа и клетка по адресу, который плитка
//           записала в свой выход — ключ из данных.
namespace {

constexpr const char* grid_buffers = R"(
{ name = cells, size = one, format = [ at = ul2, height = v1 ] }
{ name = around, size = around_capacity, format = [ key = ul2 ] }
{ name = meta, size = one, format = [ count = ui1 ] }
)";

constexpr const char* tile_buffers = R"(
{ name = sum, size = one, format = [ value = v1 ] }
{ name = tag, size = one, format = [ at = ul2 ] }
)";

constexpr const char* tile_body = R"(
return function(step)
  local cells = step.reads.cells
  local total = cells:field("height"):get(0)
  for _, prior in ipairs(step.links.prior) do
    total = total + prior.sum:field("value"):get(0)
  end
  step.writes.sum:field("value"):set(0, total)
  local at = cells:field("at")
  assert(step.key[1] == at:get(0, 0) and step.key[2] == at:get(0, 1) and step.key[3] == 0, "instance key")
  local tag = step.writes.tag:field("at")
  tag:set(0, at:get(0, 0), 0)
  tag:set(0, at:get(0, 1), 1)
end
)";

constexpr const char* column_buffers = R"(
{ name = value, size = one, format = [ total = v1 ] }
)";

constexpr const char* column_body = R"(
return function(step)
  local total = step.reads.sum:field("value"):get(0) + step.reads.cells:field("height"):get(0)
  step.writes.value:field("total"):set(0, total)
end
)";

double height(const int64_t x, const int64_t z) { return double(x * 10 + z + 1); }

double expected_tile(const int64_t x, const int64_t z) {
  double total = height(x, z);
  if (x > 0) total += expected_tile(x - 1, z);
  if (z > 0) total += expected_tile(x, z - 1);
  return total;
}

struct grid_object {
  int64_t x = 0;
};

originator::pipeline_description make_pipeline(const std::string& name, const char* buffers, std::vector<std::string> reads,
                                               std::vector<std::string> writes) {
  originator::pipeline_description d;
  d.name = name;
  d.buffers = originator::parse_buffers(buffers, name);
  originator::step_description step;
  step.name = "make";
  step.body = name + "/make";
  step.reads = std::move(reads);
  step.writes = std::move(writes);
  d.steps.push_back(std::move(step));
  return d;
}

constexpr const char* chain_text = R"(
name = test
generators = [
  { name = grid, native = grid, key = 2, buffers = grid_buffers, outputs = [ cells, around, meta ],
    trim = { around = meta.count } },
  { name = tile, entry = tile, key = 2, outputs = [ sum, tag ],
    links = [ { name = grid, from = grid, key = same },
              { name = prior, from = tile, keys = [ around.key ], count = meta.count } ] },
  { name = column, entry = column, key = 1, outputs = [ value ],
    links = [ { name = first, from = tile, project = [ 0, 0 ] },
              { name = second, from = grid, key = tag.at } ] }
]
)";

TEST_CASE("chain shape aabb requires two corners and retains its mode") {
  const auto parsed = originator::parse_chain(R"(
shapes = [ { name = room, buffer = rooms, box_a = low, box_b = high, box = aabb, reach = 1,
             value = room.ds } ]
generators = [ { name = rooms, native = room_host, key = 1, buffers = rooms.tavl, outputs = [ rooms ] } ]
)", "aabb");
  REQUIRE(parsed.shapes.size() == 1);
  CHECK(parsed.shapes[0].box == "aabb");
  CHECK_THROWS(originator::parse_chain(R"(
shapes = [ { name = room, buffer = rooms, box_a = low, box = aabb, reach = 1, value = room.ds } ]
generators = [ { name = rooms, native = room_host, key = 1, buffers = rooms.tavl, outputs = [ rooms ] } ]
)", "missing corner"));
}

struct test_chain {
  std::unique_ptr<originator::generator_chain> chain;
  std::map<std::pair<int64_t, int64_t>, int> grid_calls;
};

std::unique_ptr<test_chain> make_chain(const char* text = chain_text, const char* tile_body_text = tile_body,
                                       const char* tile_buffer_text = tile_buffers,
                                       std::vector<originator::script_text> modules = {},
                                       originator::parameters constants = {}) {
  auto out = std::make_unique<test_chain>();
  originator::chain_setup setup;
  setup.modules = std::move(modules);
  setup.constants = std::move(constants);
  setup.native_buffers.emplace_back("grid", originator::parse_buffers(grid_buffers, "grid"));
  setup.pipelines.emplace_back("tile", make_pipeline("tile", tile_buffer_text, {"cells"}, {"sum", "tag"}));
  setup.pipelines.emplace_back("column", make_pipeline("column", column_buffers, {"sum", "cells"}, {"value"}));
  setup.scripts.push_back({"tile", {{false, "make", "tile/make", tile_body_text}}});
  setup.scripts.push_back({"column", {{false, "make", "column/make", column_body}}});
  setup.fingerprints.emplace_back("tile", tile_body_text);
  setup.sizes.set("one", 1);
  setup.sizes.set("around_capacity", 4);

  out->chain = std::make_unique<originator::generator_chain>(originator::parse_chain(text, "test"), std::move(setup));
  auto* calls = &out->grid_calls;
  out->chain->set_native("grid", [calls](originator::generator_run& run) {
    const auto x = int64_t(run.key().words[0]);
    const auto z = int64_t(run.key().words[1]);
    ++(*calls)[{x, z}];
    auto& cells = run.output("cells");
    originator::write_key_word(cells.field(0), 0, uint64_t(x), 0);
    originator::write_key_word(cells.field(0), 0, uint64_t(z), 1);
    cells.field(1).set(0, height(x, z));
    auto& around = run.output("around");
    uint32_t count = 0;
    if (x > 0) {
      originator::write_key_word(around.field(0), count, uint64_t(x - 1), 0);
      originator::write_key_word(around.field(0), count++, uint64_t(z), 1);
    }
    if (z > 0) {
      originator::write_key_word(around.field(0), count, uint64_t(x), 0);
      originator::write_key_word(around.field(0), count++, uint64_t(z - 1), 1);
    }
    run.output("meta").field(0).set(0, count);
    run.set_payload(std::make_shared<const grid_object>(grid_object{x}));
  });
  out->chain->set_invoker_source(originator::make_chain_invokers(*out->chain, {}));
  return out;
}

originator::instance_key key2(const uint64_t x, const uint64_t z) { return {{x, z, 0}}; }

} // namespace

TEST_CASE("chain resolves same, projected, data and fan-in links through one cache") {
  auto test = make_chain();
  auto& chain = *test->chain;

  const auto tile = chain.get("tile", 7, key2(3, 2));
  CHECK(tile->field("sum", "value").get(0) == doctest::Approx(expected_tile(3, 2)));
  // Каждая клетка, до которой дотянулась рекурсия, построена ровно один раз.
  for (const auto& [at, calls] : test->grid_calls) CHECK(calls == 1);
  CHECK(test->grid_calls.size() == 12);
  CHECK(chain.stats("tile").builds == 12);

  const auto again = chain.get("tile", 7, key2(3, 2));
  CHECK(again.get() == tile.get());
  CHECK(chain.stats("tile").hits > 0);

  // Другое зерно мира — другой экземпляр, хотя данные здесь от зерна не зависят.
  const auto other_world = chain.get("tile", 8, key2(3, 2));
  CHECK(other_world.get() != tile.get());
  CHECK(other_world->compare(*tile).empty());

  const auto column = chain.get("column", 7, {{3, 0, 0}});
  CHECK(column->field("value", "total").get(0) == doctest::Approx(expected_tile(3, 3) + height(3, 3)));

  const auto grid = chain.get("grid", 7, key2(2, 1));
  // Ёмкость соседей 4, занято 2: в кэш ушли ровно они.
  CHECK(grid->at("around").count() == 2);
  CHECK(chain.get("grid", 7, key2(0, 0))->at("around").count() == 0);
  REQUIRE(grid->payload<grid_object>() != nullptr);
  CHECK(grid->payload<grid_object>()->x == 2);
  CHECK(grid->payload<double>() == nullptr);
}

TEST_CASE("chain verify rebuilds past the cache and compares bytes") {
  auto test = make_chain();
  CHECK(test->chain->verify("tile", 3, key2(2, 2)).empty());
  CHECK(test->chain->verify("column", 3, {{1, 0, 0}}).empty());

  auto independent = make_chain();
  const auto a = test->chain->get("tile", 3, key2(2, 2));
  const auto b = independent->chain->get("tile", 3, key2(2, 2));
  CHECK(a->compare(*b).empty());
  CHECK(test->chain->fingerprint("tile") == independent->chain->fingerprint("tile"));
  CHECK(test->chain->fingerprint("column") != test->chain->fingerprint("tile"));
}

TEST_CASE("chain refusals") {
  // Ключ с ненулевым лишним словом: у экземпляра было бы два имени.
  auto test = make_chain();
  CHECK_THROWS(test->chain->get("column", 1, {{1, 5, 0}}));

  // Пайплайн tile объявляет запись cells — буфера, привезённого связью: сборка обязана отказать.
  originator::chain_setup setup;
  setup.native_buffers.emplace_back("grid", originator::parse_buffers(grid_buffers, "grid"));
  setup.pipelines.emplace_back("tile", make_pipeline("tile", tile_buffers, {}, {"sum", "tag", "cells"}));
  setup.pipelines.emplace_back("column", make_pipeline("column", column_buffers, {"sum", "cells"}, {"value"}));
  setup.sizes.set("one", 1);
  setup.sizes.set("around_capacity", 4);
  originator::generator_chain bad(originator::parse_chain(chain_text, "bad"), std::move(setup));
  bad.set_native("grid", [](originator::generator_run&) {});
  CHECK_THROWS(bad.get("tile", 1, key2(0, 0)));

  // Связанный буфер, объявленный у потребителя второй раз.
  constexpr const char* redeclared = R"(
{ name = sum, size = one, format = [ value = v1 ] }
{ name = tag, size = one, format = [ at = ul2 ] }
{ name = cells, size = one, format = [ at = ul2, height = v1 ] }
)";
  auto twice = make_chain(chain_text, tile_body, redeclared);
  CHECK_THROWS(twice->chain->get("tile", 1, key2(0, 0)));

  // Связь на тот же экземпляр — цикл в одном потоке.
  constexpr const char* cyclic = R"(
name = cyclic
generators = [
  { name = grid, native = grid, key = 2, buffers = grid_buffers, outputs = [ cells, around, meta ] },
  { name = tile, entry = tile, key = 2, outputs = [ sum, tag ],
    links = [ { name = grid, from = grid, key = same }, { name = self, from = tile, key = same } ] },
  { name = column, entry = column, key = 1, outputs = [ value ],
    links = [ { name = first, from = tile, project = [ 0, 0 ] }, { name = second, from = grid, key = tag.at } ] }
]
)";
  auto loop = make_chain(cyclic);
  CHECK_THROWS(loop->chain->get("tile", 1, key2(1, 1)));

  // Связь без способа получить ключ и связь с двумя способами.
  CHECK_THROWS(originator::parse_chain(R"(
generators = [ { name = a, native = a, key = 1, outputs = [ x ], links = [ { from = b } ] } ]
)", "none"));
  CHECK_THROWS(originator::parse_chain(R"(
generators = [ { name = a, native = a, key = 1, outputs = [ x ], links = [ { from = b, key = same, project = [ 0 ] } ] } ]
)", "both"));
}

TEST_CASE("chain modules are shared, read-only and part of the fingerprint") {
  constexpr const char* module_text = R"(
local m = {}
function m.scaled(v, k) return v * k end
return m
)";
  constexpr const char* body_using_module = R"(
return function(step)
  local total = step.reads.cells:field("height"):get(0)
  for _, prior in ipairs(step.links.prior) do total = total + prior.sum:field("value"):get(0) end
  step.writes.sum:field("value"):set(0, modules.shared.scaled(total, 1))
  step.writes.tag:field("at"):set(0, step.key[1], 0)
  step.writes.tag:field("at"):set(0, step.key[2], 1)
end
)";
  auto test = make_chain(chain_text, body_using_module, tile_buffers, {{false, "shared", "test/shared", module_text}});
  const auto tile = test->chain->get("tile", 7, {{2, 1, 0}});
  CHECK(tile->field("sum", "value").get(0) == doctest::Approx(expected_tile(2, 1)));

  constexpr const char* body_writing_module = R"(
return function(step)
  modules.shared.scaled = nil
end
)";
  auto writer = make_chain(chain_text, body_writing_module, tile_buffers, {{false, "shared", "test/shared", module_text}});
  CHECK_THROWS(writer->chain->get("tile", 7, {{0, 0, 0}}));

  auto other = make_chain(chain_text, body_using_module, tile_buffers,
                          {{false, "shared", "test/shared", "return { scaled = function(v, k) return v * k end }"}});
  CHECK(other->chain->fingerprint("tile") != test->chain->fingerprint("tile"));
}

TEST_CASE("chain field profile controls noise and background") {
  const auto description = originator::parse_chain(R"(
noise_slots = [ { name = relief, seed_add = 7, period = 9 } ]
background = { enabled = true, base = 3, outside = 8, y_min = -4, y_max = 4,
               upper = 3.5, lower = 2.5, tag = 2,
               terms = [ { slot = relief, weight = 0.3 } ] }
generators = [ { name = dummy, native = dummy, outputs = [ result ] } ]
)", "profile");
  originator::shape_vocabulary words;
  originator::configure_script_field(words, description);
  const originator::primitive_field field(words, std::span<const originator::field_term_source>{}, 11);
  const originator::field_vector p{1.25, 0.5, -2.0};
  CHECK(field.value(p).value == doctest::Approx(3.0 + 0.3 * originator::value_noise(18, p, 9.0)));
  CHECK(field.value({1.25, 5.0, -2.0}).value == 8.0);
  CHECK(field.value(p).tag == 2);
  CHECK(words.background_bound() == 3.5);
  CHECK(words.background_lower() == 2.5);
}

TEST_CASE("chain constants reach modules and steps, are read-only and affect fingerprints") {
  originator::parameters values;
  values.set_number("gain", 3.0);
  values.set_string("label", "world");
  constexpr const char* body = R"(
return function(step)
  assert(step.constants.label == "world" and step.params.gain == step.constants.gain)
  step.writes.sum:field("value"):set(0, modules.rules.gain * step.reads.cells:field("height"):get(0))
  step.writes.tag:field("at"):set(0, step.key[1], 0)
  step.writes.tag:field("at"):set(0, step.key[2], 1)
end
)";
  const std::vector<originator::script_text> modules{{false, "rules", "rules", "return { gain = constants.gain }"}};
  auto test = make_chain(chain_text, body, tile_buffers, modules, values);
  CHECK(test->chain->get("tile", 0, {{0, 0, 0}})->field("sum", "value").get(0) == 3.0);
  CHECK(test->chain->constants().number("gain") == 3.0);
  auto writer = make_chain(chain_text, "return function(step) step.constants.gain = 7 end", tile_buffers, {}, values);
  CHECK_THROWS(writer->chain->get("tile", 0, {{0, 0, 0}}));
  values.set_number("gain", 4.0);
  auto changed = make_chain(chain_text, body, tile_buffers, modules, values);
  CHECK(test->chain->fingerprint("tile") != changed->chain->fingerprint("tile"));
}

TEST_CASE("generation contexts release Lua hosts while the worker pool stays alive") {
  auto test = make_chain();
  std::mutex grid_lock;
  test->chain->set_native("grid", [&](originator::generator_run& run) {
    std::lock_guard lock(grid_lock);
    auto& cells = run.output("cells");
    cells.field(0).set_integer(0, int64_t(run.key().words[0]), 0);
    cells.field(0).set_integer(0, int64_t(run.key().words[1]), 1);
    cells.field(1).set(0, height(int64_t(run.key().words[0]), int64_t(run.key().words[1])));
    run.output("meta").field(0).set(0, 0);
  });
  std::weak_ptr<int> last_host_data;
  std::mutex host_lock;
  test->chain->set_invoker_source(originator::make_chain_invokers(*test->chain, [&](originator::tool_registry& tools) {
    std::lock_guard lock(host_lock);
    auto pin = std::make_shared<int>(42);
    last_host_data = pin;
    originator::tool_description tool;
    tool.name = "lifetime_probe";
    tool.shape = originator::aperture::sequential;
    tool.body = [pin](const originator::tool_call&, size_t, size_t) {};
    tools.add(std::move(tool));
  }));
  std::array<originator::generation_context, 8> contexts;
  thread::atomic_pool pool(8);
  std::atomic<size_t> good = 0;
  for (size_t i = 0; i < contexts.size(); ++i) {
    pool.submit([&, i] {
      const auto result = test->chain->get(contexts[i], "tile", i, key2(0, 0));
      if (result->field("sum", "value").get(0) == height(0, 0)) ++good;
    });
  }
  pool.compute();
  pool.wait();
  CHECK(good == 8);
  CHECK(test->chain->context_count() == 0);
  REQUIRE_FALSE(last_host_data.expired());
  for (auto& context : contexts) {
    CHECK_FALSE(context.hosts.empty());
    context.release();
    CHECK(context.hosts.empty());
    CHECK(context.field_scratch.retained_bytes() == 0);
  }
  CHECK(last_host_data.expired());
  test->chain->get("tile", 99, key2(0, 0));
  CHECK(test->chain->context_count() == 1);
  test->chain->release_contexts();
  CHECK(test->chain->context_count() == 0);
  CHECK(last_host_data.expired());
  test->chain->get("tile", 100, key2(0, 0));
  REQUIRE_FALSE(last_host_data.expired());
  test.reset(); // pool and its worker threads are still alive.
  CHECK(last_host_data.expired());
}

TEST_CASE("native nested builds forward context and restore state after failure") {
  auto test = make_chain();
  bool cycle = false;
  test->chain->set_native("grid", [&](originator::generator_run& run) {
    auto& context = run.context();
    const auto* parent_fields = context.fields;
    const auto parent_node = context.current_node;
    const auto parent_depth = context.step_depth;
    const auto parent_stack = context.building.size();
    double value = 1;
    if (cycle || run.key().words[0] != 0) {
      auto child = run.key();
      if (!cycle) --child.words[0];
      value += test->chain->get(context, "grid", run.world_seed(), child)->field("cells", "height").get(0);
    }
    CHECK(context.fields == parent_fields);
    CHECK(context.current_node == parent_node);
    CHECK(context.step_depth == parent_depth);
    CHECK(context.building.size() == parent_stack);
    run.output("cells").field(1).set(0, value);
    run.output("meta").field(0).set(0, 0);
  });
  originator::generation_context context;
  CHECK(test->chain->get(context, "grid", 0, key2(3, 0))->field("cells", "height").get(0) == 4);
  cycle = true;
  CHECK_THROWS(test->chain->get(context, "grid", 1, key2(3, 0)));
  CHECK(context.building.empty());
  CHECK(context.fields == nullptr);
  CHECK(context.current_node == nullptr);
  CHECK(context.step_depth == 0);
  context.release();
  cycle = false;
  CHECK(test->chain->get(context, "grid", 1, key2(3, 0))->field("cells", "height").get(0) == 4);
}
