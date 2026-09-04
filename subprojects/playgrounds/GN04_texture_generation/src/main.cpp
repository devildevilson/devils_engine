#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/painter/compute_context.h"
#include "devils_engine/utils/core.h"

using namespace devils_engine;

// GN04 — ФОНОВЫЕ ТЕКСТУРЫ НА УСТРОЙСТВЕ: первый настоящий потребитель вычислительной очереди.
//
// Выбран он не наугад. `ORIGINATOR_GPGPU.md` §5 п.5 называет именно этот случай, потому что здесь НЕ
// МЕШАЕТ ни одна слабость очереди: результат остаётся на устройстве, поэтому передача обратно не
// съедает выигрыш (§4.5); детерминизм не требуется, потому что это ПРЕДСТАВЛЕНИЕ, а не симуляция
// (§4.2 и водораздел `NETWORKING.md`); очередь короткая, поэтому мёртвую работу видно глазами.
// Проверять конструкцию надо там, где её слабости ни при чём — иначе замер меряет слабости.
//
// Что здесь доказывается числами:
//   1. результат ОСТАЁТСЯ на устройстве: наружу приезжает сводка на несколько сотен байт, а не
//      мегабайты картинки, и разница между этими двумя величинами и есть смысл всей задачи;
//   2. КАРТИНКА выгодна там, где читают по НЕЦЕЛОЙ координате (§6.3): проход сглаживания читает с
//      линейным фильтром, и это тот самый случай, ради которого картинка вообще нужна;
//   3. свёртка ЦЕЛЫМИ через атомики детерминирована, хотя порядок прихода групп не закреплён:
//      целочисленное сложение от порядка не зависит. Плавающая — нет, и это разница, которую §4.3
//      называет единственным больным местом;
//   4. цепочка проходов на устройстве стоит дешевле, чем один круг передачи её результата.
//
// Чего здесь НЕТ намеренно: окна. Площадка доказывает, что картинка НЕ уезжает с устройства, и окно
// этому доказательству ничем не помогает — оно бы его подменило.

namespace {
struct options {
  uint32_t size = 1024;
  uint32_t sites = 64;
  uint32_t seed = 20260904;
  bool verify = false;
  std::string dump;
};

// ПРОХОД 1: разметка Вороного. Для каждого пикселя ищется ближайший сайт; в одну картинку уезжает
// НОМЕР области, в другую — разница расстояний до ближайшего и второго, то есть близость к границе.
//
// Апертура здесь `gather`: пиксель читает произвольные сайты и пишет свой. Правило «источник !=
// приёмник» выполняется по построению — сайты лежат в буфере, метки в картинках.
constexpr std::string_view label_source = R"glsl(
#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(std430, binding = 0) readonly buffer site_buffer { vec2 sites[]; };
layout(r32f, binding = 1) writeonly uniform image2D region_image;
layout(r32f, binding = 2) writeonly uniform image2D edge_image;

layout(push_constant) uniform settings {
  uint width;
  uint height;
  uint site_count;
} args;

void main() {
  uvec2 at = gl_GlobalInvocationID.xy;
  if (at.x >= args.width || at.y >= args.height) return;

  vec2 position = (vec2(at) + vec2(0.5)) / vec2(args.width, args.height);

  float nearest = 1e30;
  float second = 1e30;
  uint winner = 0u;
  for (uint i = 0u; i < args.site_count; ++i) {
    vec2 delta = sites[i] - position;
    float distance = dot(delta, delta);
    if (distance < nearest) {
      second = nearest;
      nearest = distance;
      winner = i;
    } else if (distance < second) {
      second = distance;
    }
  }

  imageStore(region_image, ivec2(at), vec4(float(winner), 0.0, 0.0, 0.0));
  // Близость к границе: у самой границы расстояния до двух ближайших сайтов равны.
  imageStore(edge_image, ivec2(at), vec4(sqrt(second) - sqrt(nearest), 0.0, 0.0, 0.0));
}
)glsl";

