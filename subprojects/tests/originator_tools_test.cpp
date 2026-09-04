#include <cstring>
#include <stdexcept>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"

using namespace devils_engine;

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t grid_width = 512;
constexpr size_t grid_count = grid_width * grid_width;

originator::buffer make_field_buffer(const originator::storage_kind::values storage, const size_t count) {
  const std::vector<field_pair> fields = {
    {"height", "v1"},
    {"moisture", "v1"},
    {"smoothed", "v1"},
    {"biome", "ub1"},
  };
  auto layout = originator::make_buffer_layout(storage, fields, "cells");
  return originator::buffer("cells", std::move(layout), count);
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

std::vector<std::byte> snapshot(const originator::buffer& b) {
  std::vector<std::byte> copy(b.byte_size());
  std::memcpy(copy.data(), b.base_pointer(), copy.size());
  return copy;
}

originator::parameters noise_params() {
  originator::parameters p;
  p.set_number("width", double(grid_width));
  p.set_number("frequency", 0.02);
  p.set_number("octaves", 4);
  return p;
}
} // namespace

TEST_CASE("originator parallel execution is bit-identical to serial") {
  auto& registry = *[] {
    static originator::tool_registry r;
    if (r.size() == 0) {
      r.add_standard_tools();
    }
    return &r;
  }();

  const auto* noise = registry.find("value_noise");
  REQUIRE(noise != nullptr);

  const auto params = noise_params();

  auto serial_buffer = make_field_buffer(originator::storage_kind::soa, grid_count);
  const auto serial_out = std::vector<originator::field_ref>{writable(serial_buffer, "height")};
  originator::dispatch(*noise, {}, serial_out, params, 12345, 0, grid_count, "test", nullptr);
  const auto serial_bytes = snapshot(serial_buffer);

  // Разное число рабочих потоков не должно менять ни одного байта результата.
  for (const size_t threads : {1u, 3u, 7u}) {
    thread::atomic_pool pool(threads);
    auto parallel_buffer = make_field_buffer(originator::storage_kind::soa, grid_count);
    const auto parallel_out = std::vector<originator::field_ref>{writable(parallel_buffer, "height")};
    originator::dispatch(*noise, {}, parallel_out, params, 12345, 0, grid_count, "test", &pool);

    const auto parallel_bytes = snapshot(parallel_buffer);
    CHECK(parallel_bytes == serial_bytes);
  }
}

TEST_CASE("originator storage layout does not change the result") {
  originator::tool_registry registry;
  registry.add_standard_tools();
  const auto* noise = registry.find("value_noise");
  const auto params = noise_params();

  auto soa = make_field_buffer(originator::storage_kind::soa, grid_count);
  auto aos = make_field_buffer(originator::storage_kind::aos, grid_count);

  const auto soa_out = std::vector<originator::field_ref>{writable(soa, "height")};
  const auto aos_out = std::vector<originator::field_ref>{writable(aos, "height")};

  originator::dispatch(*noise, {}, soa_out, params, 777, 0, grid_count, "test", nullptr);
  originator::dispatch(*noise, {}, aos_out, params, 777, 0, grid_count, "test", nullptr);

  // Байты буферов разные (разная раскладка), но ЗНАЧЕНИЯ по именам совпадают полностью.
  const auto soa_height = soa.field(soa.find_field("height"));
  const auto aos_height = aos.field(aos.find_field("height"));
  CHECK(soa_height.contiguous());
  CHECK_FALSE(aos_height.contiguous());

  for (size_t i = 0; i < grid_count; i += 977) {
    CHECK(soa_height.get(i) == aos_height.get(i));
  }
}

TEST_CASE("originator refuses a gather that reads and writes the same field") {
  originator::tool_registry registry;
  registry.add_standard_tools();
  const auto* blur = registry.find("box_blur");
  REQUIRE(blur != nullptr);
  CHECK(blur->shape == originator::aperture::gather);

  auto cells = make_field_buffer(originator::storage_kind::soa, grid_count);

  originator::parameters params;
  params.set_number("width", double(grid_width));
  params.set_number("radius", 2);

  const auto same_in = std::vector<originator::field_ref>{readable(cells, "height")};
  const auto same_out = std::vector<originator::field_ref>{writable(cells, "height")};

  const auto rejected = originator::check_dispatch(*blur, same_in, same_out, 0, grid_count, "climate");
  CHECK_FALSE(rejected.allowed);
  CHECK(rejected.message.find("gather") != std::string::npos);
  CHECK(rejected.message.find("cells.height") != std::string::npos);

  CHECK_THROWS_AS(
    originator::dispatch(*blur, same_in, same_out, params, 1, 0, grid_count, "climate", nullptr),
    std::runtime_error);

  // Отдельное поле-приёмник — тот же инструмент проходит.
  const auto ok_out = std::vector<originator::field_ref>{writable(cells, "smoothed")};
  const auto accepted = originator::check_dispatch(*blur, same_in, ok_out, 0, grid_count, "climate");
  CHECK(accepted.allowed);
  CHECK(accepted.parallel);
  CHECK_NOTHROW(originator::dispatch(*blur, same_in, ok_out, params, 1, 0, grid_count, "climate", nullptr));
}

