#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/computation_queue.h"
#include "devils_engine/thread/atomic_pool.h"

using namespace devils_engine;

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t grid_width = 128;
constexpr size_t grid_count = grid_width * grid_width;

originator::tool_registry& registry() {
  static originator::tool_registry r;
  if (r.size() == 0) {
    r.add_standard_tools();
  }
  return r;
}

originator::buffer make_cells(const size_t count) {
  const std::vector<field_pair> fields = {
    {"height", "v1"},
    {"moisture", "v1"},
    {"smoothed", "v1"},
    {"biome", "ub1"},
  };
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
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

originator::queue_call tool_call(const std::string_view& name,
                                std::vector<originator::field_ref> inputs,
                                std::vector<originator::field_ref> outputs,
                                originator::parameters params,
                                const size_t count) {
  const auto* tool = registry().find(name);
  REQUIRE(tool != nullptr);

  originator::queue_call call;
  call.label.assign(name);
  call.tool = tool;
  call.shape = tool->shape;
  call.inputs = std::move(inputs);
  call.outputs = std::move(outputs);
  call.params = std::move(params);
  call.seed = 12345;
  call.range_begin = 0;
  call.range_end = count;
  return call;
}

// Тот же куплет, что живёт в теле шага climate у GN01: размытие по окну и классификация по двум
// полям. Между вызовами нет ни одного промежуточного значения — ровно тот подграф, ради которого
// очередь и заводится.
originator::computation_queue climate_queue(originator::buffer& cells, const size_t count) {
  originator::parameters blur;
  blur.set_number("width", double(grid_width));
  blur.set_number("radius", 2);

  originator::parameters classify;
  classify.set_number("sea_level", 0.5);
  classify.set_number("dry", 0.35);
  classify.set_number("wet", 0.65);

  originator::computation_queue queue;
  queue.name = "climate";
  queue.calls.push_back(tool_call("box_blur", {readable(cells, "height")}, {writable(cells, "smoothed")}, blur, count));
  queue.calls.push_back(tool_call("classify", {readable(cells, "smoothed"), readable(cells, "moisture")},
                                  {writable(cells, "biome")}, classify, count));
  queue.output.push_back(writable(cells, "biome"));
  return queue;
}

void fill_noise(originator::buffer& cells, const size_t count) {
  originator::parameters params;
  params.set_number("width", double(grid_width));
  params.set_number("frequency", 0.05);
  params.set_number("octaves", 3);

  const auto* noise = registry().find("value_noise");
  REQUIRE(noise != nullptr);
  for (const auto* name : {"height", "moisture"}) {
    const std::vector<originator::field_ref> out{writable(cells, name)};
    originator::dispatch(*noise, {}, out, params, 777, 0, count, "fixture", nullptr);
  }
}
} // namespace

TEST_CASE("originator queue runs the same work as the same calls one by one") {
  auto queued = make_cells(grid_count);
  auto separate = make_cells(grid_count);
  fill_noise(queued, grid_count);
  fill_noise(separate, grid_count);

  const auto queue = climate_queue(queued, grid_count);
  const auto report = originator::run_queue(queue, nullptr);
  CHECK(report.calls == 2);
  // Обходов два, хотя слияние есть: `box_blur` это gather, ему нужны соседи, то есть весь
  // предыдущий проход целиком, — группа на нём разрывается. Слияние этой цепочки не касается.
  CHECK(report.passes == 2);
  CHECK(report.fused == 0);

  const auto reference = climate_queue(separate, grid_count);
  for (const auto& call : reference.calls) {
    originator::dispatch(*call.tool, call.inputs, call.outputs, call.params, call.seed, call.range_begin,
                         call.range_end, "climate", nullptr);
  }

  CHECK(snapshot(queued) == snapshot(separate));
}

TEST_CASE("originator queue result does not depend on the number of threads") {
  auto serial = make_cells(grid_count);
  fill_noise(serial, grid_count);
  originator::run_queue(climate_queue(serial, grid_count), nullptr);
  const auto serial_bytes = snapshot(serial);

  for (const size_t threads : {1u, 3u, 7u}) {
    thread::atomic_pool pool(threads);
    auto parallel = make_cells(grid_count);
    fill_noise(parallel, grid_count);
    originator::run_queue(climate_queue(parallel, grid_count), &pool);
    CHECK(snapshot(parallel) == serial_bytes);
  }
}

