#include <bit>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/script_program.h"
#include "devils_engine/originator/script_translate.h"
#include "devils_engine/painter/compute_context.h"
#include "devils_engine/utils/shared.h"

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

// Push-константа устройственного вызова: ОБЩАЯ шапка, затем объявленные аргументы по одному float.
std::vector<std::byte> make_push(const uint32_t element_count,
                                 const originator::translation& translated,
                                 const originator::parameters& params) {
  std::vector<std::byte> push(sizeof(originator::device_call_header) +
                              translated.form.params().size() * sizeof(float));
  originator::device_call_header header;
  header.count = element_count;
  header.begin = 0;
  header.extent_x = element_count;
  header.extent_y = 1;
  std::memcpy(push.data(), &header, sizeof(header));
  for (size_t i = 0; i < translated.form.params().size(); ++i) {
    const float value = float(params.number(translated.form.params()[i].name, translated.form.params()[i].fallback));
    std::memcpy(push.data() + sizeof(header) + i * sizeof(float), &value, sizeof(value));
  }
  return push;
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
  CHECK(translated.source.find("in_0_at(index)") != std::string::npos);
  CHECK(translated.source.find("in_1_at(index)") != std::string::npos);
  // Целые поля читаются с преобразованием: `float[]` над сырым int32 читал бы биты float.
  CHECK(translated.source.find("float(in_0_at(index))") != std::string::npos);
  // ФОРМА ВЫДАНА ПЕРЕВОДОМ, и это единственный способ для чужого тела попасть на устройство: собрать
  // `translated_form` из строки нельзя, конструктор закрыт ключом. Проверяется здесь не «нельзя» —
  // это утверждение компилятора, — а то, что перевод форму ВЫДАЁТ.
  CHECK(translated.form.declared());
  CHECK(translated.form.refusal().empty());

  // Тело отдаётся ОТДЕЛЬНО от привязок: род поля выводится из всей очереди, поэтому собрать текст
  // может только тот, кто её видит целиком.
  CHECK(translated.form.body().find("out_0_set(index,") != std::string::npos);
  CHECK(translated.source.find(translated.form.body()) != std::string::npos);
  // Шапка push-константы — ОБЩАЯ, та же, что у нативного инструмента.
  CHECK(translated.source.find("uint count; uint begin; uint extent_x; uint extent_y;") != std::string::npos);
  REQUIRE(translated.form.params().size() == 2);
  CHECK(translated.form.params()[0].name == "low");
  CHECK(translated.form.params()[0].shader_name() == "arg_low");
  // Запись в целое поле повторяет store_component: зажим и усечение, а не «как получится».
  CHECK(translated.source.find("uint(clamp(") != std::string::npos);
  // Индекс из двух осей и охранник — без них диспатч не портируемый.
  CHECK(translated.source.find("gl_WorkGroupID.y") != std::string::npos);
  CHECK(translated.source.find("if (local >= args.count) return;") != std::string::npos);
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

  // УЗКИЙ РОД БОЛЬШЕ НЕ ОТКАЗ: на устройстве поле живёт расширенным, потому что копия там это КЭШ, а
  // кэш вправе быть шире истины. Проверяется ровно то, что делает расширение честным: тип аксессора
  // тридцатидвухбитный, а ЗАЖИМ идёт по ИСХОДНОМУ роду — иначе поле из `resident`, которое никогда не
  // скачивается, осталось бы со значением, невозможным на хосте.
  const originator::translated_field narrow{"biome", originator::field_base::ub};
  const auto widened = originator::translate_to_glsl("narrow", classify_source, inputs, narrow, kind);
  CHECK(widened.source.find("buffer out_0_block { uint data[]; }") != std::string::npos);
  CHECK(widened.source.find("clamp(") != std::string::npos);
  CHECK(widened.source.find("255.0") != std::string::npos);
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

  // Push-байты выкладываются ОБЩЕЙ шапкой, той же, что у нативного инструмента, а за ней аргументы в
  // объявленном порядке. Своя шапка у перевода однажды уже была — и отличалась от инструментной; пока
  // перевод не попадал в устройственную очередь, это не стреляло.
  const auto push = make_push(uint32_t(count), translated, params);

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

  const auto push = make_push(uint32_t(count), translated, params);

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

TEST_CASE("originator names a refused device form instead of silently having none") {
  // ОТКАЗ — ЗАКОННЫЙ ОТВЕТ, а не дефект: непереводимая конструкция (случайность, списки, `ctx_save`)
  // оставляет очередь на CPU. Но отказ обязан НЕСТИ ПРИЧИНУ: форма, которой просто нет, и форма,
  // которую отказались объявить, различаются только тем, что вторая может это сказать.
  const auto refused = originator::translated_form::refused("randomness has no salt in the AST");
  CHECK_FALSE(refused.declared());
  CHECK(refused.body().empty());
  CHECK(refused.refusal() == "randomness has no salt in the AST");

  const originator::translated_form nothing;
  CHECK_FALSE(nothing.declared());
  CHECK(nothing.refusal().empty());
}

namespace {
// ВТОРАЯ ПОЛОВИНА СЛОВАРЯ: ветвления, локали и случайность. Проверяется она иначе, чем арифметика:
// у ветвлений сверка с CPU остаётся ПОБИТОВОЙ (они детерминированы и целочисленны), а у случайности
// сверять пути нельзя вовсе — соль места вызова у `ds` живёт в скомпилированном контейнере, и
// устройство считает СВОЙ поток. Поэтому случайность сверяется с формулой на хосте, а не с `ds`.
constexpr size_t small_count = 4096;

// Поля с маленьким диапазоном: у `switch` каждая ветка обязана кому-то достаться, иначе сверка с CPU
// упёрлась бы в не определённое у `ds` поведение не совпавшего `switch`.
originator::buffer make_small() {
  const std::vector<field_pair> fields = {{"a", "i1"}, {"b", "i1"}, {"out", "i1"}, {"value", "v1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "small");
  originator::buffer cells("small", std::move(layout), small_count);

  auto a = cells.field(cells.find_field("a"));
  auto b = cells.field(cells.find_field("b"));
  for (size_t i = 0; i < small_count; ++i) {
    a.set(i, double(i % 3));
    b.set(i, double(i % 5));
  }
  return cells;
}

std::vector<originator::translated_field> small_inputs() {
  return {originator::translated_field{"a", originator::field_base::i},
          originator::translated_field{"b", originator::field_base::i}};
}

// Прогон переведённой программы на устройстве. Входы кладутся буферами В ОБЪЯВЛЕННОМ ПОРЯДКЕ, выход
// возвращается словами: как их читать, решает род выходного поля.
std::vector<uint32_t> run_on_device(painter::compute_context& ctx,
                                    const originator::translation& translated,
                                    const std::span<const std::vector<int32_t>>& inputs,
                                    const std::span<const std::byte>& push,
                                    const size_t count) {
  const size_t byte_size = count * sizeof(uint32_t);
  const auto program = ctx.create_program("case", translated.source, uint32_t(inputs.size() + 1),
                                          uint32_t(push.size()));
  const auto staging = ctx.create_buffer(byte_size, true);

  std::vector<painter::compute_context::buffer_id> bound;
  bound.reserve(inputs.size() + 1);
  for (const auto& input : inputs) {
    const auto device_buffer = ctx.create_buffer(byte_size, false);
    ctx.write(staging, input.data(), byte_size);
    ctx.copy(staging, device_buffer, byte_size);
    bound.push_back(device_buffer);
  }
  bound.push_back(ctx.create_buffer(byte_size, false));

  ctx.dispatch(program, bound, push.data(), push.size(), count, translated.group_size);

  std::vector<uint32_t> result(count, 0xffffffffu);
  ctx.copy(bound.back(), staging, byte_size);
  ctx.read(staging, result.data(), byte_size);
  return result;
}

// Значения полей `a` и `b` в том виде, в каком их принимает устройство.
std::vector<std::vector<int32_t>> small_host(const originator::buffer& cells) {
  std::vector<std::vector<int32_t>> host(2, std::vector<int32_t>(small_count));
  const auto a = cells.field(cells.find_field("a"));
  const auto b = cells.field(cells.find_field("b"));
  for (size_t i = 0; i < small_count; ++i) {
    host[0][i] = int32_t(a.get(i));
    host[1][i] = int32_t(b.get(i));
  }
  return host;
}
} // namespace

TEST_CASE("originator translates branch blocks, locals and randomness") {
  const auto inputs = small_inputs();
  const originator::translated_field output{"out", originator::field_base::i};
  const auto number = originator::script_program::result_kind::number;
  const auto predicate = originator::script_program::result_kind::predicate;

  const auto translate = [&](const std::string_view& source, const originator::script_program::result_kind kind) {
    return originator::translate_to_glsl("case", source, inputs, output, kind);
  };

  // `select`: первое истинное условие, последний блок — иначе-ветка. Тернарники вложены В ТОМ ЖЕ
  // порядке, в каком написаны условия. Программа предикатная не случайно: у `ds` ожидаемый тип
  // протекает в условие, поэтому сравнение условием живёт только там, где сама программа возвращает
  // истину/ложь, а числовая программа пишет условия константами или через `value_or`.
  const auto selected = translate("{ select = { { condition = a < 1, a < 2 }, { b < 3 } } }", predicate);
  CHECK(selected.source.find(" ? ") != std::string::npos);
  CHECK(selected.source.find(" : ") != std::string::npos);

  // `sequence`: сходится по И у предикатной программы и складывается у числовой — тот же накопитель,
  // что у `ds`. Оборванная цепочка отдаёт единицу накопителя, а не ноль.
  const auto sequenced = translate("{ sequence = { { condition = a < 2, b < 3 } } }", predicate);
  CHECK(sequenced.source.find("&&") != std::string::npos);
  CHECK(sequenced.source.find(": true") != std::string::npos);

  // Равенство ПЛАВАЮЩИХ переводится сравнением С ДОПУСКОМ — тем же, каким его считает `ds`: побитовое
  // `==` разошлось бы с ним ровно на границе правила.
  const std::vector<originator::translated_field> floating = {{"a", originator::field_base::v},
                                                              {"b", originator::field_base::v}};
  const auto equality = originator::translate_to_glsl("equality", "{ value_or = { a == b, 1, 2 } }", floating,
                                                      output, number);
  CHECK(equality.source.find("abs(") != std::string::npos);
  CHECK(equality.source.find("0.000001") != std::string::npos);

  // А два ЦЕЛЫХ листа сравниваются целыми: у `ds` это сравнение двух точных double, то есть тот же
  // ответ, а на устройстве оно перестаёт врать выше 2^24. Допуск здесь был бы лишним кругом через
  // `float32` — ровно там, где он и теряет числа.
  const auto integer_equality = translate("{ value_or = { a == 0, 1, 2 } }", number);
  CHECK(integer_equality.source.find("0.000001") == std::string::npos);

  // `switch`: сравниваемое значение считается ОДИН раз и живёт локалью.
  const auto switched =
    translate("{ switch = { value = a, { value = 0, 10 }, { value = 1, 20 }, { value = 2, 30 } } }", number);
  CHECK(switched.form.body().find("float tmp_0 = ") != std::string::npos);

  // `random`: веса считаются все и до выбора, `chance` умножается на их сумму.
  const auto chosen = translate("{ random = { { weight = 1, 5 }, { weight = 3, 7 } } }", number);
  CHECK(chosen.form.body().find("originator_chance(index, 0u)") != std::string::npos);

  // `ctx_save` — ОПЕРАТОР: он объявляет локаль и не является слагаемым блока.
  const auto stored = translate("{ ctx_save = { t = a + b }, ctx:saved:t * 2 }", number);
  CHECK(stored.form.body().find("float saved_t = ") != std::string::npos);
  CHECK(stored.form.body().find("out_0_set(index, int(clamp(((saved_t * float(2)))") != std::string::npos);

  // `ctx_set` перекрывает аргумент локалью, и чтение ПОСЛЕ него видит локаль, а не push-константу.
  // Аргумент, который программа только устанавливает, в push-константу поэтому и не попадает.
  const auto overridden = translate("{ ctx_set = { k = a * 3 }, ctx:arg:k + 1 }", number);
  CHECK(overridden.arguments.empty());
  CHECK(overridden.form.body().find("float set_k = ") != std::string::npos);

  // Соль у каждого места вызова СВОЯ и растёт по порядку обхода: два `chance` в одной программе —
  // два разных потока, а не одно и то же число дважды.
  const auto twice = translate("{ random = { { weight = 1, 0 }, { weight = 1, 1 } }, chance }", number);
  CHECK(twice.form.body().find("originator_chance(index, 0u)") != std::string::npos);
  CHECK(twice.form.body().find("originator_chance(index, 1u)") != std::string::npos);

  // ЧТО ОТКАЗЫВАЕТСЯ, И ПОЧЕМУ ИМЕННО ЭТО. Локаль поднимается в преамбулу и считается до выбора
  // ветки, поэтому запись изнутри ветки состоялась бы и тогда, когда ветка не выбрана.
  CHECK_THROWS_AS(translate("{ value_or = { a > 0, ctx_save = { t = 1 }, 0 } }", number), std::runtime_error);
  // Слот, который на этом пути ещё не записан, — чтение неинициализированной локали.
  CHECK_THROWS_AS(translate("{ ctx:saved:missing + 1 }", number), std::runtime_error);
  // У `select` последний блок это иначе-ветка: без неё шейдеру нечего вернуть.
  CHECK_THROWS_AS(translate("{ select = { { condition = a == 0, 1 } } }", number), std::runtime_error);
  CHECK_THROWS_AS(translate("{ sequence = { { 1 } } }", number), std::runtime_error);
  CHECK_THROWS_AS(translate("{ switch = { { value = 0, 1 } } }", number), std::runtime_error);
  CHECK_THROWS_AS(translate("{ random = { { 5 } } }", number), std::runtime_error);
  // `chance` это значение, а не вызов: написанное рядом число выглядело бы порогом, которым не является.
  CHECK_THROWS_AS(translate("{ chance = 50 }", number), std::runtime_error);
}

TEST_CASE("originator branch blocks agree with the CPU path bit for bit") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the two paths are not compared here");
    return;
  }

  auto cells = make_small();
  const auto host = small_host(cells);
  const std::vector<std::string> names = {"a", "b"};
  const auto inputs = small_inputs();
  const originator::translated_field output{"out", originator::field_base::i};

  painter::compute_context_config config;
  config.app_name = "translate_branch_test";
  painter::compute_context ctx(config);

  // Сверка идёт по ЦЕЛЫМ, и только поэтому она побитовая: ветвление — это ВЫБОР, а не арифметика,
  // поэтому расхождение здесь означало бы, что пути выбрали РАЗНЫЕ ветки, то есть ошибку перевода.
  // Случайности среди них нет намеренно: её потоки у CPU и устройства разные по построению.
  const auto agrees = [&](const std::string_view& source, const originator::script_program::result_kind kind) {
    originator::parameters params;
    params.set_number("k", 0.0);
    const auto program = originator::script_program::compile("case", source, names, kind);
    const std::vector<originator::field_ref> program_inputs{readable(cells, "a"), readable(cells, "b")};
    const std::vector<originator::field_ref> program_outputs{writable(cells, "out")};
    originator::dispatch_script(*program, program_inputs, program_outputs, params, 7, 0, small_count, "branch",
                                nullptr);

    std::vector<int32_t> cpu(small_count);
    const auto out = cells.field(cells.find_field("out"));
    for (size_t i = 0; i < small_count; ++i) {
      cpu[i] = int32_t(out.get(i));
    }

    const auto translated = originator::translate_to_glsl("case", source, inputs, output, kind);
    const auto push = make_push(uint32_t(small_count), translated, params);
    const auto gpu = run_on_device(ctx, translated, host, push, small_count);

    size_t differences = 0;
    for (size_t i = 0; i < small_count; ++i) {
      differences += size_t(std::bit_cast<int32_t>(gpu[i]) != cpu[i]);
    }
    if (differences != 0) {
      MESSAGE("'" << source << "': " << differences << " of " << small_count << " differ");
    }
    return differences == 0;
  };

  const auto number = originator::script_program::result_kind::number;
  const auto predicate = originator::script_program::result_kind::predicate;

  CHECK(agrees("{ select = { { condition = a < 1, a < 2 }, { b < 3 } } }", predicate));
  CHECK(agrees("{ sequence = { { condition = a < 2, b < 3 }, { condition = b < 4, a < 2 } } }", predicate));
  CHECK(agrees("{ switch = { value = a, { value = 0, 10 }, { value = 1, 20 }, { value = 2, 30 } } }", number));
  CHECK(agrees("{ ctx_save = { t = a + b }, ctx:saved:t * 2 }", number));
  // `ctx_set` пишет в слот АРГУМЕНТА, и чтение после него видит записанное — на обоих путях.
  CHECK(agrees("{ ctx_set = { k = a * 3 }, ctx:arg:k + 1 }", number));
  // Вложенные ветвления: соли и аргументы обязаны нумероваться в порядке ТЕКСТА, а не в порядке
  // сборки тернарников.
  CHECK(agrees("{ select = { { condition = a < 1, select = { { condition = b < 1, a < 3 }, { b < 3 } } }, "
               "{ b < 2 } } }", predicate));
}

TEST_CASE("originator randomness on the device is a function of seed, element and call site") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: device randomness is not checked here");
    return;
  }

  auto cells = make_small();
  const auto host = small_host(cells);
  const auto inputs = small_inputs();
  const originator::translated_field output{"value", originator::field_base::v};

  painter::compute_context_config config;
  config.app_name = "translate_random_test";
  painter::compute_context ctx(config);

  const auto translated = originator::translate_to_glsl("noise", "{ chance }", inputs, output,
                                                        originator::script_program::result_kind::number);

  const auto run_with = [&](const uint32_t seed) {
    std::vector<std::byte> push(sizeof(originator::device_call_header));
    originator::device_call_header header;
    header.count = uint32_t(small_count);
    header.extent_x = uint32_t(small_count);
    header.extent_y = 1;
    header.seed = seed;
    std::memcpy(push.data(), &header, sizeof(header));
    return run_on_device(ctx, translated, host, push, small_count);
  };

  const auto first = run_with(originator::fold_seed(11));
  const auto again = run_with(originator::fold_seed(11));
  const auto other = run_with(originator::fold_seed(12));

  // ОДНО ЗЕРНО — ОДИН МИР, побитово: значение выведено из (зерно, элемент, место вызова) и больше ни
  // из чего, поэтому разбиение работы по инвокациям на него не влияет.
  CHECK(first == again);

  size_t changed = 0;
  for (size_t i = 0; i < small_count; ++i) {
    changed += size_t(first[i] != other[i]);
  }
  CHECK(changed > small_count / 2);

  // И ЭТО ТОТ ЖЕ ХЕШ, ЧТО У ДВИЖКА. Текст `prng2` выписан в преамбуле шейдера, потому что у
  // вычислительной программы нет `#include`, — значит копия обязана сверяться, иначе она разъедется
  // с оригиналом молча.
  size_t mismatches = 0;
  for (size_t i = 0; i < small_count; ++i) {
    const uint32_t seed = originator::fold_seed(11);
    const auto state = utils::shared::prng2(seed + 0x9E3779B9u, uint32_t(i) + 0x85EBCA6Bu);
    const auto expected = utils::shared::prng_normalize(utils::shared::prng2(state + 0xC2B2AE35u, 0x27D4EB2Fu));
    mismatches += size_t(std::bit_cast<float>(first[i]) != expected);
  }
  CHECK(mismatches == 0);

  // `rndmix` — хеш САМИХ ЗНАЧЕНИЙ: он не зависит ни от зерна, ни от элемента, поэтому одно и то же
  // число даёт одно и то же значение везде. Проверяется той же формулой на хосте.
  const auto mixed = originator::translate_to_glsl("mixed", "{ rndmix = { a, b } }", inputs, output,
                                                   originator::script_program::result_kind::number);
  {
    std::vector<std::byte> push(sizeof(originator::device_call_header));
    originator::device_call_header header;
    header.count = uint32_t(small_count);
    header.extent_x = uint32_t(small_count);
    header.extent_y = 1;
    header.seed = originator::fold_seed(11);
    std::memcpy(push.data(), &header, sizeof(header));

    const auto result = run_on_device(ctx, mixed, host, push, small_count);
    size_t mixes = 0;
    for (size_t i = 0; i < small_count; ++i) {
      const auto first_bits = std::bit_cast<uint32_t>(float(host[0][i])) + 0x9E3779B9u;
      const auto second_bits = std::bit_cast<uint32_t>(float(host[1][i])) + 0x85EBCA6Bu;
      const auto expected = utils::shared::prng_normalize(utils::shared::prng2(first_bits, second_bits));
      mixes += size_t(std::bit_cast<float>(result[i]) != expected);
    }
    CHECK(mixes == 0);
  }

  // Значение лежит в [0, 1) и заполняет отрезок: хеш, который вернул бы константу, прошёл бы обе
  // проверки выше и не прошёл бы эту.
  size_t buckets[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < small_count; ++i) {
    const auto value = std::bit_cast<float>(first[i]);
    REQUIRE(value >= 0.0f);
    REQUIRE(value < 1.0f);
    buckets[size_t(value * 4.0f)] += 1;
  }
  for (const auto count : buckets) {
    CHECK(count > small_count / 8);
  }
}

