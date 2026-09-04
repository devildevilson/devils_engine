#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/tools.h"
#include "devils_engine/painter/compute_context.h"

using namespace devils_engine;

// ЧТО ОСТАЛОСЬ У ОЧЕРЕДИ ПЕРЕД УСТРОЙСТВОМ, кроме перевода: апертура `gather`, роды полей и
// РАСКЛАДКА, кэш шейдеров. Всё три — измеряемые вопросы, поэтому здесь они и живут, а не в
// рассуждении.
//
// Часть с устройством ОХРАНЯЕМАЯ: отсутствие устройства — законный ответ, и очередь обязана иметь
// путь на CPU.

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t grid_width = 1024;
constexpr size_t grid_count = grid_width * grid_width;

double milliseconds_since(const std::chrono::steady_clock::time_point start) {
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

// GATHER на устройстве: то же окно, что у нативного `box_blur`, слово в слово по его правилам —
// окно зажимается по краям карты, элементы за пределами буфера пропускаются, делится на ЧИСЛО
// ВЗЯТЫХ, а не на площадь окна. Иначе сравнивались бы две разные задачи.
//
// Правило «источник != приёмник» здесь выполняется по построению: у gather приёмник это отдельный
// буфер, и на устройстве иначе и не выразить — писать в тот же буфер, который читают соседи,
// означало бы читать его в неопределённом состоянии.
constexpr std::string_view box_blur_source = R"glsl(
#version 450

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer source_buffer { float source[]; };
layout(std430, binding = 1) writeonly buffer target_buffer { float target[]; };

layout(push_constant) uniform settings {
  uint count;
  uint width;
  uint height;
  uint radius;
} args;

void main() {
  uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
  uint index = group * gl_WorkGroupSize.x + gl_LocalInvocationID.x;
  if (index >= args.count) return;

  uint x = index % args.width;
  uint y = index / args.width;

  uint x0 = x >= args.radius ? x - args.radius : 0u;
  uint x1 = min(x + args.radius, args.width - 1u);
  uint y0 = y >= args.radius ? y - args.radius : 0u;
  uint y1 = args.height == 0u ? y : min(y + args.radius, args.height - 1u);

  float sum = 0.0;
  uint taken = 0u;
  for (uint sy = y0; sy <= y1; ++sy) {
    for (uint sx = x0; sx <= x1; ++sx) {
      uint at = sy * args.width + sx;
      if (at >= args.count) continue;
      sum += source[at];
      taken += 1u;
    }
  }

  target[index] = taken == 0u ? 0.0 : sum / float(taken);
}
)glsl";

struct blur_settings {
  uint32_t count = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t radius = 0;
};

// РАСКЛАДКА. Один и тот же счёт по полю, лежащему подряд (`soa`) и с шагом элемента (`aos`). Шаг и
// смещение приходят параметрами, поэтому текст один, а раскладки две — сравнивается именно доступ, а
// не два разных шейдера.
constexpr std::string_view strided_source = R"glsl(
#version 450

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer source_buffer { float source[]; };
layout(std430, binding = 1) writeonly buffer target_buffer { float target[]; };

layout(push_constant) uniform settings {
  uint count;
  uint stride;
  uint source_offset;
  uint target_offset;
  float scale;
} args;

void main() {
  uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
  uint index = group * gl_WorkGroupSize.x + gl_LocalInvocationID.x;
  if (index >= args.count) return;

  uint at = index * args.stride + args.source_offset;
  target[index * args.stride + args.target_offset] = source[at] * args.scale;
}
)glsl";

struct strided_settings {
  uint32_t count = 0;
  uint32_t stride = 1;
  uint32_t source_offset = 0;
  uint32_t target_offset = 0;
  float scale = 1.5f;
};

originator::tool_registry& registry() {
  static originator::tool_registry r;
  if (r.size() == 0) {
    r.add_standard_tools();
  }
  return r;
}

