#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/painter/compute_context.h"

using namespace devils_engine;

// ВЫЧИСЛИТЕЛЬНЫЙ КОНТЕКСТ и полный круг передачи: хост -> устройство -> счёт -> обратно.
//
// Тест ОХРАНЯЕМЫЙ: отсутствие устройства — законный ответ, а не провал (CI, сервер без GPU, машина
// без драйвера), и потребитель обязан уметь пойти на CPU. Там, где устройство есть, проверяется и
// сходство результата с CPU, и цена круга — второе и есть то число, из-за которого вся задача про
// GPGPU вообще имеет смысл: если передача съедает выигрыш, переносить нечего.

namespace {
// Шейдер написан РУКАМИ и лежит рядом с кодом, который его проверяет: транслятора `ds` -> GLSL пока
// нет, и первый шаг обязан мерить контекст, а не транслятор.
//
// Считает он то же, что нативный `remap` у originator: clamp(x * scale + offset). Одинаковая работа
// — единственный способ сравнить два пути, не сравнивая заодно две разные задачи.
//
// ИНДЕКС СОБИРАЕТСЯ ИЗ ДВУХ ОСЕЙ, потому что контекст сворачивает в них число групп: гарантированный
// минимум `maxComputeWorkGroupCount[0]` это 65535, и одномерный диспатч кончается на 4.2 млн
// элементов при группе 64. Охранник `if (index >= count) return;` нужен по той же причине, по
// которой он нужен всегда: диспатч кратен размеру группы, а число элементов — нет.
constexpr std::string_view remap_source = R"glsl(
#version 450

layout(local_size_x = 64) in;

layout(std430, binding = 0) readonly buffer source_buffer { float source[]; };
layout(std430, binding = 1) writeonly buffer target_buffer { float target[]; };

layout(push_constant) uniform settings {
  uint count;
  float scale;
  float offset;
  float lower;
  float upper;
} args;

void main() {
  uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
  uint index = group * gl_WorkGroupSize.x + gl_LocalInvocationID.x;
  if (index >= args.count) return;

  target[index] = clamp(source[index] * args.scale + args.offset, args.lower, args.upper);
}
)glsl";

struct remap_settings {
  uint32_t count = 0;
  float scale = 1.0f;
  float offset = 0.0f;
  float lower = 0.0f;
  float upper = 1.0f;
};

double milliseconds_since(const std::chrono::steady_clock::time_point start) {
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}
} // namespace