// ПРОХОД 2: сглаживание С ФИЛЬТРОМ. Читается ВЫБОРКОЙ по нецелой координате — четыре отсчёта на
// половине текселя, — и каждый из них аппаратный bilinear уже усреднил по четырём соседям. Это и есть
// та причина, по которой картинка бывает выгоднее буфера: у буфера такого чтения нет, там пришлось бы
// читать шестнадцать элементов вручную.
//
// И это же граница: точность фильтра в Vulkan implementation-defined, поэтому такой проход
// принадлежит классу представления и в чанковую генерацию не пускается.
constexpr std::string_view smooth_source = R"glsl(
#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(binding = 0) uniform sampler2D edge_sampler;
layout(r32f, binding = 1) writeonly uniform image2D smoothed_image;

layout(push_constant) uniform settings {
  uint width;
  uint height;
  float radius;
} args;

void main() {
  uvec2 at = gl_GlobalInvocationID.xy;
  if (at.x >= args.width || at.y >= args.height) return;

  vec2 texel = vec2(1.0) / vec2(args.width, args.height);
  vec2 uv = (vec2(at) + vec2(0.5)) * texel;
  vec2 step = texel * args.radius;

  float sum = texture(edge_sampler, uv + vec2(-step.x, -step.y)).r;
  sum += texture(edge_sampler, uv + vec2(step.x, -step.y)).r;
  sum += texture(edge_sampler, uv + vec2(-step.x, step.y)).r;
  sum += texture(edge_sampler, uv + vec2(step.x, step.y)).r;

  imageStore(smoothed_image, ivec2(at), vec4(sum * 0.25, 0.0, 0.0, 0.0));
}
)glsl";

// ПРОХОД 3: видимая текстура. Номер области превращается в цвет, сглаженная близость к границе — в
// затемнение. Ничего из этого на хост не едет: дальше её читал бы пост-процесс, и §5 п.5 именно про
// это.
constexpr std::string_view compose_source = R"glsl(
#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(r32f, binding = 0) readonly uniform image2D region_image;
layout(r32f, binding = 1) readonly uniform image2D smoothed_image;
layout(rgba8, binding = 2) writeonly uniform image2D colour_image;

layout(push_constant) uniform settings {
  uint width;
  uint height;
  float border;
} args;

vec3 region_colour(uint id) {
  // Цвет ВЫВОДИТСЯ из номера области хешем: таблицы цветов у площадки нет и не нужно, а один и тот
  // же номер обязан давать один и тот же цвет.
  uint h = id * 2654435761u;
  return vec3(float((h >> 16) & 255u), float((h >> 8) & 255u), float(h & 255u)) / 255.0;
}

void main() {
  uvec2 at = gl_GlobalInvocationID.xy;
  if (at.x >= args.width || at.y >= args.height) return;

  uint region = uint(imageLoad(region_image, ivec2(at)).r + 0.5);
  float edge = imageLoad(smoothed_image, ivec2(at)).r;

  float shade = smoothstep(0.0, args.border, edge);
  vec3 colour = region_colour(region) * (0.25 + 0.75 * shade);
  imageStore(colour_image, ivec2(at), vec4(colour, 1.0));
}
)glsl";

// ПРОХОД 4: СВОДКА, и только она уезжает наружу. Гистограмма по областям через атомики.
//
// Порядок прихода групп ничем не закреплён, но результат от этого не зависит: ЦЕЛОЧИСЛЕННОЕ сложение
// не зависит от порядка. Именно это отличает такую свёртку от плавающей, про которую §4.3 говорит,
// что её последний бит решает, где будет берег.
constexpr std::string_view histogram_source = R"glsl(
#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(r32f, binding = 0) readonly uniform image2D region_image;
layout(std430, binding = 1) buffer histogram_buffer { uint counts[]; };

layout(push_constant) uniform settings {
  uint width;
  uint height;
  uint site_count;
} args;

void main() {
  uvec2 at = gl_GlobalInvocationID.xy;
  if (at.x >= args.width || at.y >= args.height) return;

  uint region = uint(imageLoad(region_image, ivec2(at)).r + 0.5);
  if (region >= args.site_count) return;
  atomicAdd(counts[region], 1u);
}
)glsl";

struct label_settings {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t site_count = 0;
};

struct smooth_settings {
  uint32_t width = 0;
  uint32_t height = 0;
  float radius = 1.0f;
};

struct compose_settings {
  uint32_t width = 0;
  uint32_t height = 0;
  float border = 0.02f;
};

