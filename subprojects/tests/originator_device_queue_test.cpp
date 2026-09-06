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
    r.add_volume_tools();
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

TEST_CASE("originator native noise computes the same field on both paths") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the noise paths are not compared here");
    return;
  }

  // СОБСТВЕННЫЙ ШУМ ДВИЖКА, написанный ДВАЖДЫ — на C++ и на GLSL, — потому что одной реализации на два
  // пути дать нечем. Значит совпадение обязано держаться ТЕСТОМ, а не аккуратностью: расхождение здесь
  // означало бы, что мир зависит от того, где его посчитали.
  //
  // ПОБИТОВОГО РАВЕНСТВА ЗДЕСЬ НЕТ, И ОБЕЩАТЬ ЕГО НЕЛЬЗЯ (§4.2): обе стороны считают во `float32`
  // одними и теми же операциями, но драйвер вправе применять FMA и переставлять ассоциативность.
  // Измерено: типичное расхождение — ОДИН последний бит, худшее около `3e-06` на накоплении четырёх
  // октав, и около восьмой части поля сходится точно. Проверяется поэтому ПОРЯДОК величины, а не ноль:
  // ноль здесь означал бы, что сравнение сравнивает не то (так уже было — копия буфера делила память
  // с оригиналом, и «побитово» получалось само собой).
  const std::vector<field_pair> fields = {
    {"position", "v3"}, {"value", "v1"}, {"perlin", "v1"}, {"cellular", "v1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout), originator::buffer_extent{grid_width, grid_width, 0});

  auto position = cells.field(cells.find_field("position"));
  for (size_t i = 0; i < grid_count; ++i) {
    position.set(i, double(i % grid_width) * 0.37, 0);
    position.set(i, double(i / grid_width) * 0.29, 1);
    position.set(i, double(i % 13) * 0.11, 2);
  }

  originator::parameters params;
  params.set_number("frequency", 0.05);
  params.set_number("octaves", 4);
  params.set_number("lacunarity", 2.0);
  params.set_number("gain", 0.5);
  params.set_number("seed_offset", 17);

  const auto build = [&](originator::buffer& target) {
    originator::parameters grid;
    grid.set_number("cell_size", 0.37);

    originator::computation_queue queue;
    queue.name = "noise";
    // РЕШЁТКУ СТРОИТ САМО УСТРОЙСТВО: позиции приходят полем, и это поле — первый элемент очереди, а
    // не привезённые с хоста байты. Иначе цепочка рвалась бы ровно там, где начинается работа.
    queue.calls.push_back(tool_call("position_grid", {}, {writable(target, "position")}, grid));
    queue.calls.push_back(tool_call("noise_value", {readable(target, "position")},
                                    {writable(target, "value")}, params));
    queue.calls.push_back(tool_call("noise_perlin", {readable(target, "position")},
                                    {writable(target, "perlin")}, params));
    queue.calls.push_back(tool_call("noise_cellular", {readable(target, "position")},
                                    {writable(target, "cellular")}, params));
    queue.output.push_back(writable(target, "value"));
    queue.output.push_back(writable(target, "perlin"));
    queue.output.push_back(writable(target, "cellular"));
    return queue;
  };

  auto device_cells = cells;
  const auto cpu_queue = build(cells);
  const auto check = originator::check_device_queue(cpu_queue);
  REQUIRE_MESSAGE(check.allowed, check.message);
  run_on_cpu(cpu_queue);

  painter::compute_context_config config;
  config.app_name = "device_noise_test";
  painter::compute_context context(config);
  originator::device_queue plan(context, build(device_cells));
  const auto report = plan.run();

  // ЧИСЛО УСТРОЙСТВА рядом с ценой на CPU: свой шум существует ради того, чтобы очередь с шумом
  // вообще могла уехать, и знать, во что это обходится с обеих сторон, надо сразу.
  const auto cpu_start = std::chrono::steady_clock::now();
  run_on_cpu(cpu_queue);
  const auto cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpu_start).count();
  std::printf("\n  noise on %zu elements: cpu %.2f ms (one thread), device %.2f ms recorded + %.2f ms submitted\n",
              grid_count, cpu_ms, report.record_ms, report.submit_ms);

  for (const auto& name : {"value", "perlin", "cellular"}) {
    const auto worst = worst_difference(cells, device_cells, name);
    std::printf("  noise '%s': worst deviation %.3g\n", name, worst);
    // Доля ТОЧНЫХ совпадений печатается рядом с худшим расхождением, потому что одно без другого
    // вводит в заблуждение: «worst 3e-06» звучит как разные алгоритмы, а на деле восьмая часть поля
    // сходится бит в бит, а остальное расходится последним битом.
    const auto a = cells.field(cells.find_field(name));
    const auto b = device_cells.field(device_cells.find_field(name));
    size_t exact = 0;
    for (size_t i = 0; i < grid_count; ++i) {
      exact += size_t(a.get(i) == b.get(i));
    }
    std::printf("    (%.1f%% элементов совпали точно)\n", 100.0 * double(exact) / double(grid_count));
    CHECK(worst < 1.0e-5);
  }

  // И поле обязано быть НАСТОЯЩИМ полем, а не константой: совпадение двух нулей прошло бы проверку
  // выше и не значило бы ничего.
  for (const auto& name : {"value", "perlin", "cellular"}) {
    const auto accessor = cells.field(cells.find_field(name));
    double low = 1.0e30;
    double high = -1.0e30;
    for (size_t i = 0; i < grid_count; ++i) {
      low = std::min(low, accessor.get(i));
      high = std::max(high, accessor.get(i));
    }
    CHECK(high - low > 0.2);
    // Октавы нормируются, поэтому значение остаётся в объявленном диапазоне у всех трёх.
    CHECK(low >= -1.001);
    CHECK(high <= 1.001);
  }
}