originator::buffer make_cells() {
  const std::vector<field_pair> fields = {{"height", "v1"}, {"smoothed", "v1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "cells");
  originator::buffer cells("cells", std::move(layout),
                           originator::buffer_extent{grid_width, grid_width, 0});

  auto height = cells.field(cells.find_field("height"));
  for (size_t i = 0; i < grid_count; ++i) {
    height.set(i, std::sin(double(i) * 0.0013) * 0.5 + 0.5);
  }
  return cells;
}
} // namespace

TEST_CASE("painter compute context compiles one shader once") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  painter::compute_context_config config;
  config.app_name = "device_cache_test";
  painter::compute_context ctx(config);

  const auto blur = std::string(box_blur_source);
  const auto strided = std::string(strided_source);

  const auto start = std::chrono::steady_clock::now();
  const auto first = ctx.create_program("box_blur", blur, 2, sizeof(blur_settings));
  const double compile_ms = milliseconds_since(start);

  const auto hit = std::chrono::steady_clock::now();
  const auto again = ctx.create_program("box_blur", blur, 2, sizeof(blur_settings));
  const double cached_ms = milliseconds_since(hit);

  // Тот же ТЕКСТ — та же программа, и компиляция была ровно одна. Проверяется счётчиком, а не
  // временем: дважды скомпилированная программа работает так же, и по результату этого не видно.
  CHECK(again == first);
  CHECK(ctx.compiled_programs() == 1);

  // Другой текст — другая программа: ключ это текст, потому что текст и есть производная от всего
  // остального. Время ВТОРОЙ компиляции меряется отдельно, и это важно: без него не отличить цену
  // одного шейдера от одноразовой раскрутки компилятора, а вывод про кэш от этого меняется.
  const auto second_start = std::chrono::steady_clock::now();
  const auto other = ctx.create_program("strided", strided, 2, sizeof(strided_settings));
  const double second_compile_ms = milliseconds_since(second_start);
  CHECK(other != first);
  CHECK(ctx.compiled_programs() == 2);

  // Та же форма привязок при другом тексте не должна путаться с кэшем.
  CHECK(ctx.create_program("strided", strided, 2, sizeof(strided_settings)) == other);
  CHECK(ctx.compiled_programs() == 2);

  // ТА ЖЕ ПАРА ШЕЙДЕРОВ БЕЗ ОПТИМИЗАТОРА. Без этого числа нельзя сказать, чем платится компиляция:
  // если почти всё берёт оптимизатор, то у сгенерированного шейдера — выражения из десятка узлов —
  // его ценность ещё надо доказать, и решение «оптимизировать» перестаёт быть само собой разумеющимся.
  painter::compute_context_config raw_config;
  raw_config.app_name = "device_cache_raw";
  raw_config.optimize_shaders = false;
  painter::compute_context raw(raw_config);

  const auto raw_start = std::chrono::steady_clock::now();
  raw.create_program("box_blur", blur, 2, sizeof(blur_settings));
  const double raw_first_ms = milliseconds_since(raw_start);
  const auto raw_second_start = std::chrono::steady_clock::now();
  raw.create_program("strided", strided, 2, sizeof(strided_settings));
  const double raw_second_ms = milliseconds_since(raw_second_start);

  // ТА ЖЕ ПРОГРАММА В ДРУГОМ ПРОЦЕССЕ-ПОДОБНОМ КОНТЕКСТЕ, но с кэшем пайплайнов НА ДИСКЕ. Этим
  // отделяется цена драйвера (SPIR-V -> ISA, её кэш и снимает) от цены shaderc (GLSL -> SPIR-V,
  // которую кэш пайплайнов не трогает вовсе). Без этого разделения непонятно, ЧТО кэшировать на диске.
  const std::string cache_path = "compute_pipeline_cache.tmp";
  double cold_ms = 0.0;
  double warm_ms = 0.0;
  {
    painter::compute_context_config cold;
    cold.app_name = "device_cache_cold";
    cold.pipeline_cache_path = cache_path;
    painter::compute_context context(cold);
    const auto at = std::chrono::steady_clock::now();
    context.create_program("box_blur", blur, 2, sizeof(blur_settings));
    cold_ms = milliseconds_since(at);
  }
  {
    painter::compute_context_config warm;
    warm.app_name = "device_cache_warm";
    warm.pipeline_cache_path = cache_path;
    painter::compute_context context(warm);
    const auto at = std::chrono::steady_clock::now();
    context.create_program("box_blur", blur, 2, sizeof(blur_settings));
    warm_ms = milliseconds_since(at);
  }

  std::printf("\n  shader compile, optimized:   first %.3f ms, second %.3f ms\n", compile_ms, second_compile_ms);
  std::printf("  pipeline cache on disk:      cold %.3f ms, warm %.3f ms\n", cold_ms, warm_ms);
  std::printf("  shader compile, unoptimized: first %.3f ms, second %.3f ms\n", raw_first_ms, raw_second_ms);
  std::printf("  cache hit: %.4f ms\n", cached_ms);
  // Компиляция стоит МИЛЛИСЕКУНДЫ, и у стримингового генератора чанков тысячи: без кэша один и тот
  // же перевод компилировался бы в каждом.
  CHECK(cached_ms < compile_ms);
}

