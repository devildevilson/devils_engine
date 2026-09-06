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
    r.add_graph_tools();
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
    params.set_number("width", 1.0);

    // `value_noise` устройственной формы не объявляет: у площадки на CPU он есть, а на устройстве
    // нет, и это законный ответ, а не дефект. Инструмент здесь важен только тем, что тела у него нет.
    const std::vector<field_pair> plain = {{"height", "v1"}};
    auto layout = originator::make_buffer_layout(originator::storage_kind::soa, plain, "cells");
    originator::buffer other("cells", std::move(layout), grid_count);

    originator::computation_queue queue;
    queue.name = "bodiless";
    queue.calls.push_back(tool_call("value_noise", {}, {writable(other, "height")}, params));
    queue.output.push_back(writable(other, "height"));

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

// =============================================================================================
// РОД РЕСУРСА ВЫВОДИТСЯ (§6.2/6.3).
//
// Проверяется здесь не «картинка работает», а то, что решение о ней ПРИНЯТО ПЛАНОМ и принято по
// единственному основанию: кто-то читает это поле между элементами. План, разложивший в картинки всё
// подряд, считает то же самое и стоит дороже — по результату этого не видно.
// =============================================================================================

namespace {
void copy_body(const originator::queue_call& call, const std::string_view&, thread::atomic_pool*) {
  const auto source = call.inputs[0].read();
  const auto target = call.outputs[0].write();
  for (size_t i = call.range_begin; i < call.range_end; ++i) {
    target.set(i, source.get(i));
  }
}

// РАДИУС НОЛЬ — не вырожденный случай, а единственная точка, где у фильтрованного чтения есть ТОЧНЫЙ
// эталон: все четыре отсчёта попадают в центр текселя, а там билинейный фильтр ничего не смешивает и
// обязан вернуть сам тексель. Всё остальное у фильтра implementation-defined (§6.3), и сверять его
// побитово нельзя ни с чем.
originator::queue_call blur_call(originator::buffer& cells,
                                 const std::string_view& from,
                                 const std::string_view& to,
                                 const double radius) {
  const auto* tool = registry().find("filtered_blur");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("radius", radius);

  originator::queue_call call;
  call.label = "filtered_blur";
  call.tool = tool;
  call.shape = tool->shape;
  call.inputs = {readable(cells, from)};
  call.outputs = {writable(cells, to)};
  call.params = std::move(params);
  call.range_end = grid_count;
  return call;
}
} // namespace

TEST_CASE("originator derives which field becomes an image from how the queue reads it") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  auto on_cpu = make_cells();
  auto on_device = make_cells();

  const auto chain = [](originator::buffer& cells) {
    originator::parameters scale;
    scale.set_number("scale", 1.5);
    scale.set_number("offset", -0.25);

    originator::computation_queue queue;
    queue.name = "sampled";
    queue.calls.push_back(tool_call("remap", {readable(cells, "height")}, {writable(cells, "smoothed")}, scale));
    queue.calls.push_back(blur_call(cells, "smoothed", "mixed", 0.0));
    // `smoothed` названо в выходе НАМЕРЕННО: оно картинка, и значит план обязан уметь привезти
    // картинку обратно. Внутри очереди такая передача не платится вовсе — здесь она на границе.
    queue.output.push_back(writable(cells, "smoothed"));
    queue.output.push_back(writable(cells, "mixed"));
    return queue;
  };

  run_on_cpu(chain(on_cpu));

  painter::compute_context_config config;
  config.app_name = "device_queue_images";
  painter::compute_context ctx(config);
  originator::device_queue plan(ctx, chain(on_device));

  // РОВНО ОДНО поле стало картинкой — то, которое читают фильтром. Ни вход очереди, ни выход, который
  // читают по своему индексу, род не меняют: по всем прочим пунктам буфер лучше.
  CHECK(plan.image_count() == 1);
  CHECK(plan.is_image("cells.smoothed"));
  CHECK_FALSE(plan.is_image("cells.height"));
  CHECK_FALSE(plan.is_image("cells.mixed"));

  const std::vector<std::string> expected_upload = {"cells.height"};
  CHECK(plan.uploaded_fields() == expected_upload);

  const auto report = plan.run();
  CHECK(report.images == 1);

  // Картинка, объявленная выходом, приехала обратно — и приехала целой.
  const double image_worst = worst_difference(on_cpu, on_device, "smoothed");
  std::printf("  image round trip: worst deviation %.3g\n", image_worst);
  CHECK(image_worst < 1e-6);

  // Выборка в ЦЕНТРЕ текселя обязана вернуть сам тексель: фильтр там ничего не смешивает, и это
  // единственная точка, где у фильтрованного чтения есть точный эталон.
  const double centre_worst = worst_difference(on_cpu, on_device, "mixed");
  std::printf("  filtered read at the texel centre: worst deviation %.3g\n", centre_worst);
  CHECK(centre_worst < 1e-6);
}