double milliseconds_since(const std::chrono::steady_clock::time_point start) {
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

uint32_t hash_u32(uint32_t value) noexcept {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

// Сайты расставляет ХОСТ, и это ровно тот случай, который правилом разрешён: множество маленькое и
// перечислено самим хостом, а не обходом плотного буфера.
std::vector<float> make_sites(const options& opts) {
  std::vector<float> sites(size_t(opts.sites) * 2);
  for (uint32_t i = 0; i < opts.sites; ++i) {
    sites[i * 2 + 0] = float(hash_u32(opts.seed ^ (i * 2u + 1u)) >> 8) / float(1u << 24);
    sites[i * 2 + 1] = float(hash_u32(opts.seed ^ (i * 2u + 2u)) >> 8) / float(1u << 24);
  }
  return sites;
}

// Эталон на CPU: тот же ближайший сайт, посчитанный тем же способом. Сравнение осмысленно потому, что
// РЕШЕНИЕ здесь целочисленное (номер области), а не плавающее: разница арифметик меняет ответ только
// у пикселей, стоящих ровно на границе, и таких считанные единицы.
uint32_t nearest_site(const std::vector<float>& sites, const uint32_t count, const float x, const float y) {
  float nearest = 1e30f;
  uint32_t winner = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const float dx = sites[i * 2 + 0] - x;
    const float dy = sites[i * 2 + 1] - y;
    const float distance = dx * dx + dy * dy;
    if (distance < nearest) {
      nearest = distance;
      winner = i;
    }
  }
  return winner;
}

options parse_options(const int argc, const char** argv) {
  options result;
  const auto starts_with = [](const std::string_view& text, const std::string_view& prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--verify") {
      result.verify = true;
    } else if (starts_with(argument, "--size=")) {
      result.size = uint32_t(std::stoul(std::string(argument.substr(7))));
    } else if (starts_with(argument, "--sites=")) {
      result.sites = uint32_t(std::stoul(std::string(argument.substr(8))));
    } else if (starts_with(argument, "--seed=")) {
      result.seed = uint32_t(std::stoul(std::string(argument.substr(7))));
    } else if (starts_with(argument, "--dump=")) {
      result.dump = std::string(argument.substr(7));
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN04 texture generation lab\n"
                << "  --size=N    сторона текстуры (по умолчанию 1024)\n"
                << "  --sites=N   число областей (по умолчанию 64)\n"
                << "  --seed=N    зерно расстановки сайтов\n"
                << "  --verify    прогнать контрактные проверки\n"
                << "  --dump=PATH сохранить видимую текстуру в PPM (для ГЛАЗА, не для конвейера)\n";
      std::exit(0);
    } else {
      utils::error{}("GN04: unknown argument '{}'", argument);
    }
  }

  if (result.sites == 0) {
    utils::error{}("GN04: a texture of zero regions has nothing to show");
  }
  return result;
}

// Результат одного полного прогона цепочки. Наружу из устройства уезжает ровно это: гистограмма и
// времена.
struct run_result {
  std::vector<uint32_t> histogram;
  std::vector<uint32_t> region_sample;
  std::vector<float> edge_sample;
  std::vector<float> smoothed_sample;
  double label_ms = 0.0;
  double smooth_ms = 0.0;
  double compose_ms = 0.0;
  double histogram_ms = 0.0;
  double summary_ms = 0.0;
  size_t summary_bytes = 0;
  size_t image_bytes = 0;
  std::vector<uint8_t> colour;
};

