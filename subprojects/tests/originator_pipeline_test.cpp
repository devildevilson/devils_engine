#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/pipeline.h"

using namespace devils_engine;

namespace {
constexpr std::string_view buffers_config = R"(
{
  name = cells
  format = [ (height, v1), (moisture, v1), (smoothed, v1), (biome, ub1) ]
  layout = soa
  size = cell_count
}
{
  name = state
  format = [ (land_cells, ui1), (peak, v1) ]
  size = single
}
)";

constexpr std::string_view steps_config = R"(
{
  name = terrain
  body = gen/terrain
  writes = [ cells ]
  params = { frequency = 0.02, octaves = 4 }
}
{
  name = climate
  body = gen/climate
  reads = [ cells ]
  writes = [ state ]
}
)";

originator::size_table make_sizes(const size_t cells) {
  originator::size_table sizes;
  sizes.set("cell_count", cells);
  sizes.set("single", 1);
  return sizes;
}
} // namespace

TEST_CASE("originator parses buffers and steps from tavl") {
  const auto buffers = originator::parse_buffers(buffers_config, "buffers");
  REQUIRE(buffers.size() == 2);

  CHECK(buffers[0].name == "cells");
  CHECK(buffers[0].size_name == "cell_count");
  CHECK(buffers[0].layout.storage == originator::storage_kind::soa);
  REQUIRE(buffers[0].layout.fields.size() == 4);
  CHECK(buffers[0].layout.fields[0].name == "height");
  CHECK(buffers[0].layout.fields[0].type == originator::parse_field_type("v1"));
  CHECK(buffers[0].layout.fields[3].name == "biome");
  CHECK(buffers[0].layout.fields[3].type.byte_size() == 1);

  // Раскладка не указана => aos по умолчанию.
  CHECK(buffers[1].layout.storage == originator::storage_kind::aos);

  const auto steps = originator::parse_steps(steps_config, "steps");
  REQUIRE(steps.size() == 2);
  CHECK(steps[0].name == "terrain");
  CHECK(steps[0].body == "gen/terrain");
  REQUIRE(steps[0].writes.size() == 1);
  CHECK(steps[0].writes[0] == "cells");
  CHECK(steps[0].reads.empty());
  CHECK(steps[0].params.number("frequency") == doctest::Approx(0.02));
  CHECK(steps[0].params.integer("octaves") == 4);

  CHECK(steps[1].reads.size() == 1);
  CHECK(steps[1].writes[0] == "state");
}

TEST_CASE("originator pipeline allocates declared buffers and reports its cost") {
  originator::pipeline_description description;
  description.name = "test";
  description.buffers = originator::parse_buffers(buffers_config, "buffers");
  description.steps = originator::parse_steps(steps_config, "steps");

  originator::pipeline p(description, make_sizes(1024), 42);

  CHECK(p.buffer_count() == 2);
  CHECK(p.step_count() == 2);
  REQUIRE(p.find_buffer("cells") != nullptr);
  CHECK(p.find_buffer("cells")->count() == 1024);
  CHECK(p.find_buffer("missing") == nullptr);

  // 3 * v1 + ub1 на 1024 элемента плюс выравнивание планов, и один элемент state.
  CHECK(p.total_byte_size() >= 13 * 1024);
  CHECK(p.total_byte_size() < 13 * 1024 + 1024);

  // Что доступно потребителю после шага — это ровно writes шага.
  REQUIRE(p.published_after(0).size() == 1);
  CHECK(p.published_after(0)[0]->name() == "cells");
  CHECK(p.published_after(1)[0]->name() == "state");
}

TEST_CASE("originator step seed follows the name, not the position") {
  originator::pipeline_description description;
  description.name = "test";
  description.buffers = originator::parse_buffers(buffers_config, "buffers");
  description.steps = originator::parse_steps(steps_config, "steps");

  std::vector<uint64_t> seeds;
  const auto collect = [&](const originator::step_context& ctx) { seeds.push_back(ctx.seed); };

  originator::pipeline first(description, make_sizes(64), 7);
  first.run(collect);
  REQUIRE(seeds.size() == 2);
  CHECK(seeds[0] != seeds[1]);

  // Другое зерно пайплайна => другие зёрна шагов.
  std::vector<uint64_t> other;
  originator::pipeline second(description, make_sizes(64), 8);
  second.run([&](const originator::step_context& ctx) { other.push_back(ctx.seed); });
  CHECK(other[0] != seeds[0]);

  // То же зерно => те же значения, независимо от запуска.
  std::vector<uint64_t> again;
  originator::pipeline third(description, make_sizes(64), 7);
  third.run([&](const originator::step_context& ctx) { again.push_back(ctx.seed); });
  CHECK(again == seeds);
}

TEST_CASE("originator rejects a step that reads before anything wrote") {
  originator::pipeline_description description;
  description.name = "broken";
  description.buffers = originator::parse_buffers(buffers_config, "buffers");
  description.steps = originator::parse_steps(R"(
{
  name = climate
  body = gen/climate
  reads = [ cells ]
  writes = [ state ]
}
)", "steps");

  CHECK_THROWS_AS(originator::pipeline(description, make_sizes(64), 1), std::runtime_error);
}