TEST_CASE("originator refuses a filtered read it cannot honour, and says why") {
  auto cells = make_cells();

  SUBCASE("a filter over integers") {
    const std::vector<field_pair> fields = {{"label", "ui1"}, {"result", "v1"}};
    auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "labels");
    originator::buffer labels("labels", std::move(layout),
                              originator::buffer_extent{grid_width, grid_width, 0});

    originator::computation_queue queue;
    queue.name = "integer_filter";
    queue.calls.push_back(blur_call(labels, "label", "result", 1.0));
    queue.output.push_back(writable(labels, "result"));

    const auto check = originator::check_device_queue(queue);
    CHECK_FALSE(check.allowed);
    // Среднее двух номеров области — не номер области, и это не придирка к типу, а к смыслу.
    CHECK(check.message.find("not a label") != std::string::npos);
  }

  SUBCASE("a filter over a buffer with no shape") {
    const std::vector<field_pair> fields = {{"height", "v1"}, {"result", "v1"}};
    auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "linear");
    originator::buffer linear("linear", std::move(layout), grid_count);

    originator::computation_queue queue;
    queue.name = "shapeless_filter";
    queue.calls.push_back(blur_call(linear, "height", "result", 1.0));
    queue.output.push_back(writable(linear, "result"));

    const auto check = originator::check_device_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("extent") != std::string::npos);
  }

  SUBCASE("an image of another shape addressed by index") {
    // Индекс сворачивается по форме ВЫЗОВА, поэтому картинку другой формы им не прочитать: она
    // вернула бы не тот тексель и не пожаловалась бы. Читать её можно выборкой — тем самым чтением,
    // ради которого она картинкой и стала.
    const std::vector<field_pair> coarse_fields = {{"height", "v1"}, {"result", "v1"}};
    auto coarse_layout = originator::make_buffer_layout(originator::storage_kind::soa, coarse_fields, "coarse");
    originator::buffer coarse("coarse", std::move(coarse_layout),
                              originator::buffer_extent{grid_width / 2, grid_width / 2, 0});

    const size_t coarse_count = (grid_width / 2) * (grid_width / 2);

    originator::computation_queue queue;
    queue.name = "mixed_shapes";
    // Первый вызов делает `coarse.height` картинкой, читая его фильтром.
    auto filtered = blur_call(coarse, "height", "result", 1.0);
    filtered.range_end = coarse_count;
    queue.calls.push_back(std::move(filtered));

    // Второй читает ту же картинку ПО ИНДЕКСУ, но пишет в буфер ДРУГОЙ формы, а свернуть индекс
    // можно только по форме вызова.
    originator::parameters scale;
    scale.set_number("scale", 2.0);
    auto by_index = tool_call("remap", {readable(coarse, "height")}, {writable(cells, "smoothed")}, scale);
    by_index.range_end = coarse_count;
    queue.calls.push_back(std::move(by_index));
    queue.output.push_back(writable(coarse, "result"));
    queue.output.push_back(writable(cells, "smoothed"));

    const auto check = originator::check_device_queue(queue);
    CHECK_FALSE(check.allowed);
    CHECK(check.message.find("wrong texel") != std::string::npos);
  }
}