run_result run_chain(painter::compute_context& ctx, const options& opts, const bool want_colour) {
  using context = painter::compute_context;

  const uint32_t width = opts.size;
  const uint32_t height = opts.size;
  const auto sites = make_sites(opts);

  const size_t site_bytes = sites.size() * sizeof(float);
  const auto host_sites = ctx.create_buffer(site_bytes, true);
  const auto device_sites = ctx.create_buffer(site_bytes, false);
  ctx.write(host_sites, sites.data(), site_bytes);
  ctx.copy(host_sites, device_sites, site_bytes);

  const auto region = ctx.create_image(width, height, context::image_format::r32f);
  const auto edge = ctx.create_image(width, height, context::image_format::r32f);
  const auto smoothed = ctx.create_image(width, height, context::image_format::r32f);
  const auto colour = ctx.create_image(width, height, context::image_format::rgba8);

  const size_t histogram_bytes = size_t(opts.sites) * sizeof(uint32_t);
  const auto histogram = ctx.create_buffer(histogram_bytes, false);
  const auto histogram_host = ctx.create_buffer(histogram_bytes, true);
  {
    // Гистограмма обнуляется ЯВНО: атомики только добавляют, поэтому мусор в буфере стал бы частью
    // ответа, а по правдоподобным числам этого не видно.
    const std::vector<uint32_t> zeros(opts.sites, 0);
    ctx.write(histogram_host, zeros.data(), histogram_bytes);
    ctx.copy(histogram_host, histogram, histogram_bytes);
  }

  const context::binding_kind label_bindings[] = {
    context::binding_kind::storage_buffer, context::binding_kind::storage_image,
    context::binding_kind::storage_image};
  const context::binding_kind smooth_bindings[] = {context::binding_kind::sampled_image,
                                                   context::binding_kind::storage_image};
  const context::binding_kind compose_bindings[] = {
    context::binding_kind::storage_image, context::binding_kind::storage_image,
    context::binding_kind::storage_image};
  const context::binding_kind histogram_bindings[] = {context::binding_kind::storage_image,
                                                      context::binding_kind::storage_buffer};

  const auto label_program =
    ctx.create_program("voronoi_label", std::string(label_source), label_bindings, sizeof(label_settings));
  const auto smooth_program =
    ctx.create_program("smooth_edges", std::string(smooth_source), smooth_bindings, sizeof(smooth_settings));
  const auto compose_program =
    ctx.create_program("compose", std::string(compose_source), compose_bindings, sizeof(compose_settings));
  const auto histogram_program = ctx.create_program("histogram", std::string(histogram_source),
                                                    histogram_bindings, sizeof(label_settings));

  label_settings label{width, height, opts.sites};
  smooth_settings smooth{width, height, 1.0f};
  compose_settings compose{width, height, 0.02f};

  const context::bound_resource label_resources[] = {context::bound_resource::of_buffer(device_sites),
                                                     context::bound_resource::of_image(region),
                                                     context::bound_resource::of_image(edge)};
  const context::bound_resource smooth_resources[] = {context::bound_resource::of_image(edge),
                                                      context::bound_resource::of_image(smoothed)};
  const context::bound_resource compose_resources[] = {context::bound_resource::of_image(region),
                                                       context::bound_resource::of_image(smoothed),
                                                       context::bound_resource::of_image(colour)};
  const context::bound_resource histogram_resources[] = {context::bound_resource::of_image(region),
                                                          context::bound_resource::of_buffer(histogram)};

  run_result result;

  auto at = std::chrono::steady_clock::now();
  ctx.dispatch_2d(label_program, label_resources, &label, sizeof(label), width, height);
  result.label_ms = milliseconds_since(at);

  at = std::chrono::steady_clock::now();
  ctx.dispatch_2d(smooth_program, smooth_resources, &smooth, sizeof(smooth), width, height);
  result.smooth_ms = milliseconds_since(at);

  at = std::chrono::steady_clock::now();
  ctx.dispatch_2d(compose_program, compose_resources, &compose, sizeof(compose), width, height);
  result.compose_ms = milliseconds_since(at);

  at = std::chrono::steady_clock::now();
  ctx.dispatch_2d(histogram_program, histogram_resources, &label, sizeof(label), width, height);
  result.histogram_ms = milliseconds_since(at);

  // НАРУЖУ УЕЗЖАЕТ ТОЛЬКО ЭТО. Картинки остаются на устройстве, и разница между двумя числами ниже —
  // весь смысл §5 п.5.
  at = std::chrono::steady_clock::now();
  result.histogram.resize(opts.sites);
  ctx.copy(histogram, histogram_host, histogram_bytes);
  ctx.read(histogram_host, result.histogram.data(), histogram_bytes);
  result.summary_ms = milliseconds_since(at);
  result.summary_bytes = histogram_bytes;
  result.image_bytes = size_t(width) * size_t(height) * 4;

  // А вот это уже НЕ часть конвейера: выборки нужны проверкам, картинка — глазу. Оба чтения названы
  // отдельно, чтобы их нельзя было спутать со сводкой.
  std::vector<float> region_field(size_t(width) * height);
  std::vector<float> edge_field(size_t(width) * height);
  std::vector<float> smoothed_field(size_t(width) * height);
  ctx.read_image(region, region_field.data(), region_field.size() * sizeof(float));
  ctx.read_image(edge, edge_field.data(), edge_field.size() * sizeof(float));
  ctx.read_image(smoothed, smoothed_field.data(), smoothed_field.size() * sizeof(float));

  result.region_sample.resize(region_field.size());
  for (size_t i = 0; i < region_field.size(); ++i) {
    result.region_sample[i] = uint32_t(region_field[i] + 0.5f);
  }
  result.edge_sample = std::move(edge_field);
  result.smoothed_sample = std::move(smoothed_field);

  if (want_colour) {
    result.colour.resize(result.image_bytes);
    ctx.read_image(colour, result.colour.data(), result.colour.size());
  }

  return result;
}