namespace {
// Цепочка из трёх pointwise над одним диапазоном: `smoothed` считается, читается и превращается в
// `moisture`, ни одно промежуточное поле наружу не выходит. Это и есть то, что слияние должно
// сложить в ОДИН обход.
originator::computation_queue pointwise_chain(originator::buffer& cells, const size_t count) {
  originator::parameters scale;
  scale.set_number("scale", 1.5);
  scale.set_number("offset", -0.25);

  originator::parameters blend;
  blend.set_number("a", 0.5);
  blend.set_number("b", 0.5);

  originator::parameters modulate;
  modulate.set_number("scale", 2.0);

  originator::computation_queue queue;
  queue.name = "chain";
  queue.calls.push_back(tool_call("remap", {readable(cells, "height")}, {writable(cells, "smoothed")}, scale, count));
  queue.calls.push_back(tool_call("blend", {readable(cells, "smoothed"), readable(cells, "moisture")},
                                  {writable(cells, "smoothed")}, blend, count));
  queue.calls.push_back(tool_call("modulate", {readable(cells, "smoothed"), readable(cells, "height")},
                                  {writable(cells, "moisture")}, modulate, count));
  queue.output.push_back(writable(cells, "moisture"));
  return queue;
}

void run_call_by_call(const originator::computation_queue& queue, thread::atomic_pool* pool) {
  for (const auto& call : queue.calls) {
    originator::dispatch(*call.tool, call.inputs, call.outputs, call.params, call.seed, call.range_begin,
                         call.range_end, queue.name, pool);
  }
}
} // namespace

TEST_CASE("originator queue fuses adjacent pointwise passes into one traversal") {
  auto cells = make_cells(grid_count);
  fill_noise(cells, grid_count);

  const auto report = originator::run_queue(pointwise_chain(cells, grid_count), nullptr);
  CHECK(report.calls == 3);
  CHECK(report.fused == 3);
  // Три вызова, ОДИН обход данных. Это и есть измеряемая величина.
  CHECK(report.passes == 1);
}

TEST_CASE("originator queue fusion is bit-identical to the same calls run one by one") {
  auto fused = make_cells(grid_count);
  auto separate = make_cells(grid_count);
  fill_noise(fused, grid_count);
  fill_noise(separate, grid_count);

  originator::run_queue(pointwise_chain(fused, grid_count), nullptr);
  run_call_by_call(pointwise_chain(separate, grid_count), nullptr);
  CHECK(snapshot(fused) == snapshot(separate));

  // И при любом числе потоков: плитки независимы, потому что каждый элемент считается сам по себе.
  const auto reference = snapshot(separate);
  for (const size_t threads : {1u, 3u, 7u}) {
    thread::atomic_pool pool(threads);
    auto parallel = make_cells(grid_count);
    fill_noise(parallel, grid_count);
    originator::run_queue(pointwise_chain(parallel, grid_count), &pool);
    CHECK(snapshot(parallel) == reference);
  }
}

TEST_CASE("originator queue does not fuse across what it must not") {
  auto cells = make_cells(grid_count);
  fill_noise(cells, grid_count);

  SUBCASE("a gather in the middle breaks the group") {
    // box_blur читает соседей, то есть ему нужен ВЕСЬ предыдущий проход, а не своя плитка.
    auto queue = pointwise_chain(cells, grid_count);
    originator::parameters blur;
    blur.set_number("width", double(grid_width));
    blur.set_number("radius", 2);
    auto gather = tool_call("box_blur", {readable(cells, "moisture")}, {writable(cells, "biome")}, blur, grid_count);
    queue.calls.push_back(std::move(gather));
    queue.output.push_back(writable(cells, "biome"));

    const auto report = originator::run_queue(queue, nullptr);
    CHECK(report.calls == 4);
    CHECK(report.fused == 3);
    // Слитая тройка плюс отдельный gather.
    CHECK(report.passes == 2);
  }

  SUBCASE("a different range breaks the group") {
    auto queue = pointwise_chain(cells, grid_count);
    queue.calls[1].range_end = grid_count / 2;
    // Второй вызов теперь пишет только половину, поэтому третий не может считаться по его плиткам.
    const auto report = originator::run_queue(queue, nullptr);
    CHECK(report.calls == 3);
    CHECK(report.fused == 0);
    CHECK(report.passes == 3);
  }

  SUBCASE("a single pointwise call stays a single call") {
    originator::parameters scale;
    scale.set_number("scale", 2.0);

    originator::computation_queue queue;
    queue.name = "one";
    queue.calls.push_back(tool_call("remap", {readable(cells, "height")}, {writable(cells, "smoothed")}, scale, grid_count));
    queue.output.push_back(writable(cells, "smoothed"));

    const auto report = originator::run_queue(queue, nullptr);
    CHECK(report.fused == 0);
    CHECK(report.passes == 1);
  }
}