TEST_CASE("originator refuses a native tool over a field it was not written for") {
  // ЛАТЕНТНЫЙ ДЕФЕКТ, закрытый вместе со сборкой привязок: прежде текст инструмента объявлял
  // `float data[]` у ЛЮБОГО поля, поэтому вызов над `ui1` читал БИТЫ как float. На CPU то же поле
  // читается аксессором и даёт значение — то есть два пути молча считали разное.
  const std::vector<field_pair> fields = {{"label", "ui1"}, {"result", "v1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "labels");
  originator::buffer labels("labels", std::move(layout), grid_count);

  originator::parameters scale;
  scale.set_number("scale", 2.0);

  originator::computation_queue queue;
  queue.name = "integer_tool";
  queue.calls.push_back(tool_call("remap", {readable(labels, "label")}, {writable(labels, "result")}, scale));
  queue.output.push_back(writable(labels, "result"));

  const auto check = originator::check_device_queue(queue);
  CHECK_FALSE(check.allowed);
  CHECK(check.message.find("BITS") != std::string::npos);
}

TEST_CASE("originator device plan repeats the reason a foreign body refused") {
  // Чужое тело попадает на устройство ТОЛЬКО переводом `devils_script`. Когда перевод отказал,
  // причина обязана доехать до плана: «на устройство не переносится» без объяснения означало бы, что
  // автор ищет её сам, а искать надо в трансляторе.
  auto cells = make_cells();

  originator::queue_call call;
  call.label = "untranslatable";
  call.body = copy_body;
  call.shape = originator::aperture::pointwise;
  call.inputs = {readable(cells, "height")};
  call.outputs = {writable(cells, "smoothed")};
  call.range_end = grid_count;
  call.device = originator::translated_form::refused("'chance' has no salt in the AST");

  originator::computation_queue queue;
  queue.name = "refused";
  queue.calls.push_back(std::move(call));
  queue.output.push_back(writable(cells, "smoothed"));

  const auto check = originator::check_device_queue(queue);
  CHECK_FALSE(check.allowed);
  CHECK(check.message.find("no salt in the AST") != std::string::npos);
}

TEST_CASE("originator device plan refuses a range it would silently read as zero") {
  // КОСВЕННЫЙ ДИАПАЗОН на устройстве — это `dispatchIndirect`, и его пока нет. Прежде такой вызов
  // получал `range_count() == 0` и не делал НИЧЕГО: поле оставалось прежним, а прежним оно бывает и
  // без того, поэтому по результату этого не видно.
  auto cells = make_cells();

  const std::vector<field_pair> fields = {{"used", "ui1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "state");
  originator::buffer state("state", std::move(layout), size_t(1));
  state.field(state.find_field("used")).set(0, double(grid_count / 2));

  originator::parameters scale;
  scale.set_number("scale", 2.0);

  auto call = tool_call("remap", {readable(cells, "height")}, {writable(cells, "smoothed")}, scale);
  call.range_end = 0;
  call.count_from = readable(state, "used");

  originator::computation_queue queue;
  queue.name = "counted";
  queue.calls.push_back(std::move(call));
  queue.output.push_back(writable(cells, "smoothed"));

  // На CPU такая очередь законна и работает — путь на устройство отказывает отдельно.
  CHECK(originator::check_queue(queue).allowed);

  const auto check = originator::check_device_queue(queue);
  CHECK_FALSE(check.allowed);
  CHECK(check.message.find("indirect dispatch") != std::string::npos);
}

TEST_CASE("originator keeps a narrow field on the device as a widened copy, and the two paths agree") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the widened copy is not checked here");
    return;
  }

  // КОПИЯ НА УСТРОЙСТВЕ — КЭШ, А КЭШ ВПРАВЕ БЫТЬ ШИРЕ ИСТИНЫ. Байтового буфера у шейдера нет, но это
  // не повод отказывать полю `ub1`: на устройстве оно живёт как `uint`, а преобразование на границе
  // делают те же аксессоры, что и весь CPU-путь. Проверяется именно СОВПАДЕНИЕ ПУТЕЙ, и на целых оно
  // обязано быть побитовым — расхождение здесь означало бы, что расширение считает не то.
  const std::vector<field_pair> fields = {
    {"height", "v1"}, {"label", "ub1"}, {"copy", "ub1"}, {"mark", "us1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout), originator::buffer_extent{grid_width, grid_width, 0});

  auto height = cells.field(cells.find_field("height"));
  auto label = cells.field(cells.find_field("label"));
  for (size_t i = 0; i < grid_count; ++i) {
    height.set(i, double(i % 97));
    label.set(i, double(i % 256));
  }

  originator::parameters copy;
  copy.set_number("scale", 1.0);
  originator::parameters wide;
  // ЗАЖИМ ПРОВЕРЯЕТСЯ НАРОЧНО: `label` доходит до 255, а множитель 300 уводит результат далеко за
  // границу байта. На хосте `store_component` зажимает, и расширенная копия обязана зажать так же —
  // иначе поле, оставшееся на устройстве, несло бы значение, невозможное на CPU.
  wide.set_number("scale", 300.0);

  originator::computation_queue queue;
  queue.name = "widened";
  queue.calls.push_back(tool_call("remap", {readable(cells, "label")}, {writable(cells, "copy")}, copy));
  queue.calls.push_back(tool_call("remap", {readable(cells, "label")}, {writable(cells, "mark")}, wide));
  queue.output.push_back(writable(cells, "copy"));
  queue.output.push_back(writable(cells, "mark"));

  // `remap` объявил себя годным для любого рода, иначе план отклонил бы вызов над целым полем.
  const auto check = originator::check_device_queue(queue);
  REQUIRE_MESSAGE(check.allowed, check.message);

  auto expected = cells;
  run_on_cpu(queue);
  std::vector<double> cpu_copy(grid_count);
  std::vector<double> cpu_mark(grid_count);
  const auto host_copy = cells.field(cells.find_field("copy"));
  const auto host_mark = cells.field(cells.find_field("mark"));
  for (size_t i = 0; i < grid_count; ++i) {
    cpu_copy[i] = host_copy.get(i);
    cpu_mark[i] = host_mark.get(i);
  }

  painter::compute_context_config config;
  config.app_name = "device_widened_test";
  painter::compute_context context(config);
  originator::device_queue plan(context, queue);

  // Передача считается по УСТРОЙСТВЕННОМУ размеру: байтовое поле едет вчетверо шире своего хостового,
  // и это объявленная цена расширения, а не просчёт.
  CHECK(plan.upload_byte_count() == grid_count * sizeof(uint32_t));

  auto device_cells = expected;
  originator::computation_queue device_queue_copy = queue;
  for (auto& call : device_queue_copy.calls) {
    for (auto& binding : call.inputs) binding = readable(device_cells, binding.field_name());
    for (auto& binding : call.outputs) binding = writable(device_cells, binding.field_name());
  }
  for (auto& binding : device_queue_copy.output) {
    binding = writable(device_cells, binding.field_name());
  }
  originator::device_queue device_plan(context, device_queue_copy);
  device_plan.run();

  const auto gpu_copy = device_cells.field(device_cells.find_field("copy"));
  const auto gpu_mark = device_cells.field(device_cells.find_field("mark"));
  size_t differences = 0;
  size_t clamped = 0;
  for (size_t i = 0; i < grid_count; ++i) {
    differences += size_t(gpu_copy.get(i) != cpu_copy[i]) + size_t(gpu_mark.get(i) != cpu_mark[i]);
    clamped += size_t(cpu_mark[i] == 65535.0);
  }

  // ПОБИТОВО: значения целые и небольшие, поэтому обещать здесь можно именно совпадение, а не
  // близость.
  CHECK(differences == 0);
  // И зажим действительно случился, иначе проверка выше ничего не проверяла бы.
  CHECK(clamped > 0);
}

TEST_CASE("originator device bodies of the graph tools agree with the CPU ones") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the graph bodies are not compared here");
    return;
  }

  // ГРАФ — ДРУГОЙ СПОСОБ АДРЕСОВАТЬ ПРОСТРАНСТВО, и на устройстве он выражается тремя буферами и
  // одним циклом. Сверяется не «шейдер работает», а то, что цикл по CSR читает ТЕХ ЖЕ соседей: у
  // сбитого смещения результат остаётся правдоподобным полем, и по картинке этого не видно.
  constexpr size_t nodes = 4096;
  constexpr size_t degree = 6;

  const std::vector<field_pair> graph_fields = {{"start", "ui1"}};
  auto offsets_layout = originator::make_buffer_layout(originator::storage_kind::soa, graph_fields, "offsets");
  originator::buffer offsets("offsets", std::move(offsets_layout), nodes + 1);

  const std::vector<field_pair> arc_fields = {{"node", "ui1"}};
  auto arcs_layout = originator::make_buffer_layout(originator::storage_kind::soa, arc_fields, "arcs");
  originator::buffer arcs("arcs", std::move(arcs_layout), nodes * degree);

  const std::vector<field_pair> value_fields = {{"value", "v1"}, {"blurred", "v1"}, {"label", "ub1"},
                                                {"edge", "ub1"}, {"picked", "v1"}};
  auto cells_layout = originator::make_buffer_layout(originator::storage_kind::soa, value_fields, "cells");
  originator::buffer cells("cells", std::move(cells_layout), nodes);

  // Кольцо со степенью 6: соседство симметрично по построению, как того и требует контракт графа.
  auto start = offsets.field(offsets.find_field("start"));
  auto arc = arcs.field(arcs.find_field("node"));
  for (size_t i = 0; i <= nodes; ++i) {
    start.set(i, double(i * degree));
  }
  for (size_t i = 0; i < nodes; ++i) {
    for (size_t k = 0; k < degree; ++k) {
      const int64_t shift = int64_t(k) - int64_t(degree / 2) - (k >= degree / 2 ? 0 : 1);
      arc.set(i * degree + k, double((int64_t(i) + shift + int64_t(nodes)) % int64_t(nodes)));
    }
  }

  auto value = cells.field(cells.find_field("value"));
  auto label = cells.field(cells.find_field("label"));
  for (size_t i = 0; i < nodes; ++i) {
    value.set(i, std::sin(double(i) * 0.05));
    label.set(i, double((i / 64) % 5));
  }

  originator::parameters blur;
  blur.set_number("self_weight", 1.0);
  blur.set_number("neighbour_weight", 0.5);
  originator::parameters frontier;
  frontier.set_number("ignore", 0.0);
  originator::parameters pick;
  pick.set_number("offset", 0.0);
  pick.set_number("missing", -1.0);

  const auto make = [&](originator::buffer& o, originator::buffer& a, originator::buffer& c) {
    const auto read_o = [&](const std::string_view& n) { return readable(o, n); };
    originator::computation_queue queue;
    queue.name = "graph";
    auto blur_call = tool_call("graph_blur", {read_o("start"), readable(a, "node"), readable(c, "value")},
                               {writable(c, "blurred")}, blur);
    blur_call.range_end = nodes;
    auto frontier_call = tool_call("graph_frontier", {read_o("start"), readable(a, "node"), readable(c, "label")},
                                   {writable(c, "edge")}, frontier);
    frontier_call.range_end = nodes;
    auto lookup_call = tool_call("lookup", {readable(c, "label"), readable(c, "value")},
                                 {writable(c, "picked")}, pick);
    lookup_call.range_end = nodes;
    queue.calls.push_back(std::move(blur_call));
    queue.calls.push_back(std::move(frontier_call));
    queue.calls.push_back(std::move(lookup_call));
    queue.output.push_back(writable(c, "blurred"));
    queue.output.push_back(writable(c, "edge"));
    queue.output.push_back(writable(c, "picked"));
    return queue;
  };

  auto device_cells = cells;
  const auto cpu_queue = make(offsets, arcs, cells);
  const auto check = originator::check_device_queue(cpu_queue);
  REQUIRE_MESSAGE(check.allowed, check.message);
  run_on_cpu(cpu_queue);

  painter::compute_context_config config;
  config.app_name = "device_graph_test";
  painter::compute_context context(config);
  originator::device_queue plan(context, make(offsets, arcs, device_cells));
  plan.run();

  // Размытие плавающее, поэтому сверяется в пределах float; граница и выборка по индексу —
  // ЦЕЛОЧИСЛЕННЫЕ решения, и там расхождение означало бы чтение не тех соседей.
  CHECK(worst_difference(cells, device_cells, "blurred") < 1.0e-6);

  size_t edge_differences = 0;
  size_t picked_differences = 0;
  size_t edges = 0;
  const auto cpu_edge = cells.field(cells.find_field("edge"));
  const auto gpu_edge = device_cells.field(device_cells.find_field("edge"));
  const auto cpu_picked = cells.field(cells.find_field("picked"));
  const auto gpu_picked = device_cells.field(device_cells.find_field("picked"));
  for (size_t i = 0; i < nodes; ++i) {
    edge_differences += size_t(cpu_edge.get(i) != gpu_edge.get(i));
    picked_differences += size_t(cpu_picked.get(i) != gpu_picked.get(i));
    edges += size_t(cpu_edge.get(i) != 0.0);
  }
  CHECK(edge_differences == 0);
  CHECK(picked_differences == 0);
  // И граница обязана быть содержательной: без единой границы сверка ничего не проверяет.
  CHECK(edges > 0);
}