TEST_CASE("painter compute context runs a hand-written shader and pays for the round trip") {
  if (!painter::compute_device_available()) {
    // Не провал: «устройства нет» — объявленный законный ответ, и очередь обязана иметь путь на CPU.
    MESSAGE("no Vulkan device available: the compute context is not exercised here");
    return;
  }

  painter::compute_context_config config;
  config.app_name = "compute_context_test";
  painter::compute_context ctx(config);

  const auto& limits = ctx.limits();
  std::printf("\ncompute context on '%s'\n", ctx.device_name().c_str());
  std::printf("  max group count      %u x %u x %u\n", limits.max_group_count[0], limits.max_group_count[1],
              limits.max_group_count[2]);
  std::printf("  max group size       %u x %u x %u, invocations %u\n", limits.max_group_size[0],
              limits.max_group_size[1], limits.max_group_size[2], limits.max_group_invocations);
  std::printf("  shared memory        %u bytes\n", limits.max_shared_memory_bytes);
  std::printf("  storage buffer range %llu bytes\n", (unsigned long long)limits.max_storage_buffer_range);

  // Пределы не декоративны: по ним контекст сворачивает диспатч, и молчащий предел означал бы отказ
  // драйвера у игрока, а не у автора.
  CHECK(limits.max_group_count[0] >= 65535);
  CHECK(limits.max_group_invocations >= 64);

  const remap_settings base_settings{0, 1.5f, -0.25f, 0.0f, 1.0f};
  const auto program = ctx.create_program("remap", std::string(remap_source), 2, sizeof(remap_settings));

  // Размеры настоящие: та же карта 512x512, на которой мерялись уровни исполнения, миллион клеток
  // планеты и вчетверо больше. Три точки, а не две, потому что из них считается ЛИНЕЙНАЯ цена: у
  // отправки есть постоянная часть, и без третьей точки её от полосы не отличить.
  for (const size_t count : {size_t(262144), size_t(1048576), size_t(4194304)}) {
    const size_t byte_size = count * sizeof(float);

    std::vector<float> input(count);
    for (size_t i = 0; i < count; ++i) {
      input[i] = float(std::sin(double(i) * 0.001) * 0.5 + 0.5);
    }

    // Раздельный путь: перевалочный буфер на хосте и рабочий на устройстве. На интегрированном GPU
    // память одна, поэтому объединённый путь измерил бы удачу конкретной машины, а не передачу.
    const auto host_source = ctx.create_buffer(byte_size, true);
    const auto host_target = ctx.create_buffer(byte_size, true);
    const auto device_source = ctx.create_buffer(byte_size, false);
    const auto device_target = ctx.create_buffer(byte_size, false);
    const painter::compute_context::buffer_id bound[] = {device_source, device_target};

    auto settings = base_settings;
    settings.count = uint32_t(count);

    double best_upload = 1e30;
    double best_dispatch = 1e30;
    double best_download = 1e30;
    std::vector<float> output(count, -1.0f);

    // Лучшее из пяти: первый прогон платит за прогрев очереди и аллокаций, а мерить надо код.
    for (int repeat = 0; repeat < 5; ++repeat) {
      auto start = std::chrono::steady_clock::now();
      ctx.write(host_source, input.data(), byte_size);
      ctx.copy(host_source, device_source, byte_size);
      best_upload = std::min(best_upload, milliseconds_since(start));

      start = std::chrono::steady_clock::now();
      ctx.dispatch(program, bound, &settings, sizeof(settings), count);
      best_dispatch = std::min(best_dispatch, milliseconds_since(start));

      start = std::chrono::steady_clock::now();
      ctx.copy(device_target, host_target, byte_size);
      ctx.read(host_target, output.data(), byte_size);
      best_download = std::min(best_download, milliseconds_since(start));
    }

    // Сходство, а НЕ побитовое равенство: два разных способа вычисления дают два разных результата,
    // и это объявленное свойство, а не дефект. Здесь оба считают во float, поэтому расхождение
    // должно быть на уровне последних бит — но обещать ноль нельзя, и обещать его никто не будет.
    double worst = 0.0;
    for (size_t i = 0; i < count; ++i) {
      const float reference =
        std::clamp(input[i] * base_settings.scale + base_settings.offset, base_settings.lower, base_settings.upper);
      worst = std::max(worst, double(std::abs(output[i] - reference)));
    }

    const double total = best_upload + best_dispatch + best_download;
    std::printf("  %zu elements (%.2f MB): upload %.3f ms, dispatch %.3f ms, download %.3f ms, total %.3f ms\n",
                count, double(byte_size) / (1024.0 * 1024.0), best_upload, best_dispatch, best_download, total);
    std::printf("    transfer is %.0f%% of the round trip, worst deviation from the CPU formula %.3g\n",
                100.0 * (best_upload + best_download) / total, worst);

    CHECK(worst < 1e-6);
  }
}

TEST_CASE("painter compute context refuses what the device cannot do") {
  if (!painter::compute_device_available()) {
    MESSAGE("no Vulkan device available: the compute context is not exercised here");
    return;
  }

  painter::compute_context_config config;
  config.app_name = "compute_context_refusals";
  painter::compute_context ctx(config);

  const auto program = ctx.create_program("remap", std::string(remap_source), 2, sizeof(remap_settings));
  const auto source = ctx.create_buffer(1024, false);
  const auto target = ctx.create_buffer(1024, false);
  const painter::compute_context::buffer_id bound[] = {source, target};
  remap_settings settings{256, 1.0f, 0.0f, 0.0f, 1.0f};

  // Буфер устройства не отображён на хост, и это не повод молча ничего не сделать.
  std::vector<float> scratch(256, 0.0f);
  CHECK_THROWS_AS(ctx.write(source, scratch.data(), scratch.size() * sizeof(float)), std::runtime_error);

  // Группа больше, чем устройство умеет.
  CHECK_THROWS_AS(ctx.dispatch(program, bound, &settings, sizeof(settings), 256,
                               ctx.limits().max_group_invocations * 2),
                  std::runtime_error);

  // Число привязок и размер push-константы объявлены программой, поэтому расхождение — отказ, а не
  // чтение мусора из чужих байт.
  const painter::compute_context::buffer_id single[] = {source};
  CHECK_THROWS_AS(ctx.dispatch(program, single, &settings, sizeof(settings), 256), std::runtime_error);
  CHECK_THROWS_AS(ctx.dispatch(program, bound, &settings, sizeof(settings) - 4, 256), std::runtime_error);

  // Буфер больше того, что устройство адресует одним storage-буфером.
  CHECK_THROWS_AS(ctx.create_buffer(size_t(ctx.limits().max_storage_buffer_range) + 1024, false),
                  std::runtime_error);

  // Программа без привязок ничего не читает и не пишет.
  CHECK_THROWS_AS(ctx.create_program("empty", std::string(remap_source), 0, 0), std::runtime_error);

  // Шейдер, который не компилируется, обязан падать с сообщением компилятора, а не создавать
  // пайплайн из пустого SPIR-V.
  CHECK_THROWS_AS(ctx.create_program("broken", "#version 450\nvoid main() { this is not glsl }", 1, 0),
                  std::runtime_error);
}
