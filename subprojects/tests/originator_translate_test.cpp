#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/script_program.h"
#include "devils_engine/originator/script_translate.h"
#include "devils_engine/painter/compute_context.h"

using namespace devils_engine;

// ТРАНСЛЯТОР `ds` -> GLSL и сверка ДВУХ ПУТЕЙ.
//
// Сверка идёт на ЦЕЛОЧИСЛЕННОЙ программе, и это не выбор удобного случая. На плавающей программе
// сверять пути нельзя вовсе: разная точность и разный порядок операций дают разные результаты, и это
// объявленное свойство, а не дефект. А на целочисленной сверка ПОБИТОВАЯ и осмысленная — float32
// представляет целые до 2^24 точно, поэтому расхождение здесь означало бы ошибку перевода, а не
// арифметику.

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t count = 65536;

// Классификация целыми: пороги целые, поля целые, ответ — номер класса. Та же форма, из которой в
// GN01 собрано правило биома вложением `value_or`, только без дробей.
constexpr std::string_view classify_source =
  "{ value_or = { height < ctx:arg:low, 0, value_or = { moisture < ctx:arg:high, 1, 2 } } }";

originator::buffer make_cells() {
  const std::vector<field_pair> fields = {
    {"height", "i1"},
    {"moisture", "i1"},
    {"biome", "ui1"},
  };
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout), count);

  auto height = cells.field(cells.find_field("height"));
  auto moisture = cells.field(cells.find_field("moisture"));
  for (size_t i = 0; i < count; ++i) {
    height.set(i, double(int64_t(i % 200) - 100));
    moisture.set(i, double(int64_t(i % 37) - 18));
  }
  return cells;
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

std::vector<originator::translated_field> classify_inputs() {
  return {originator::translated_field{"height", originator::field_base::i},
          originator::translated_field{"moisture", originator::field_base::i}};
}
} // namespace

TEST_CASE("originator translates a ds program into a compute shader") {
  const auto inputs = classify_inputs();
  const originator::translated_field output{"biome", originator::field_base::ui};

  const auto translated = originator::translate_to_glsl("classify", classify_source, inputs, output,
                                                        originator::script_program::result_kind::number);

  // Аргументы собраны в порядке первой встречи, и этот порядок уезжает наружу: хост обязан выкладывать
  // push-байты в нём, а не догадываться.
  REQUIRE(translated.arguments.size() == 2);
  CHECK(translated.arguments[0] == "low");
  CHECK(translated.arguments[1] == "high");

  // Поле читается по своему индексу и никак иначе: программа структурно pointwise, и в переводе это
  // видно буквально.
  CHECK(translated.source.find("in_0.data[index]") != std::string::npos);
  CHECK(translated.source.find("in_1.data[index]") != std::string::npos);
  // Целые поля читаются с преобразованием: `float[]` над сырым int32 читал бы биты float.
  CHECK(translated.source.find("float(in_0.data[index])") != std::string::npos);
  // Запись в целое поле повторяет store_component: зажим и усечение, а не «как получится».
  CHECK(translated.source.find("uint(clamp(") != std::string::npos);
  // Индекс из двух осей и охранник — без них диспатч не портируемый.
  CHECK(translated.source.find("gl_WorkGroupID.y") != std::string::npos);
  CHECK(translated.source.find("if (index >= args.count) return;") != std::string::npos);
  CHECK(translated.source.find("local_size_x = 64") != std::string::npos);
}

TEST_CASE("originator refuses what it cannot translate, and says why") {
  const auto inputs = classify_inputs();
  const originator::translated_field output{"biome", originator::field_base::ui};
  const auto kind = originator::script_program::result_kind::number;

  const auto fails = [&](const std::string_view& source) {
    CHECK_THROWS_AS(originator::translate_to_glsl("bad", source, inputs, output, kind), std::runtime_error);
  };

  // Случайность: соль каждого места вызова генерирует эмиттер ds при компиляции, и в AST её нет.
  // Причина не в ширине хеша — 32-битный PRNG в движке уже есть и уже общий для C++ и GLSL.
  fails("{ chance = 50 }");
  fails("{ value_or = { chance = 50, 1, 2 } }");
  // Список — динамическая память на элемент, которой у инвокации нет.
  fails("{ add_to = { mylist, height } }");
  // Сохранённый слот переводится в локаль шейдера только при доказанном доминировании записи.
  fails("{ ctx_save = { keep = height } }");
  // Диагностика в очередь не пускается.
  fails("{ assert = { height > 0 } }");
  // Имя, не привязанное ни к одному полю, ничем стать не может.
  fails("{ unbound + height }");
  // Функции вне словаря: у перевода нет догадок про чужую семантику.
  fails("{ mystery = { height } }");
  // Арность проверяется до компиляции шейдера, чтобы сообщение ссылалось на текст ds.
  fails("{ max = { height } }");
  fails("{ value_or = { height < 0, 1 } }");

  // Род поля, у которого на устройстве нет представления без расширений.
  const originator::translated_field narrow{"biome", originator::field_base::ub};
  CHECK_THROWS_AS(originator::translate_to_glsl("narrow", classify_source, inputs, narrow, kind),
                  std::runtime_error);
}