TEST_CASE("originator counts into shared memory and agrees with the CPU count") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the histogram is not compared here");
    return;
  }

  // СВЁРТКА ЧЕРЕЗ АТОМИКИ: узкое место у неё не двухстадийность, а КОНКУРЕНЦИЯ — миллион элементов
  // бьётся за десятки счётчиков. Группа копит в разделяемой памяти, и в общий буфер уходит один
  // атомик на корзину на группу.
  //
  // Проверяется здесь ДВА пути сразу: узкая гистограмма (влезает в разделяемую память) и широкая
  // (не влезает, остаётся прямой атомик). Второй путь легко забыть, а ошибка в нём проявится только
  // на большом числе корзин.
  const auto measure_buckets = [&](const size_t buckets) {
    const std::vector<field_pair> fields = {{"key", "ui1"}};
    auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
    originator::buffer cells("cells", std::move(layout), originator::buffer_extent{grid_width, grid_width, 0});

    const std::vector<field_pair> bins = {{"total", "ui1"}};
    auto bin_layout = originator::make_buffer_layout(originator::storage_kind::soa, bins, "histogram");
    originator::buffer histogram("histogram", std::move(bin_layout), buckets);

    auto key = cells.field(cells.find_field("key"));
    for (size_t i = 0; i < grid_count; ++i) {
      key.set(i, double((i * 7919) % buckets));
    }

    originator::computation_queue queue;
    queue.name = "histogram";
    queue.calls.push_back(tool_call("count_by", {readable(cells, "key")}, {writable(histogram, "total")}, {}));
    queue.output.push_back(writable(histogram, "total"));

    const auto check = originator::check_device_queue(queue);
    REQUIRE_MESSAGE(check.allowed, check.message);

    auto device_histogram = histogram;
    run_on_cpu(queue);

    painter::compute_context_config config;
    config.app_name = "device_histogram_test";
    painter::compute_context context(config);

    originator::computation_queue device_copy = queue;
    device_copy.calls.front().outputs.front() = writable(device_histogram, "total");
    device_copy.output.front() = writable(device_histogram, "total");
    originator::device_queue plan(context, device_copy);
    const auto report = plan.run();

    const auto cpu = histogram.field(histogram.find_field("total"));
    const auto gpu = device_histogram.field(device_histogram.find_field("total"));
    size_t differences = 0;
    size_t total = 0;
    for (size_t i = 0; i < buckets; ++i) {
      differences += size_t(cpu.get(i) != gpu.get(i));
      total += size_t(gpu.get(i));
    }

    std::printf("  histogram of %zu elements into %zu buckets: %.2f ms recorded\n", grid_count, buckets,
                report.record_ms);
    // ПОБИТОВО: счёт целый, и порядок прихода групп на него не влияет — сложение целых коммутативно.
    CHECK(differences == 0);
    // И посчитаны ВСЕ элементы: гистограмма, потерявшая половину, совпала бы с CPU только если бы обе
    // теряли одинаково, а вот это уже проверяется суммой.
    CHECK(total == grid_count);
  };

  // Узкая: влезает в разделяемую память группы.
  measure_buckets(64);
  // Широкая: не влезает, работает запасной путь с прямым атомиком.
  measure_buckets(1024);
}