TEST_CASE("originator weighted choice follows the declared weights") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the weighted choice is not checked here");
    return;
  }

  auto cells = make_small();
  const auto host = small_host(cells);
  const auto inputs = small_inputs();
  const originator::translated_field output{"out", originator::field_base::i};

  painter::compute_context_config config;
  config.app_name = "translate_weight_test";
  painter::compute_context ctx(config);

  // Веса 1 и 3: доля второй ветки обязана быть около трёх четвертей. Проверяется именно ДОЛЯ, потому
  // что вес — это предпочтение при выборе, а не обещанное число попаданий.
  const auto translated = originator::translate_to_glsl(
    "weighted", "{ random = { { weight = 1, 0 }, { weight = 3, 1 } } }", inputs, output,
    originator::script_program::result_kind::number);

  std::vector<std::byte> push(sizeof(originator::device_call_header));
  originator::device_call_header header;
  header.count = uint32_t(small_count);
  header.extent_x = uint32_t(small_count);
  header.extent_y = 1;
  header.seed = originator::fold_seed(29);
  std::memcpy(push.data(), &header, sizeof(header));

  const auto result = run_on_device(ctx, translated, host, push, small_count);

  size_t heavy = 0;
  for (size_t i = 0; i < small_count; ++i) {
    const auto value = std::bit_cast<int32_t>(result[i]);
    REQUIRE((value == 0 || value == 1));
    heavy += size_t(value == 1);
  }

  const double share = double(heavy) / double(small_count);
  MESSAGE("weights 1:3 gave the heavy branch " << share);
  CHECK(share > 0.70);
  CHECK(share < 0.80);
}