TEST_CASE("originator translates the arithmetic vocabulary one to one") {
  const auto inputs = classify_inputs();
  const originator::translated_field output{"biome", originator::field_base::v};
  const auto kind = originator::script_program::result_kind::number;

  const auto translated = originator::translate_to_glsl(
    "math", "{ clamp = { fma = { height, 2, moisture }, 0, 10 } }", inputs, output, kind);
  CHECK(translated.source.find("clamp(fma(") != std::string::npos);

  // `inv` — единственная арифметическая функция ds без тёзки в GLSL.
  const auto inverted = originator::translate_to_glsl("inverted", "{ inv = { height } }", inputs, output, kind);
  CHECK(inverted.source.find("(1.0 / ") != std::string::npos);

  // Словесные операторы ds становятся своими символами GLSL.
  const auto logic = originator::translate_to_glsl(
    "logic", "{ height > 0 and moisture > 0 }", inputs, output,
    originator::script_program::result_kind::predicate);
  CHECK(logic.source.find("&&") != std::string::npos);
  // У предиката ответ приводится к числу поля тем же способом, каким это делает CPU.
  CHECK(logic.source.find("? 1.0 : 0.0") != std::string::npos);
}

TEST_CASE("originator translated shader agrees with the CPU path bit for bit") {
  if (!painter::compute_device_available()) {
    // Устройства может не быть вовсе, и это законный ответ: очередь обязана иметь путь на CPU.
    MESSAGE("no Vulkan device available: the two paths are not compared here");
    return;
  }

  const int64_t low = -20;
  const int64_t high = 5;

  // ПУТЬ CPU: та же программа, скомпилированная штатным `script_program`.
  auto cells = make_cells();
  const std::vector<std::string> names = {"height", "moisture"};
  const auto program = originator::script_program::compile("classify", classify_source, names,
                                                           originator::script_program::result_kind::number);

  originator::parameters params;
  params.set_number("low", double(low));
  params.set_number("high", double(high));

  const std::vector<originator::field_ref> program_inputs{readable(cells, "height"), readable(cells, "moisture")};
  const std::vector<originator::field_ref> program_outputs{writable(cells, "biome")};
  originator::dispatch_script(*program, program_inputs, program_outputs, params, 1, 0, count, "translate", nullptr);

  std::vector<uint32_t> cpu(count);
  const auto biome = cells.field(cells.find_field("biome"));
  for (size_t i = 0; i < count; ++i) {
    cpu[i] = uint32_t(biome.get(i));
  }

  // ПУТЬ УСТРОЙСТВА: тот же текст, переведённый в шейдер.
  const auto inputs = classify_inputs();
  const originator::translated_field output{"biome", originator::field_base::ui};
  const auto translated = originator::translate_to_glsl("classify", classify_source, inputs, output,
                                                        originator::script_program::result_kind::number);
  REQUIRE(translated.arguments.size() == 2);

  painter::compute_context_config config;
  config.app_name = "translate_test";
  painter::compute_context ctx(config);

  // Push-байты выкладываются в объявленном порядке: сначала число элементов, затем аргументы так, как
  // их перечислил транслятор. Свой порядок здесь означал бы правило, считающее по чужим числам.
  std::vector<std::byte> push(sizeof(uint32_t) + translated.arguments.size() * sizeof(float));
  const uint32_t element_count = uint32_t(count);
  std::memcpy(push.data(), &element_count, sizeof(element_count));
  for (size_t i = 0; i < translated.arguments.size(); ++i) {
    const float value = float(params.number(translated.arguments[i]));
    std::memcpy(push.data() + sizeof(uint32_t) + i * sizeof(float), &value, sizeof(value));
  }

  const auto device_program =
    ctx.create_program("classify", translated.source, uint32_t(inputs.size() + 1), uint32_t(push.size()));

  std::vector<int32_t> host_height(count);
  std::vector<int32_t> host_moisture(count);
  const auto height = cells.field(cells.find_field("height"));
  const auto moisture = cells.field(cells.find_field("moisture"));
  for (size_t i = 0; i < count; ++i) {
    host_height[i] = int32_t(height.get(i));
    host_moisture[i] = int32_t(moisture.get(i));
  }

  const size_t byte_size = count * sizeof(int32_t);
  const auto staging = ctx.create_buffer(byte_size, true);
  const auto device_height = ctx.create_buffer(byte_size, false);
  const auto device_moisture = ctx.create_buffer(byte_size, false);
  const auto device_biome = ctx.create_buffer(byte_size, false);

  ctx.write(staging, host_height.data(), byte_size);
  ctx.copy(staging, device_height, byte_size);
  ctx.write(staging, host_moisture.data(), byte_size);
  ctx.copy(staging, device_moisture, byte_size);

  const painter::compute_context::buffer_id bound[] = {device_height, device_moisture, device_biome};
  ctx.dispatch(device_program, bound, push.data(), push.size(), count, translated.group_size);

  std::vector<uint32_t> gpu(count, 0xffffffffu);
  ctx.copy(device_biome, staging, byte_size);
  ctx.read(staging, gpu.data(), byte_size);

  // ПОБИТОВО. На целочисленной программе обещать это можно, и поэтому именно она проверяет
  // транслятор: расхождение здесь означало бы ошибку перевода, а не разницу арифметик.
  size_t differences = 0;
  size_t first_difference = count;
  for (size_t i = 0; i < count; ++i) {
    if (gpu[i] == cpu[i]) continue;
    ++differences;
    first_difference = std::min(first_difference, i);
  }

  if (differences != 0) {
    MESSAGE("first difference at " << first_difference << ": cpu " << cpu[first_difference] << ", gpu "
                                   << gpu[first_difference]);
  }
  CHECK(differences == 0);

  // И само правило обязано быть содержательным: если бы все элементы попали в один класс, сверка
  // ничего не проверяла бы.
  size_t classes[3] = {0, 0, 0};
  for (size_t i = 0; i < count; ++i) {
    if (cpu[i] < 3) ++classes[cpu[i]];
  }
  CHECK(classes[0] > 0);
  CHECK(classes[1] > 0);
  CHECK(classes[2] > 0);
}