TEST_CASE("originator device bodies of the remaining gather tools agree with the CPU ones") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the remaining bodies are not compared here");
    return;
  }

  constexpr size_t nodes = 4096;
  constexpr size_t degree = 6;

  const std::vector<field_pair> graph_fields = {{"start", "ui1"}};
  auto offsets_layout = originator::make_buffer_layout(originator::storage_kind::soa, graph_fields, "offsets");
  originator::buffer offsets("offsets", std::move(offsets_layout), nodes + 1);

  const std::vector<field_pair> arc_fields = {{"node", "ui1"}};
  auto arcs_layout = originator::make_buffer_layout(originator::storage_kind::soa, arc_fields, "arcs");
  originator::buffer arcs("arcs", std::move(arcs_layout), nodes * degree);

  const std::vector<field_pair> value_fields = {{"value", "v1"},  {"label", "ub1"}, {"weight", "v1"},
                                                {"mask", "ub1"},  {"slope", "v1"},  {"voted", "ub1"}};
  auto cells_layout = originator::make_buffer_layout(originator::storage_kind::soa, value_fields, "cells");
  originator::buffer cells("cells", std::move(cells_layout), nodes);

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
  auto weight = cells.field(cells.find_field("weight"));
  auto mask = cells.field(cells.find_field("mask"));
  for (size_t i = 0; i < nodes; ++i) {
    value.set(i, std::sin(double(i) * 0.05));
    // Метка есть у части клеток: голосование только тогда и осмысленно, когда есть куда расти.
    label.set(i, (i % 7) == 0 ? double((i / 7) % 4 + 1) : 0.0);
    weight.set(i, double((i * 37) % 11) / 11.0);
    mask.set(i, 1.0);
  }

  originator::parameters vote;
  vote.set_number("threshold", 0.0);

  const auto make = [&](originator::buffer& o, originator::buffer& a, originator::buffer& c) {
    originator::computation_queue queue;
    queue.name = "gather";
    auto slope = tool_call("graph_slope", {readable(o, "start"), readable(a, "node"), readable(c, "value")},
                           {writable(c, "slope")}, {});
    slope.range_end = nodes;
    auto voted = tool_call("graph_vote",
                           {readable(o, "start"), readable(a, "node"), readable(c, "label"),
                            readable(c, "weight"), readable(c, "mask")},
                           {writable(c, "voted")}, vote);
    voted.range_end = nodes;
    queue.calls.push_back(std::move(slope));
    queue.calls.push_back(std::move(voted));
    queue.output.push_back(writable(c, "slope"));
    queue.output.push_back(writable(c, "voted"));
    return queue;
  };

  auto device_cells = cells;
  const auto cpu_queue = make(offsets, arcs, cells);
  const auto check = originator::check_device_queue(cpu_queue);
  REQUIRE_MESSAGE(check.allowed, check.message);
  run_on_cpu(cpu_queue);

  painter::compute_context_config config;
  config.app_name = "device_gather_test";
  painter::compute_context context(config);
  originator::device_queue plan(context, make(offsets, arcs, device_cells));
  plan.run();

  CHECK(worst_difference(cells, device_cells, "slope") < 1.0e-6);

  // Голосование — ВЫБОР, а не арифметика: расхождение здесь означало бы, что пути выбрали разного
  // соседа, и это обязано быть побитовым.
  size_t vote_differences = 0;
  size_t voted_cells = 0;
  const auto cpu_voted = cells.field(cells.find_field("voted"));
  const auto gpu_voted = device_cells.field(device_cells.find_field("voted"));
  for (size_t i = 0; i < nodes; ++i) {
    vote_differences += size_t(cpu_voted.get(i) != gpu_voted.get(i));
    voted_cells += size_t(cpu_voted.get(i) != 0.0);
  }
  CHECK(vote_differences == 0);
  // И голосование обязано что-то изменить: если бы метку не получил никто, сверка была бы пустой.
  CHECK(voted_cells > nodes / 4);
}

