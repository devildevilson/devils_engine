#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/device_queue.h"

using namespace devils_engine;

// ОЧЕРЕДЬ, СОСТАВЛЕННАЯ ДЛЯ УСТРОЙСТВА ИЗ САМОЙ СЕБЯ.
//
// Проверяется здесь не «шейдер работает» — это уже проверено раньше, — а то, что план ВЫВЕДЕН: какие
// поля загрузились, какие уехали обратно, сколько барьеров поставлено. План, загрузивший лишнее поле
// или поставивший лишний барьер, работает точно так же, и по результату этого не видно.
//
// И главное: тот же объявленный набор вызовов даёт на двух путях ОДИН результат. Не побитово — §4.2
// этого не обещает и обещать не будет, — но в пределах float, и на классификации это проверяется
// отдельно.

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t grid_width = 512;
constexpr size_t grid_count = grid_width * grid_width;

originator::tool_registry& registry() {
  static originator::tool_registry r;
  if (r.size() == 0) {
    r.add_standard_tools();
  }
  return r;
}

originator::buffer make_cells() {
  const std::vector<field_pair> fields = {
    {"height", "v1"}, {"moisture", "v1"}, {"smoothed", "v1"}, {"mixed", "v1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout),
                           originator::buffer_extent{grid_width, grid_width, 0});

  auto height = cells.field(cells.find_field("height"));
  auto moisture = cells.field(cells.find_field("moisture"));
  for (size_t i = 0; i < grid_count; ++i) {
    height.set(i, std::sin(double(i) * 0.0013) * 0.5 + 0.5);
    moisture.set(i, std::cos(double(i) * 0.0007) * 0.5 + 0.5);
  }
  return cells;
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

originator::queue_call tool_call(const std::string_view& name,
                                 std::vector<originator::field_ref> inputs,
                                 std::vector<originator::field_ref> outputs,
                                 originator::parameters params) {
  const auto* tool = registry().find(name);
  REQUIRE(tool != nullptr);

  originator::queue_call call;
  call.label.assign(name);
  call.tool = tool;
  call.shape = tool->shape;
  call.inputs = std::move(inputs);
  call.outputs = std::move(outputs);
  call.params = std::move(params);
  call.seed = 1;
  call.range_begin = 0;
  call.range_end = grid_count;
  return call;
}

// Цепочка, у которой ВСЕ три свойства плана видны: два входа, промежуточное поле, объявленный выход.
// `smoothed` пишется первым проходом и читается вторым, значит загружать его не надо, а барьер между
// ними нужен — и то, и другое обязано вывестись, а не быть сказанным.
originator::computation_queue make_chain(originator::buffer& cells) {
  originator::parameters scale;
  scale.set_number("scale", 1.5);
  scale.set_number("offset", -0.25);

  originator::parameters mix;
  mix.set_number("first", 0.5);
  mix.set_number("second", 0.5);

  originator::computation_queue queue;
  queue.name = "chain";
  queue.calls.push_back(tool_call("remap", {readable(cells, "height")}, {writable(cells, "smoothed")}, scale));
  queue.calls.push_back(tool_call("blend", {readable(cells, "smoothed"), readable(cells, "moisture")},
                                  {writable(cells, "mixed")}, mix));
  queue.output.push_back(writable(cells, "mixed"));
  return queue;
}

void run_on_cpu(const originator::computation_queue& queue) {
  originator::run_queue(queue, nullptr);
}

double worst_difference(const originator::buffer& left, const originator::buffer& right,
                        const std::string_view& field) {
  const auto a = left.field(left.find_field(field));
  const auto b = right.field(right.find_field(field));
  double worst = 0.0;
  for (size_t i = 0; i < grid_count; ++i) {
    worst = std::max(worst, std::abs(a.get(i) - b.get(i)));
  }
  return worst;
}
} // namespace

TEST_CASE("originator derives the device plan from the queue itself") {
  auto cells = make_cells();
  const auto queue = make_chain(cells);

  // Проверка плана НЕ требует устройства: и передача, и барьеры выводятся из объявленной очереди, а
  // не спрашиваются у драйвера. Поэтому эта часть работает всюду.
  const auto check = originator::check_device_queue(queue);
  CHECK(check.allowed);

  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the plan is not executed here");
    return;
  }

  painter::compute_context_config config;
  config.app_name = "device_queue_test";
  painter::compute_context ctx(config);
  originator::device_queue plan(ctx, queue);

  // ВХОДЫ ВЫВЕДЕНЫ: загружаются `height` и `moisture` — их читают, но никто в очереди не пишет.
  // `smoothed` НЕ загружается, потому что его пишет первый проход, и грузить его значило бы платить
  // за байты, которые всё равно будут перезаписаны.
  const auto& uploaded = plan.uploaded_fields();
  CHECK(uploaded.size() == 2);
  CHECK(std::find(uploaded.begin(), uploaded.end(), "cells.height") != uploaded.end());
  CHECK(std::find(uploaded.begin(), uploaded.end(), "cells.moisture") != uploaded.end());
  CHECK(std::find(uploaded.begin(), uploaded.end(), "cells.smoothed") == uploaded.end());

  // ВЫХОД ОБЪЯВЛЕН: обратно едет ровно то, что названо в `output`. `smoothed` посчитан и остался на
  // устройстве — он промежуточный, и снаружи он никому не нужен.
  const auto& downloaded = plan.downloaded_fields();
  REQUIRE(downloaded.size() == 1);
  CHECK(downloaded.front() == "cells.mixed");

  // БАРЬЕР ВЫВЕДЕН: второй проход читает то, что записал первый, — значит один барьер между ними.
  CHECK(plan.barrier_count() == 1);
}