bool write_ppm(const std::string& path, const std::vector<uint8_t>& rgba, const uint32_t width,
               const uint32_t height) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << width << " " << height << "\n255\n";
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    out.put(char(rgba[i * 4 + 0]));
    out.put(char(rgba[i * 4 + 1]));
    out.put(char(rgba[i * 4 + 2]));
  }
  return bool(out);
}

int run_once(const options& opts) {
  painter::compute_context_config config;
  config.app_name = "GN04";
  painter::compute_context ctx(config);

  std::cout << "GN04: текстура " << opts.size << "x" << opts.size << ", областей " << opts.sites
            << ", устройство '" << ctx.device_name() << "'\n";

  const auto result = run_chain(ctx, opts, !opts.dump.empty());

  const double device_ms = result.label_ms + result.smooth_ms + result.compose_ms + result.histogram_ms;
  std::cout << "  разметка Вороного   " << result.label_ms << " мс\n"
            << "  сглаживание фильтром" << result.smooth_ms << " мс\n"
            << "  сборка цвета        " << result.compose_ms << " мс\n"
            << "  гистограмма         " << result.histogram_ms << " мс\n"
            << "  ИТОГО на устройстве " << device_ms << " мс\n"
            << "  сводка наружу       " << result.summary_ms << " мс, " << result.summary_bytes << " байт\n"
            << "  картинка осталась   " << result.image_bytes << " байт ("
            << (double(result.image_bytes) / double(result.summary_bytes)) << "x того, что уехало)\n";

  uint32_t smallest = UINT32_MAX;
  uint32_t largest = 0;
  size_t total = 0;
  for (const auto count : result.histogram) {
    smallest = std::min(smallest, count);
    largest = std::max(largest, count);
    total += count;
  }
  std::cout << "  области             от " << smallest << " до " << largest << " пикселей, сумма " << total
            << "\n";

  if (!opts.dump.empty()) {
    if (!write_ppm(opts.dump, result.colour, opts.size, opts.size)) {
      utils::error{}("GN04: could not write '{}'", opts.dump);
    }
    std::cout << "  картинка для глаза  " << opts.dump << "\n";
  }
  return 0;
}