TEST_CASE("originator rejects unknown buffers and contradictory bindings") {
  auto buffers = originator::parse_buffers(buffers_config, "buffers");

  {
    originator::pipeline_description description;
    description.name = "unknown";
    description.buffers = buffers;
    description.steps = originator::parse_steps(R"(
{ name = terrain, body = gen/terrain, writes = [ nowhere ] }
)", "steps");
    CHECK_THROWS_AS(originator::pipeline(description, make_sizes(64), 1), std::runtime_error);
  }

  {
    originator::pipeline_description description;
    description.name = "twice";
    description.buffers = buffers;
    description.steps = originator::parse_steps(R"(
{ name = terrain, body = gen/terrain, writes = [ cells, cells ] }
)", "steps");
    CHECK_THROWS_AS(originator::pipeline(description, make_sizes(64), 1), std::runtime_error);
  }

  {
    originator::pipeline_description description;
    description.name = "both";
    description.buffers = buffers;
    description.steps = originator::parse_steps(R"(
{ name = terrain, body = gen/terrain, reads = [ cells ], writes = [ cells ] }
)", "steps");
    CHECK_THROWS_AS(originator::pipeline(description, make_sizes(64), 1), std::runtime_error);
  }
}

TEST_CASE("originator rejects an undeclared size") {
  originator::pipeline_description description;
  description.name = "test";
  description.buffers = originator::parse_buffers(buffers_config, "buffers");
  description.steps = originator::parse_steps(steps_config, "steps");

  originator::size_table incomplete;
  incomplete.set("single", 1);
  CHECK_THROWS_AS(originator::pipeline(description, incomplete, 1), std::runtime_error);
}

TEST_CASE("originator keeps two seeds with different chunk contracts") {
  originator::pipeline_description description;
  description.name = "chunked";
  description.buffers = originator::parse_buffers(buffers_config, "buffers");
  description.steps = originator::parse_steps(steps_config, "steps");

  struct seeds {
    std::vector<uint64_t> continuous;
    std::vector<uint64_t> per_chunk;
  };

  const auto collect = [](seeds& out) {
    return [&out](const originator::step_context& ctx) {
      out.continuous.push_back(ctx.seed);
      out.per_chunk.push_back(ctx.chunk_seed);
    };
  };

  originator::pipeline p(description, make_sizes(64), 100);

  seeds first;
  p.set_chunk({1, 2, 0});
  p.run(collect(first));

  seeds other;
  p.set_chunk({2, 2, 0});
  p.run(collect(other));

  // Непрерывное зерно НЕ зависит от чанка: поле, посчитанное по мировой позиции, не должно
  // разъезжаться на швах. Именно на этом изначальный дизайн с одним зерном и сломался.
  CHECK(other.continuous == first.continuous);
  // Зерно чанка зависит: то, что обязано быть независимым в каждом чанке, его и берёт.
  CHECK(other.per_chunk != first.per_chunk);

  // Возврат к тому же ключу даёт то же самое, хотя между ними считался другой чанк.
  seeds again;
  p.set_chunk({1, 2, 0});
  p.run(collect(again));
  CHECK(again.per_chunk == first.per_chunk);
  CHECK(again.continuous == first.continuous);

  // Свежий пайплайн с тем же зерном мира и тем же ключом — тоже то же самое.
  originator::pipeline fresh(description, make_sizes(64), 100);
  seeds from_fresh;
  fresh.set_chunk({1, 2, 0});
  fresh.run(collect(from_fresh));
  CHECK(from_fresh.per_chunk == first.per_chunk);
  CHECK(from_fresh.continuous == first.continuous);

  // Каждая координата ключа участвует: иначе (1,2,0) и (1,0,2) совпали бы.
  seeds swapped;
  fresh.set_chunk({1, 0, 2});
  fresh.run(collect(swapped));
  CHECK(swapped.per_chunk != first.per_chunk);

  // Разные шаги получают разные зёрна, и это не зависит от чанка.
  REQUIRE(first.continuous.size() == 2);
  CHECK(first.continuous[0] != first.continuous[1]);
  CHECK(first.per_chunk[0] != first.per_chunk[1]);

  // Ключ виден телу шага: без него нельзя посчитать мировое смещение чанка.
  p.set_chunk({7, -3, 1});
  p.run_step(0, [](const originator::step_context& ctx) {
    CHECK(ctx.chunk.x == 7);
    CHECK(ctx.chunk.y == -3);
    CHECK(ctx.chunk.z == 1);
  });
}

TEST_CASE("originator buffers survive a chunk change so streaming does not reallocate") {
  originator::pipeline_description description;
  description.name = "chunked";
  description.buffers = originator::parse_buffers(buffers_config, "buffers");
  description.steps = originator::parse_steps(steps_config, "steps");

  originator::pipeline p(description, make_sizes(256), 5);
  auto* cells = p.find_buffer("cells");
  REQUIRE(cells != nullptr);

  const auto* memory_before = cells->base_pointer();
  auto height = cells->field(cells->find_field("height"));
  height.set(10, 0.75);

  p.set_chunk({4, 4, 0});
  CHECK(cells->base_pointer() == memory_before); // та же память, без перевыделения
  CHECK(height.get(10) == doctest::Approx(0.75)); // и без неявного обнуления

  // Обнуление остаётся явным: оно нужно только чтобы ДОКАЗАТЬ независимость чанков.
  p.clear_buffers();
  CHECK(height.get(10) == doctest::Approx(0.0));
}