TEST_CASE("originator keeps integer leaves integer, and that shows above 2^24") {
  // ГРАНИЦА `float32` — 2^24: выше него соседние целые склеиваются в одно число. У `ds` поле читается
  // в `double` и точно до 2^53, поэтому программа, СРАВНИВАЮЩАЯ идентификаторы, на устройстве обязана
  // была бы отвечать неверно — если бы сравнение шло плавающим.
  //
  // Арифметика в целый путь не входит НАМЕРЕННО: у `ds` она двойной точности при любом роде поля
  // (аксессор возвращает `double`), и целые в шейдере РАЗОШЛИСЬ бы с ней делением и переполнением.
  const std::vector<originator::translated_field> inputs = {{"id", originator::field_base::ui},
                                                            {"other", originator::field_base::ui}};
  const originator::translated_field output{"mark", originator::field_base::ui};
  const auto number = originator::script_program::result_kind::number;

  // Сравнение двух целых листов — целыми, без единого `float(` вокруг них.
  const auto compared = originator::translate_to_glsl("compare", "{ value_or = { id == other, 1, 0 } }", inputs,
                                                      output, number);
  CHECK(compared.source.find("(int(in_0_at(index)) == int(in_1_at(index)))") != std::string::npos);

  // Копия идентификатора не делает круга через float.
  const auto copied = originator::translate_to_glsl("copy", "{ id }", inputs, output, number);
  CHECK(copied.form.body().find("uint(max(int(in_0_at(index)), 0))") != std::string::npos);

  // А арифметика остаётся плавающей — это и есть граница, проведённая по семантике `ds`.
  const auto summed = originator::translate_to_glsl("sum", "{ id + other }", inputs, output, number);
  CHECK(summed.source.find("float(in_0_at(index))") != std::string::npos);

  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the boundary is not measured here");
    return;
  }

  // ЧИСЛА ВЫШЕ 2^24, ОТЛИЧАЮЩИЕСЯ НА ЕДИНИЦУ: во `float32` они равны, в целых — нет.
  constexpr size_t pairs = 4096;
  const std::vector<field_pair> fields = {{"id", "ui1"}, {"other", "ui1"}, {"mark", "ui1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout), pairs);

  auto id = cells.field(cells.find_field("id"));
  auto other = cells.field(cells.find_field("other"));
  for (size_t i = 0; i < pairs; ++i) {
    const double base = 16777216.0 + double(i) * 2.0; // 2^24 и выше
    id.set(i, base);
    other.set(i, base + 1.0);
  }

  const std::vector<std::string> names = {"id", "other"};
  const auto program = originator::script_program::compile("compare", "{ value_or = { id == other, 1, 0 } }",
                                                           names, number);
  originator::parameters params;
  const std::vector<originator::field_ref> program_inputs{readable(cells, "id"), readable(cells, "other")};
  const std::vector<originator::field_ref> program_outputs{writable(cells, "mark")};
  originator::dispatch_script(*program, program_inputs, program_outputs, params, 1, 0, pairs, "boundary", nullptr);

  size_t cpu_equal = 0;
  const auto mark = cells.field(cells.find_field("mark"));
  for (size_t i = 0; i < pairs; ++i) {
    cpu_equal += size_t(mark.get(i) != 0.0);
  }
  // `ds` читает в double: соседние целые выше 2^24 для него РАЗНЫЕ.
  CHECK(cpu_equal == 0);

  std::vector<std::vector<int32_t>> host(2, std::vector<int32_t>(pairs));
  for (size_t i = 0; i < pairs; ++i) {
    host[0][i] = int32_t(id.get(i));
    host[1][i] = int32_t(other.get(i));
  }

  painter::compute_context_config config;
  config.app_name = "translate_integer_test";
  painter::compute_context context(config);

  const auto push = make_push(uint32_t(pairs), compared, params);
  const auto gpu = run_on_device(context, compared, host, push, pairs);
  size_t gpu_equal = 0;
  for (size_t i = 0; i < pairs; ++i) {
    gpu_equal += size_t(gpu[i] != 0u);
  }
  // И на устройстве теперь тоже: целое сравнение не склеивает соседей.
  CHECK(gpu_equal == 0);
}

// ПРЕДЕЛ PUSH-КОНСТАНТЫ ОБЪЯВЛЕН БИБЛИОТЕКОЙ, а не спрошен у устройства: `maxPushConstantsSize` у
// разных машин разный, и вызов, зажатый по нему, у автора собрался бы, а у игрока отказал. Проверяется
// именно ГРАНИЦА с обеих сторон — предел, который никогда не срабатывает, ничего не обещает.
TEST_CASE("originator declares its own push-constant budget instead of asking the device") {
  const auto inputs = classify_inputs();
  const originator::translated_field output{"biome", originator::field_base::i};
  const auto kind = originator::script_program::result_kind::number;

  // Шапка плюс параметры по одному float — ровно то соглашение, по которому байты и выкладываются.
  CHECK(originator::device_push_limit == 128);
  CHECK(originator::device_param_limit ==
        (originator::device_push_limit - sizeof(originator::device_call_header)) / sizeof(float));

  const auto program_with = [](const size_t arguments) {
    std::string source = "{ height";
    for (size_t i = 0; i < arguments; ++i) source.append(std::format(" + ctx:arg:a{}", i));
    source.append(" }");
    return source;
  };

  const auto fits = originator::translate_to_glsl("fits", program_with(originator::device_param_limit),
                                                  inputs, output, kind);
  CHECK(fits.form.params().size() == originator::device_param_limit);
  CHECK(sizeof(originator::device_call_header) + fits.form.params().size() * sizeof(float) ==
        originator::device_push_limit);

  // Одним больше — ОТКАЗ, а не ошибка: очередь остаётся на CPU и считает то же самое.
  CHECK_THROWS_AS(originator::translate_to_glsl("over", program_with(originator::device_param_limit + 1),
                                                inputs, output, kind),
                  std::runtime_error);

  // Тот же предел стоит и на нативном теле — сборщик текста один на переводы и на инструменты.
  std::vector<originator::device_binding> shape(2);
  shape[1].writable = true;
  const std::vector<originator::device_param> too_many(originator::device_param_limit + 1,
                                                       originator::device_param{"p", 0.0, "p"});
  CHECK_THROWS_AS(originator::build_device_shader(shape, too_many, "  out_0_set(index, 0.0);\n", 64),
                  std::runtime_error);
}