TEST_CASE("originator device queue computes what the CPU queue computes") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  auto on_cpu = make_cells();
  auto on_device = make_cells();

  run_on_cpu(make_chain(on_cpu));

  painter::compute_context_config config;
  config.app_name = "device_queue_agree";
  painter::compute_context ctx(config);
  const auto queue = make_chain(on_device);
  originator::device_queue plan(ctx, queue);
  const auto report = plan.run();

  CHECK(report.calls == 2);
  CHECK(report.barriers == 1);
  CHECK(report.upload_bytes == 2 * grid_count * sizeof(float));
  CHECK(report.download_bytes == grid_count * sizeof(float));

  // Сходство, а не побитовое равенство: CPU считает в double, шейдер во float32. Обещать ноль здесь
  // никто не будет, а величина расхождения на умножении со сложением — единицы последних бит.
  const double worst = worst_difference(on_cpu, on_device, "mixed");
  std::printf("\n  device queue vs cpu queue: worst deviation %.3g\n", worst);
  CHECK(worst < 1e-6);

  // Промежуточное поле на хосте НЕ ТРОНУТО: оно посчиталось на устройстве и осталось там, потому что
  // в `output` его никто не назвал. Если бы план выгружал всё, это было бы незаметно — и потому
  // проверяется.
  const auto smoothed = on_device.field(on_device.find_field("smoothed"));
  bool untouched = true;
  for (size_t i = 0; i < grid_count; ++i) {
    untouched = untouched && smoothed.get(i) == 0.0;
  }
  CHECK(untouched);
}

TEST_CASE("originator device queue pays one submission, not three") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  auto cells = make_cells();
  painter::compute_context_config config;
  config.app_name = "device_queue_timing";
  painter::compute_context ctx(config);

  const auto queue = make_chain(cells);
  originator::device_queue plan(ctx, queue);

  // ОДНА И ТА ЖЕ РАБОТА ДВУМЯ СПОСОБАМИ, иначе сравнивать нечего. Варианты чередуются, читается
  // лучшее: разница здесь не в разы, а стенные часы на ноутбуке шумят.
  double best_single = 1e30;
  double best_stepwise = 1e30;
  originator::device_report report;
  for (int repeat = 0; repeat < 7; ++repeat) {
    report = plan.run();
    best_single = std::min(best_single, report.record_ms);
    const auto stepwise = plan.run_step_by_step();
    best_stepwise = std::min(best_stepwise, stepwise.record_ms);
  }

  // §5 п.3 мерил круг ТРЕМЯ отправками намеренно, чтобы приписать цену по фазам, и назвал 70% на
  // передачу оценкой СВЕРХУ. Вот пересчёт: та же работа одной отправкой против пяти (две загрузки,
  // два прохода, одна выгрузка).
  const double megabytes = double(report.upload_bytes + report.download_bytes) / (1024.0 * 1024.0);
  std::printf("  %zu calls, %zu barriers, %.2f MB moved\n", report.calls, report.barriers, megabytes);
  std::printf("  one submission %.3f ms, step by step %.3f ms (%.2fx)\n", best_single, best_stepwise,
              best_stepwise / best_single);
  CHECK(best_single > 0.0);
  // Одна отправка обязана быть не дороже: работа та же, а точек синхронизации меньше.
  CHECK(best_single <= best_stepwise);
}