TEST_CASE("originator queue refuses before it runs a single call") {
  auto cells = make_cells(grid_count);
  fill_noise(cells, grid_count);

  SUBCASE("a queue without calls") {
    originator::computation_queue queue;
    queue.name = "empty";
    queue.output.push_back(writable(cells, "biome"));
    CHECK_FALSE(originator::check_queue(queue).allowed);
    CHECK_THROWS_AS(originator::run_queue(queue, nullptr), std::exception);
  }

  SUBCASE("a queue that declares no output") {
    auto queue = climate_queue(cells, grid_count);
    queue.output.clear();
    CHECK_FALSE(originator::check_queue(queue).allowed);
  }

  SUBCASE("output nobody inside writes") {
    auto queue = climate_queue(cells, grid_count);
    queue.output.push_back(writable(cells, "height"));
    CHECK_FALSE(originator::check_queue(queue).allowed);
  }

  SUBCASE("scatter, sequential and reduce do not fit at all") {
    CHECK(originator::fits_in_queue(originator::aperture::pointwise));
    CHECK(originator::fits_in_queue(originator::aperture::gather));
    CHECK_FALSE(originator::fits_in_queue(originator::aperture::scatter));
    CHECK_FALSE(originator::fits_in_queue(originator::aperture::sequential));
    CHECK_FALSE(originator::fits_in_queue(originator::aperture::reduce));

    // У каждого отказа своя причина, и это проверяется: общий текст на три случая означал бы, что
    // автор скрипта догадывается, а не читает.
    const auto scatter = originator::queue_rejection_reason(originator::aperture::scatter);
    const auto sequential = originator::queue_rejection_reason(originator::aperture::sequential);
    const auto reduce = originator::queue_rejection_reason(originator::aperture::reduce);
    CHECK(scatter != sequential);
    CHECK(sequential != reduce);
    CHECK(scatter != reduce);
  }

  SUBCASE("a reduction inside the queue") {
    auto queue = climate_queue(cells, grid_count);
    originator::parameters empty;
    queue.calls.push_back(tool_call("reduce_max", {readable(cells, "height")}, {}, empty, grid_count));
    const auto check = originator::check_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("reduce") != std::string::npos);
  }

  SUBCASE("a gather whose source is its own target") {
    originator::parameters blur;
    blur.set_number("width", double(grid_width));
    blur.set_number("radius", 2);

    originator::computation_queue queue;
    queue.name = "climate";
    queue.calls.push_back(
      tool_call("box_blur", {readable(cells, "height")}, {writable(cells, "height")}, blur, grid_count));
    queue.output.push_back(writable(cells, "height"));

    const auto check = originator::check_queue(queue);
    CHECK_FALSE(check.allowed);
    // Проверка привязок у очереди та же, что у одиночного вызова, поэтому сообщение то же, плюс
    // номер элемента: очередь исполняется в одном месте, и без номера непонятно, кто её собрал.
    CHECK(check.message.find("queue element 1 of 1") != std::string::npos);
  }
}