TEST_CASE("originator rebuilds a polyline on the device and measures the same distance") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the polyline is not compared here");
    return;
  }

  // ПОДГОТОВЛЕННОГО ИНДЕКСА НА УСТРОЙСТВЕ НЕТ: отрезок собирается на месте из тех же двух буферов.
  // Проверяется, что от этого не меняется НИ ОДНО значение — включая ответ там, где ломаной рядом нет
  // (предел, а не бесконечность) и обе метрики, потому что метрика приезжает словом, а в шейдер
  // числом.
  constexpr size_t chain_points = 24;

  const std::vector<field_pair> point_fields = {{"position", "v3"}};
  auto point_layout = originator::make_buffer_layout(originator::storage_kind::soa, point_fields, "route");
  originator::buffer route("route", std::move(point_layout), chain_points);

  const std::vector<field_pair> offset_fields = {{"start", "ui1"}};
  auto offset_layout = originator::make_buffer_layout(originator::storage_kind::soa, offset_fields, "chains");
  originator::buffer chains("chains", std::move(offset_layout), 3);

  const std::vector<field_pair> sample_fields = {{"position", "v3"}, {"distance", "v1"}};
  auto sample_layout = originator::make_buffer_layout(originator::storage_kind::soa, sample_fields, "samples");
  originator::buffer samples("samples", std::move(sample_layout),
                             originator::buffer_extent{grid_width, grid_width, 0});

  auto point = route.field(route.find_field("position"));
  for (size_t i = 0; i < chain_points; ++i) {
    point.set(i, std::cos(double(i) * 0.4) * 40.0, 0);
    point.set(i, double(i) * 3.0, 1);
    point.set(i, std::sin(double(i) * 0.4) * 40.0, 2);
  }
  auto chain_start = chains.field(chains.find_field("start"));
  chain_start.set(0, 0.0);
  chain_start.set(1, 12.0);
  chain_start.set(2, double(chain_points));

  auto position = samples.field(samples.find_field("position"));
  for (size_t i = 0; i < grid_count; ++i) {
    position.set(i, double(i % grid_width) * 0.25 - 60.0, 0);
    position.set(i, double(i / grid_width) * 0.25, 1);
    position.set(i, double((i * 13) % 97) * 0.5 - 24.0, 2);
  }

  const auto compare = [&](const std::string_view& metric) {
    originator::parameters params;
    params.set_number("max_distance", 25.0);
    params.set_string("metric", std::string(metric));

    const auto build = [&](originator::buffer& target) {
      originator::computation_queue queue;
      queue.name = "corridor";
      auto call = tool_call("polyline_distance",
                            {readable(target, "position"), readable(route, "position"), readable(chains, "start")},
                            {writable(target, "distance")}, params);
      call.range_end = grid_count;
      queue.calls.push_back(std::move(call));
      queue.output.push_back(writable(target, "distance"));
      return queue;
    };

    auto device_samples = samples;
    const auto cpu_queue = build(samples);
    const auto check = originator::check_device_queue(cpu_queue);
    REQUIRE_MESSAGE(check.allowed, check.message);
    run_on_cpu(cpu_queue);

    painter::compute_context_config config;
    config.app_name = "device_polyline_test";
    painter::compute_context context(config);
    originator::device_queue plan(context, build(device_samples));
    plan.run();

    const auto worst = worst_difference(samples, device_samples, "distance");
    // И поле обязано быть содержательным: если бы все элементы упёрлись в предел, сверка сравнивала
    // бы одну константу с другой.
    size_t inside = 0;
    const auto measured = samples.field(samples.find_field("distance"));
    for (size_t i = 0; i < grid_count; ++i) {
      inside += size_t(measured.get(i) < 24.9);
    }
    std::printf("  polyline '%s': worst deviation %.3g, %zu of %zu elements inside the corridor\n",
                std::string(metric).c_str(), worst, inside, grid_count);
    CHECK(worst < 1.0e-4);
    CHECK(inside > grid_count / 100);
  };

  compare("euclidean");
  compare("chebyshev");
}