TEST_CASE("originator device queue refuses what does not go to a device, and says why") {
  auto cells = make_cells();

  SUBCASE("a tool without a device form") {
    originator::parameters params;
    params.set_number("sea_level", 0.5);

    // `classify` пишет в однобайтовое поле и устройственной формы не объявляет: у площадки на CPU он
    // есть, а на устройстве его нет, и это законный ответ, а не дефект.
    const std::vector<field_pair> narrow = {{"height", "v1"}, {"moisture", "v1"}, {"biome", "ub1"}};
    auto layout = originator::make_buffer_layout(originator::storage_kind::soa, narrow, "cells");
    originator::buffer other("cells", std::move(layout), grid_count);

    originator::computation_queue queue;
    queue.name = "narrow";
    queue.calls.push_back(tool_call("classify", {readable(other, "height"), readable(other, "moisture")},
                                    {writable(other, "biome")}, params));
    queue.output.push_back(writable(other, "biome"));

    const auto check = originator::check_device_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("device form") != std::string::npos);
  }

  SUBCASE("a buffer laid out as aos") {
    const std::vector<field_pair> fields = {{"height", "v1"}, {"smoothed", "v1"}};
    auto layout = originator::make_buffer_layout(originator::storage_kind::aos, fields, "cells");
    originator::buffer interleaved("cells", std::move(layout), grid_count);

    originator::parameters scale;
    scale.set_number("scale", 2.0);

    originator::computation_queue queue;
    queue.name = "aos";
    queue.calls.push_back(tool_call("remap", {readable(interleaved, "height")},
                                    {writable(interleaved, "smoothed")}, scale));
    queue.output.push_back(writable(interleaved, "smoothed"));

    const auto check = originator::check_device_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("aos") != std::string::npos);
  }

  SUBCASE("what the CPU path refuses, the device path refuses too") {
    // Второго набора правил у устройственного плана нет: сначала работают те же проверки, что на CPU.
    auto queue = make_chain(cells);
    queue.output.clear();
    const auto check = originator::check_device_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("output") != std::string::npos);
  }
}

TEST_CASE("originator device tool defaults agree with the CPU tool defaults") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  // ЭТО И ЕСТЬ СВЕРКА, которой закреплено дублирование значений по умолчанию: у устройственной формы
  // они объявлены (`device_param::fallback`), а у тела инструмента написаны литералами. Разъехавшийся
  // default означал бы, что вызов БЕЗ параметра считает на двух путях разное — и увидеть это можно
  // только так.
  auto on_cpu = make_cells();
  auto on_device = make_cells();

  const auto bare_chain = [](originator::buffer& cells) {
    originator::computation_queue queue;
    queue.name = "bare";
    // Ни одного необязательного параметра: всё, что не сказано, обязано совпасть по умолчанию.
    queue.calls.push_back(
      tool_call("remap", {readable(cells, "height")}, {writable(cells, "smoothed")}, originator::parameters{}));
    queue.calls.push_back(tool_call("maximum", {readable(cells, "smoothed"), readable(cells, "moisture")},
                                    {writable(cells, "mixed")}, originator::parameters{}));
    queue.output.push_back(writable(cells, "mixed"));
    return queue;
  };

  run_on_cpu(bare_chain(on_cpu));

  painter::compute_context_config config;
  config.app_name = "device_queue_defaults";
  painter::compute_context ctx(config);
  originator::device_queue plan(ctx, bare_chain(on_device));
  plan.run();

  const double worst = worst_difference(on_cpu, on_device, "mixed");
  std::printf("  defaults with no parameters given: worst deviation %.3g\n", worst);
  CHECK(worst < 1e-6);
}

TEST_CASE("originator device queue puts a gather tool on the device too") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  auto on_cpu = make_cells();
  auto on_device = make_cells();

  const auto blur_chain = [](originator::buffer& cells) {
    originator::parameters blur;
    blur.set_number("radius", 2);

    originator::computation_queue queue;
    queue.name = "blur";
    // Форма растра приходит ОБЩЕЙ ШАПКОЙ из `extent` буфера: ни на CPU, ни на устройстве второго
    // способа назвать ширину не осталось.
    queue.calls.push_back(
      tool_call("box_blur", {readable(cells, "height")}, {writable(cells, "smoothed")}, blur));
    queue.output.push_back(writable(cells, "smoothed"));
    return queue;
  };

  run_on_cpu(blur_chain(on_cpu));

  painter::compute_context_config config;
  config.app_name = "device_queue_gather";
  painter::compute_context ctx(config);
  originator::device_queue plan(ctx, blur_chain(on_device));
  const auto report = plan.run();

  // Один проход — ни одного барьера: зависимости внутри очереди нет, и это следствие анализа, а не
  // оптимизация.
  CHECK(report.barriers == 0);

  const double worst = worst_difference(on_cpu, on_device, "smoothed");
  std::printf("  gather on the device vs the native tool: worst deviation %.3g\n", worst);
  CHECK(worst < 1e-5);
}