namespace {
// ПОДЛИННОЕ правило GN01, слово в слово из `biome_rule.ds`. Смысл именно в подлинности: транслятор,
// который переводит только придуманные для него программы, ничего не доказывает.
constexpr std::string_view gn01_rule =
  "{ value_or = { smoothed < ctx:arg:sea_level, 0,\n"
  "  value_or = { moisture < ctx:arg:dry, 1,\n"
  "  value_or = { moisture < ctx:arg:wet, 2, 3 } } } }";
} // namespace

TEST_CASE("originator translates GN01's actual biome rule, and the float paths diverge where the rule decides") {
  const std::vector<originator::translated_field> inputs = {
    {"smoothed", originator::field_base::v},
    {"moisture", originator::field_base::v},
  };
  const originator::translated_field output{"biome", originator::field_base::ui};

  const auto translated = originator::translate_to_glsl("biome", gn01_rule, inputs, output,
                                                        originator::script_program::result_kind::number);
  REQUIRE(translated.arguments.size() == 3);
  CHECK(translated.arguments[0] == "sea_level");
  CHECK(translated.arguments[1] == "dry");
  CHECK(translated.arguments[2] == "wet");

  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the float paths are not compared here");
    return;
  }

  // Поля плавающие и нарочно попадают ВПЛОТНУЮ к порогам: правило классификации УСИЛИВАЕТ разницу
  // арифметик — сдвинулся последний бит, сменился класс. Это и есть то место, где §4.2 запрещает
  // сверять пути, и здесь оно не проверяется, а ИЗМЕРЯЕТСЯ: величину расхождения полезно знать.
  const std::vector<field_pair> fields = {{"smoothed", "v1"}, {"moisture", "v1"}, {"biome", "ui1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout), count);

  auto smoothed = cells.field(cells.find_field("smoothed"));
  auto moisture = cells.field(cells.find_field("moisture"));
  for (size_t i = 0; i < count; ++i) {
    // Значения ложатся сеткой вокруг порогов, а каждая сотая — ровно в порог: там решение и висит
    // на последнем бите.
    const double base = double(i % 1000) / 1000.0;
    smoothed.set(i, i % 100 == 0 ? 0.5 : base);
    moisture.set(i, i % 100 == 50 ? 0.35 : 1.0 - base);
  }

  originator::parameters params;
  params.set_number("sea_level", 0.5);
  params.set_number("dry", 0.35);
  params.set_number("wet", 0.65);

  const std::vector<std::string> names = {"smoothed", "moisture"};
  const auto program = originator::script_program::compile("biome", gn01_rule, names,
                                                           originator::script_program::result_kind::number);
  const std::vector<originator::field_ref> program_inputs{readable(cells, "smoothed"), readable(cells, "moisture")};
  const std::vector<originator::field_ref> program_outputs{writable(cells, "biome")};
  originator::dispatch_script(*program, program_inputs, program_outputs, params, 1, 0, count, "translate", nullptr);

  std::vector<uint32_t> cpu(count);
  const auto biome = cells.field(cells.find_field("biome"));
  for (size_t i = 0; i < count; ++i) {
    cpu[i] = uint32_t(biome.get(i));
  }

  painter::compute_context_config config;
  config.app_name = "translate_float_test";
  painter::compute_context ctx(config);

  std::vector<std::byte> push(sizeof(uint32_t) + translated.arguments.size() * sizeof(float));
  const uint32_t element_count = uint32_t(count);
  std::memcpy(push.data(), &element_count, sizeof(element_count));
  for (size_t i = 0; i < translated.arguments.size(); ++i) {
    const float value = float(params.number(translated.arguments[i]));
    std::memcpy(push.data() + sizeof(uint32_t) + i * sizeof(float), &value, sizeof(value));
  }

  // Само создание программы уже проверка: шейдер, который не компилируется, — отказ с сообщением
  // glslc, и типы у перевода проверяет именно он.
  const auto device_program =
    ctx.create_program("biome", translated.source, uint32_t(inputs.size() + 1), uint32_t(push.size()));

  std::vector<float> host_smoothed(count);
  std::vector<float> host_moisture(count);
  for (size_t i = 0; i < count; ++i) {
    host_smoothed[i] = float(smoothed.get(i));
    host_moisture[i] = float(moisture.get(i));
  }

  const size_t byte_size = count * sizeof(float);
  const auto staging = ctx.create_buffer(byte_size, true);
  const auto device_smoothed = ctx.create_buffer(byte_size, false);
  const auto device_moisture = ctx.create_buffer(byte_size, false);
  const auto device_biome = ctx.create_buffer(byte_size, false);

  ctx.write(staging, host_smoothed.data(), byte_size);
  ctx.copy(staging, device_smoothed, byte_size);
  ctx.write(staging, host_moisture.data(), byte_size);
  ctx.copy(staging, device_moisture, byte_size);

  const painter::compute_context::buffer_id bound[] = {device_smoothed, device_moisture, device_biome};
  ctx.dispatch(device_program, bound, push.data(), push.size(), count, translated.group_size);

  std::vector<uint32_t> gpu(count, 0xffffffffu);
  ctx.copy(device_biome, staging, byte_size);
  ctx.read(staging, gpu.data(), byte_size);

  size_t differences = 0;
  for (size_t i = 0; i < count; ++i) {
    differences += size_t(gpu[i] != cpu[i]);
  }

  std::printf("\n  GN01 biome rule, %zu cells: %zu classified differently by the two paths (%.4f%%)\n",
              count, differences, 100.0 * double(differences) / double(count));

  // НЕ ноль, и обещать ноль здесь никто не будет: `ds` считает в double, шейдер во float32, а
  // классификация усиливает разницу до смены класса. Проверяется только то, что расхождение ОСТАЁТСЯ
  // РЕДКИМ — иначе это уже не арифметика, а ошибка перевода.
  CHECK(differences < count / 100);
}