TEST_CASE("originator refuses to write through a read binding") {
  originator::tool_registry registry;
  registry.add_standard_tools();
  const auto* fill = registry.find("fill");

  auto cells = make_field_buffer(originator::storage_kind::soa, 128);
  const auto read_only_out = std::vector<originator::field_ref>{readable(cells, "height")};

  const auto check = originator::check_dispatch(*fill, {}, read_only_out, 0, 128, "terrain");
  CHECK_FALSE(check.allowed);
  CHECK(check.message.find("bound for reading") != std::string::npos);
}

TEST_CASE("originator reductions do not depend on the number of threads") {
  originator::tool_registry registry;
  registry.add_standard_tools();
  const auto* noise = registry.find("value_noise");
  const auto* sum = registry.find("reduce_sum");
  const auto* max = registry.find("reduce_max");

  auto cells = make_field_buffer(originator::storage_kind::soa, grid_count);
  const auto out = std::vector<originator::field_ref>{writable(cells, "height")};
  originator::dispatch(*noise, {}, out, noise_params(), 99, 0, grid_count, "terrain", nullptr);

  const auto in = std::vector<originator::field_ref>{readable(cells, "height")};
  const originator::parameters empty;

  const double serial_sum = originator::dispatch_reduce(*sum, in, empty, 0, 0, grid_count, "stats", nullptr);
  const double serial_max = originator::dispatch_reduce(*max, in, empty, 0, 0, grid_count, "stats", nullptr);

  for (const size_t threads : {1u, 4u, 6u}) {
    thread::atomic_pool pool(threads);
    // Сравнение точное, а не приближённое: у свёртки фиксированное разбиение на чанки, поэтому
    // порядок сложения плавающих чисел не зависит от расклада исполнения.
    CHECK(originator::dispatch_reduce(*sum, in, empty, 0, 0, grid_count, "stats", &pool) == serial_sum);
    CHECK(originator::dispatch_reduce(*max, in, empty, 0, 0, grid_count, "stats", &pool) == serial_max);
  }

  CHECK(serial_max > 0.0);
  CHECK(serial_sum > 0.0);
}

TEST_CASE("originator ratio normalises and refuses a zero divisor policy") {
  originator::tool_registry registry;
  registry.add_standard_tools();
  const auto* ratio = registry.find("ratio");
  REQUIRE(ratio != nullptr);
  CHECK(ratio->shape == originator::aperture::pointwise);

  auto cells = make_field_buffer(originator::storage_kind::soa, 4);
  auto weight = cells.field(cells.find_field("moisture"));
  auto sum = cells.field(cells.find_field("height"));
  auto out = cells.field(cells.find_field("smoothed"));
  const double numerators[4] = {6.0, -3.0, 5.0, 7.0};
  const double denominators[4] = {2.0, 4.0, 0.0, -2.0};
  for (size_t i = 0; i < 4; ++i) {
    sum.set(i, numerators[i]);
    weight.set(i, denominators[i]);
  }

  originator::parameters params;
  params.set_number("minimum_divisor", 0.5);
  const std::vector<originator::field_ref> ratio_inputs{readable(cells, "height"), readable(cells, "moisture")};
  const std::vector<originator::field_ref> ratio_outputs{writable(cells, "smoothed")};
  originator::dispatch(*ratio, ratio_inputs, ratio_outputs, params, 1, 0, 4, "normalise", nullptr);

  CHECK(out.get(0) == doctest::Approx(3.0));
  CHECK(out.get(1) == doctest::Approx(-0.75));
  // Нулевой знаменатель зажимается объявленным минимумом, а не даёт бесконечность: у взвешенного
  // среднего это означает «ни одно правило здесь не действует», и молчаливый ноль плотности стал бы
  // поверхностью, то есть стеной в дырке покрытия.
  CHECK(out.get(2) == doctest::Approx(10.0));
  // Знак знаменателя сохраняется при зажиме: иначе поле у дырки прыгало бы через ноль.
  CHECK(out.get(3) == doctest::Approx(-3.5));

  originator::parameters broken;
  broken.set_number("minimum_divisor", 0.0);
  CHECK_THROWS_AS(originator::dispatch(*ratio, ratio_inputs, ratio_outputs, broken, 1, 0, 4, "normalise", nullptr),
                  std::runtime_error);

  // Минимальный знаменатель ОБЯЗАТЕЛЕН, а не «по умолчанию мал»: значение по умолчанию означало бы,
  // что движок сам решает, какая сумма весов считается нулевой, — а это решение автора конфига.
  originator::parameters absent;
  CHECK_THROWS_AS(originator::dispatch(*ratio, ratio_inputs, ratio_outputs, absent, 1, 0, 4, "normalise", nullptr),
                  std::runtime_error);
}

TEST_CASE("originator range is checked against the buffer") {
  originator::tool_registry registry;
  registry.add_standard_tools();
  const auto* fill = registry.find("fill");

  auto cells = make_field_buffer(originator::storage_kind::soa, 128);
  const auto out = std::vector<originator::field_ref>{writable(cells, "height")};

  const auto too_far = originator::check_dispatch(*fill, {}, out, 0, 129, "terrain");
  CHECK_FALSE(too_far.allowed);
  CHECK(too_far.message.find("exceeds") != std::string::npos);

  const auto inverted = originator::check_dispatch(*fill, {}, out, 64, 32, "terrain");
  CHECK_FALSE(inverted.allowed);
}