TEST_CASE("originator queue names dead work before it runs") {
  auto cells = make_cells(grid_count);
  fill_noise(cells, grid_count);

  originator::parameters blur;
  blur.set_number("width", double(grid_width));
  blur.set_number("radius", 2);

  originator::parameters scale;
  scale.set_number("scale", 2.0);

  SUBCASE("a pass nobody reads and output does not name") {
    auto queue = climate_queue(cells, grid_count);
    // Считает поле, которое ни один следующий элемент не читает и которого нет в output.
    queue.calls.push_back(
      tool_call("remap", {readable(cells, "height")}, {writable(cells, "moisture")}, scale, grid_count));

    const auto check = originator::check_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("dead") != std::string::npos);
  }

  SUBCASE("a pass whose result the next one overwrites without reading it") {
    originator::computation_queue queue;
    queue.name = "climate";
    queue.calls.push_back(
      tool_call("box_blur", {readable(cells, "height")}, {writable(cells, "smoothed")}, blur, grid_count));
    // Затирает `smoothed` целиком, не прочитав его: первый проход посчитан впустую.
    queue.calls.push_back(
      tool_call("remap", {readable(cells, "moisture")}, {writable(cells, "smoothed")}, scale, grid_count));
    queue.output.push_back(writable(cells, "smoothed"));

    const auto check = originator::check_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("queue element 1") != std::string::npos);
  }

  SUBCASE("two passes writing DIFFERENT halves of one field are both alive") {
    originator::computation_queue queue;
    queue.name = "climate";

    auto first = tool_call("remap", {readable(cells, "height")}, {writable(cells, "smoothed")}, scale, grid_count);
    first.range_begin = 0;
    first.range_end = grid_count / 2;

    auto second = tool_call("remap", {readable(cells, "moisture")}, {writable(cells, "smoothed")}, scale, grid_count);
    second.range_begin = grid_count / 2;
    second.range_end = grid_count;

    queue.calls.push_back(std::move(first));
    queue.calls.push_back(std::move(second));
    queue.output.push_back(writable(cells, "smoothed"));

    // Перекрытие проверяется по ПОКРЫТИЮ диапазона, а не по имени поля: иначе честный случай
    // «каждый вызов пишет свой отрезок» отклонялся бы как мёртвый.
    CHECK(originator::check_queue(queue).allowed);
  }
}