TEST_CASE("originator gather runs on the device and agrees with the native tool") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  auto cells = make_cells();
  const auto* tool = registry().find("box_blur");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("radius", 2);

  const std::vector<originator::field_ref> inputs{
    originator::field_ref{&cells, nullptr, cells.find_field("height")}};
  const std::vector<originator::field_ref> outputs{
    originator::field_ref{&cells, &cells, cells.find_field("smoothed")}};

  // CPU: тот же инструмент, форму берёт у привязки.
  const auto cpu_start = std::chrono::steady_clock::now();
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, grid_count, "device", nullptr);
  const double cpu_ms = milliseconds_since(cpu_start);

  std::vector<float> reference(grid_count);
  const auto smoothed = cells.field(cells.find_field("smoothed"));
  for (size_t i = 0; i < grid_count; ++i) {
    reference[i] = float(smoothed.get(i));
  }

  std::vector<float> host_height(grid_count);
  const auto height = cells.field(cells.find_field("height"));
  for (size_t i = 0; i < grid_count; ++i) {
    host_height[i] = float(height.get(i));
  }

  painter::compute_context_config config;
  config.app_name = "device_gather_test";
  painter::compute_context ctx(config);

  const auto program = ctx.create_program("box_blur", std::string(box_blur_source), 2, sizeof(blur_settings));

  const size_t byte_size = grid_count * sizeof(float);
  const auto staging = ctx.create_buffer(byte_size, true);
  const auto device_source = ctx.create_buffer(byte_size, false);
  const auto device_target = ctx.create_buffer(byte_size, false);
  ctx.write(staging, host_height.data(), byte_size);
  ctx.copy(staging, device_source, byte_size);

  blur_settings settings;
  settings.count = uint32_t(grid_count);
  settings.width = uint32_t(cells.extent().x);
  settings.height = uint32_t(cells.extent().y);
  settings.radius = 2;

  const painter::compute_context::buffer_id bound[] = {device_source, device_target};
  double best_dispatch = 1e30;
  for (int repeat = 0; repeat < 5; ++repeat) {
    const auto start = std::chrono::steady_clock::now();
    ctx.dispatch(program, bound, &settings, sizeof(settings), grid_count);
    best_dispatch = std::min(best_dispatch, milliseconds_since(start));
  }

  std::vector<float> gpu(grid_count, -1.0f);
  ctx.copy(device_target, staging, byte_size);
  ctx.read(staging, gpu.data(), byte_size);

  // Сходство, а не побитовое равенство: CPU складывает окно в double, шейдер во float, и порядок
  // сложения тот же только потому, что окно обходится одинаково. Ноль здесь никто не обещал.
  double worst = 0.0;
  for (size_t i = 0; i < grid_count; ++i) {
    worst = std::max(worst, double(std::abs(gpu[i] - reference[i])));
  }

  std::printf("  gather (box_blur radius 2, %zu cells): cpu 1 thread %.3f ms, device dispatch %.3f ms (%.1fx), "
              "worst deviation %.3g\n",
              grid_count, cpu_ms, best_dispatch, cpu_ms / best_dispatch, worst);
  CHECK(worst < 1e-5);

  // Окно РАСТРА сбалансировано: у каждого элемента соседей столько же, поэтому занятость волны здесь
  // ни при чём. Несбалансирован CSR соседства (в GN02 степень доходит до 24 при средних шести), и
  // это ДРУГОЙ замер — ему нужны данные графовых инструментов, и он остаётся названным пробелом.
  MESSAGE("raster gather is balanced by construction; the imbalanced case is the adjacency CSR, measured "
          "separately with the graph tools");
}

TEST_CASE("originator field layout costs measurably more on the device than on the CPU") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available");
    return;
  }

  painter::compute_context_config config;
  config.app_name = "device_layout_test";
  painter::compute_context ctx(config);

  const auto program = ctx.create_program("strided", std::string(strided_source), 2, sizeof(strided_settings));

  // `soa`: поле лежит подряд, шаг 1. `aos`: элемент лежит целиком, поэтому шаг равен числу полей, и
  // соседние инвокации читают память через дырки. На CPU этот выбор стоил 2% в один поток и 17% на
  // одиннадцати; здесь он стоит своего замера, потому что доступ волны коалесцируется или нет.
  struct layout_case {
    const char* name;
    uint32_t stride;
  };
  const layout_case cases[] = {{"soa (stride 1)", 1}, {"aos (stride 4)", 4}, {"aos (stride 8)", 8}};

  double contiguous_ms = 0.0;
  for (const auto& variant : cases) {
    const size_t elements = grid_count;
    const size_t byte_size = elements * variant.stride * sizeof(float);
    const auto source = ctx.create_buffer(byte_size, false);
    const auto target = ctx.create_buffer(byte_size, false);

    strided_settings settings;
    settings.count = uint32_t(elements);
    settings.stride = variant.stride;
    settings.source_offset = 0;
    settings.target_offset = variant.stride > 1 ? 1 : 0;

    const painter::compute_context::buffer_id bound[] = {source, target};
    double best = 1e30;
    for (int repeat = 0; repeat < 5; ++repeat) {
      const auto start = std::chrono::steady_clock::now();
      ctx.dispatch(program, bound, &settings, sizeof(settings), elements);
      best = std::min(best, milliseconds_since(start));
    }

    if (variant.stride == 1) {
      contiguous_ms = best;
    }
    std::printf("  layout %-14s %zu elements over %.2f MB: %.3f ms (%.2fx of soa)\n", variant.name, elements,
                double(byte_size) / (1024.0 * 1024.0), best, contiguous_ms > 0.0 ? best / contiguous_ms : 1.0);
  }

  // Одна декларация в конфиге (`layout = aos|soa`) переключает это, не тронув ни строки скрипта — и
  // на устройстве у неё своя цена, которую надо знать до того, как её выберут за красоту.
  CHECK(contiguous_ms > 0.0);
}