int run_verify(const options& opts) {
  size_t checks = 0;
  size_t failures = 0;
  const auto check = [&](const bool condition, const std::string_view& label) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cout << "  ПРОВАЛ: " << label << "\n";
    }
  };

  painter::compute_context_config config;
  config.app_name = "GN04_verify";
  painter::compute_context ctx(config);

  std::cout << "GN04 verify: текстура " << opts.size << "x" << opts.size << ", областей " << opts.sites
            << ", устройство '" << ctx.device_name() << "'\n";

  const auto first = run_chain(ctx, opts, false);
  const size_t pixels = size_t(opts.size) * opts.size;

  // 1. Каждый пиксель попал в какую-то область: гистограмма обязана сойтись с числом пикселей.
  size_t total = 0;
  for (const auto count : first.histogram) {
    total += count;
  }
  check(total == pixels, "гистограмма по областям сходится с числом пикселей");

  // 2. Ни одна область не пуста. Иначе разметка не разметка, а сводка врёт правдоподобно.
  bool all_present = true;
  for (const auto count : first.histogram) {
    all_present = all_present && count > 0;
  }
  check(all_present, "ни одна область не осталась пустой");

  // 3. Разметка совпадает с эталоном на CPU. Решение здесь ЦЕЛОЧИСЛЕННОЕ (номер области), поэтому
  //    сравнение осмысленно: расхождение арифметик меняет ответ только у пикселей ровно на границе.
  const auto sites = make_sites(opts);
  size_t mismatches = 0;
  const size_t stride = std::max<size_t>(pixels / 20000, 1);
  size_t sampled = 0;
  for (size_t i = 0; i < pixels; i += stride) {
    const uint32_t x = uint32_t(i % opts.size);
    const uint32_t y = uint32_t(i / opts.size);
    const float px = (float(x) + 0.5f) / float(opts.size);
    const float py = (float(y) + 0.5f) / float(opts.size);
    mismatches += size_t(nearest_site(sites, opts.sites, px, py) != first.region_sample[i]);
    ++sampled;
  }
  check(sampled > 1000, "выборка для сверки с CPU достаточно большая");
  check(mismatches * 1000 < sampled, "разметка совпадает с эталоном на CPU (кроме пикселей на границе)");

  // 4. ФИЛЬТР ДЕЙСТВИТЕЛЬНО РАБОТАЛ. Сглаженное поле обязано отличаться от исходного — иначе проход
  //    выборки был бы дорогим копированием, и заметить это по картинке нельзя.
  size_t changed = 0;
  double worst = 0.0;
  for (size_t i = 0; i < pixels; ++i) {
    const double delta = std::abs(double(first.smoothed_sample[i]) - double(first.edge_sample[i]));
    worst = std::max(worst, delta);
    changed += size_t(delta > 1e-6);
  }
  check(changed * 10 > pixels, "чтение с фильтром изменило поле, а не скопировало его");
  check(worst > 1e-4, "изменение фильтра заметно по величине");

  // 5. Сглаживание не должно выводить значения за пределы исходного диапазона: среднее четырёх
  //    отсчётов внутри выпуклой оболочки, и нарушение означало бы чтение мимо картинки.
  float lowest = first.edge_sample[0];
  float highest = first.edge_sample[0];
  for (const auto value : first.edge_sample) {
    lowest = std::min(lowest, value);
    highest = std::max(highest, value);
  }
  bool inside = true;
  for (const auto value : first.smoothed_sample) {
    inside = inside && value >= lowest - 1e-5f && value <= highest + 1e-5f;
  }
  check(inside, "сглаженное поле остаётся внутри диапазона исходного");

  // 6. СВЁРТКА ЦЕЛЫМИ ЧЕРЕЗ АТОМИКИ ВОСПРОИЗВОДИМА. Порядок прихода групп ничем не закреплён, но
  //    целочисленное сложение от порядка не зависит — а плавающее зависит, и §4.3 называет это
  //    единственным больным местом свёрток.
  const auto second = run_chain(ctx, opts, false);
  check(second.histogram == first.histogram, "гистограмма через атомики повторяется прогон в прогон");
  check(second.region_sample == first.region_sample, "разметка повторяется прогон в прогон");

  // 7. Компиляция шейдеров случилась РОВНО ЧЕТЫРЕ раза на два прогона: тексты те же, значит кэш по
  //    тексту сработал. Без этой проверки кэш легко перестал бы работать незаметно.
  check(ctx.compiled_programs() == 4, "четыре программы скомпилированы один раз на оба прогона");

  // 8. То, ради чего всё это: наружу уехала СВОДКА, а не картинка.
  check(first.summary_bytes * 100 < first.image_bytes, "наружу уезжает сводка, а не картинка");

  std::cout << "GN04 verify: " << (checks - failures) << "/" << checks << "\n";
  return failures == 0 ? 0 : 1;
}
} // namespace

int main(const int argc, const char** argv) {
  try {
    const auto opts = parse_options(argc, argv);

    // Устройства может не быть вовсе, и это ЗАКОННЫЙ ответ, а не провал: очередь обязана иметь путь
    // на CPU, а площадка про устройство — сказать, что проверять нечего, и выйти успешно.
    if (!painter::compute_device_available()) {
      std::cout << "GN04: устройства Vulkan нет — проверять нечего\n";
      return 0;
    }

    if (opts.verify) {
      return run_verify(opts);
    }
    return run_once(opts);
  } catch (const std::exception& error) {
    std::cerr << "GN04: " << error.what() << "\n";
    return 1;
  }
}