namespace {
// Буфер-счётчик: обычный буфер на один элемент, ровно как `state.vertex_count` у GN03. Отдельного
// механизма для «сколько всего получилось» не заводилось и здесь не заводится.
originator::buffer make_counter() {
  const std::vector<field_pair> fields = {{"used", "ui1"}, {"broken", "v1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "state");
  return originator::buffer("state", std::move(layout), 1);
}

void set_counter(originator::buffer& state, const double value) {
  state.field(state.find_field("used")).set(0, value);
}

originator::queue_call counted_call(originator::buffer& cells,
                                   originator::buffer& state,
                                   const std::string_view& tool,
                                   const std::string_view& source,
                                   const std::string_view& target) {
  originator::parameters scale;
  scale.set_number("scale", 2.0);

  auto call = tool_call(tool, {readable(cells, source)}, {writable(cells, target)}, scale, grid_count);
  call.range_end = 0;
  call.count_from = readable(state, "used");
  return call;
}
} // namespace

TEST_CASE("originator queue counts elements from a field instead of a number") {
  auto cells = make_cells(grid_count);
  auto state = make_counter();
  fill_noise(cells, grid_count);

  originator::computation_queue queue;
  queue.name = "counted";
  queue.calls.push_back(counted_call(cells, state, "remap", "height", "smoothed"));
  queue.output.push_back(writable(cells, "smoothed"));
  REQUIRE(originator::check_queue(queue).allowed);
  CHECK(queue.calls[0].indirect());

  // Счётчик читается В МОМЕНТ ИСПОЛНЕНИЯ, а не при объявлении: одна и та же объявленная очередь
  // обрабатывает разное число элементов, потому что число ей приносит буфер.
  const auto height = cells.field(cells.find_field("height"));
  const auto smoothed = cells.field(cells.find_field("smoothed"));

  for (const size_t requested : {size_t(0), size_t(1), size_t(37), grid_count}) {
    cells.field(cells.find_field("smoothed")).set(0, 0.0);
    for (size_t i = 0; i < grid_count; ++i) {
      cells.field(cells.find_field("smoothed")).set(i, -1.0);
    }
    set_counter(state, double(requested));

    const auto report = originator::run_queue(queue, nullptr);
    CHECK(report.clamped == 0);

    size_t processed = 0;
    while (processed < grid_count && smoothed.get(processed) != -1.0) {
      ++processed;
    }
    CHECK(processed == requested);
    for (size_t i = 0; i < requested; ++i) {
      CHECK(smoothed.get(i) == doctest::Approx(height.get(i) * 2.0));
    }
  }
}

TEST_CASE("originator queue clamps a count that exceeds the capacity and says so") {
  auto cells = make_cells(grid_count);
  auto state = make_counter();
  fill_noise(cells, grid_count);

  originator::computation_queue queue;
  queue.name = "counted";
  queue.calls.push_back(counted_call(cells, state, "remap", "height", "smoothed"));
  queue.output.push_back(writable(cells, "smoothed"));

  // Зажим, а не отказ: на устройстве бросить нечем, и если бы CPU отказывал, у одного объявления
  // было бы два поведения. Зато факт зажима возвращается наружу — тихо обрезанный проход не
  // отследить по результату.
  set_counter(state, double(grid_count * 4));
  const auto report = originator::run_queue(queue, nullptr);
  CHECK(report.clamped == 1);

  const auto height = cells.field(cells.find_field("height"));
  const auto smoothed = cells.field(cells.find_field("smoothed"));
  for (size_t i = 0; i < grid_count; ++i) {
    CHECK(smoothed.get(i) == doctest::Approx(height.get(i) * 2.0));
  }
}

TEST_CASE("originator queue fuses counted calls only when they share the counter") {
  auto cells = make_cells(grid_count);
  auto state = make_counter();
  auto other = make_counter();
  fill_noise(cells, grid_count);
  set_counter(state, double(grid_count));
  set_counter(other, double(grid_count));

  SUBCASE("one counter for both") {
    originator::computation_queue queue;
    queue.name = "counted";
    queue.calls.push_back(counted_call(cells, state, "remap", "height", "smoothed"));
    queue.calls.push_back(counted_call(cells, state, "remap", "smoothed", "moisture"));
    queue.output.push_back(writable(cells, "moisture"));

    const auto report = originator::run_queue(queue, nullptr);
    // Равенство диапазонов выполняется ПО ПОСТРОЕНИЮ, а не по совпадению чисел.
    CHECK(report.fused == 2);
    CHECK(report.passes == 1);
  }

  SUBCASE("two counters holding the same number") {
    originator::computation_queue queue;
    queue.name = "counted";
    queue.calls.push_back(counted_call(cells, state, "remap", "height", "smoothed"));
    queue.calls.push_back(counted_call(cells, other, "remap", "smoothed", "moisture"));
    queue.output.push_back(writable(cells, "moisture"));

    // Числа сейчас равны, но проверить это до исполнения нечем, а слияние требует РАВНЫХ диапазонов.
    const auto report = originator::run_queue(queue, nullptr);
    CHECK(report.fused == 0);
    CHECK(report.passes == 2);
  }
}

TEST_CASE("originator queue refuses a count it cannot trust") {
  auto cells = make_cells(grid_count);
  auto state = make_counter();
  fill_noise(cells, grid_count);
  set_counter(state, 8.0);

  SUBCASE("a fractional counter field") {
    originator::computation_queue queue;
    queue.name = "counted";
    auto call = counted_call(cells, state, "remap", "height", "smoothed");
    call.count_from = readable(state, "broken");
    queue.calls.push_back(std::move(call));
    queue.output.push_back(writable(cells, "smoothed"));

    const auto check = originator::check_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("integer") != std::string::npos);
  }

  SUBCASE("a counter written LATER in the same queue") {
    originator::parameters index_params;
    index_params.set_number("scale", 1.0);

    originator::computation_queue queue;
    queue.name = "counted";
    queue.calls.push_back(counted_call(cells, state, "remap", "height", "smoothed"));
    // Этот вызов пишет счётчик ПОСЛЕ того, как его прочитали: значение было бы с прошлого раза.
    queue.calls.push_back(
      tool_call("index", {}, {originator::field_ref{&state, &state, state.find_field("used")}}, index_params, 1));
    queue.output.push_back(writable(cells, "smoothed"));

    const auto check = originator::check_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("LATER") != std::string::npos);
  }
}
