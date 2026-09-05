#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/originator/device_queue.h"
#include "devils_engine/painter/compute_context.h"
#include "devils_engine/utils/core.h"

using namespace devils_engine;

// GN04 — ФОНОВЫЕ ТЕКСТУРЫ НА УСТРОЙСТВЕ, СОСТАВЛЕННЫЕ ОЧЕРЕДЬЮ.
//
// Случай выбран не наугад. `ORIGINATOR_GPGPU.md` §5 п.5 называет именно его, потому что здесь НЕ
// МЕШАЕТ ни одна слабость очереди: результат остаётся на устройстве, поэтому передача обратно не
// съедает выигрыш (§4.5); детерминизм не требуется, потому что это ПРЕДСТАВЛЕНИЕ, а не симуляция
// (§4.2 и водораздел `NETWORKING.md`); очередь короткая, поэтому мёртвую работу видно глазами.
// Проверять конструкцию надо там, где её слабости ни при чём — иначе замер меряет слабости.
//
// ПРОХОДЫ ЗДЕСЬ БОЛЬШЕ НЕ СВЯЗАНЫ РУКАМИ. Площадка объявляет четыре вызова и границу, а pipeline —
// программы, наборы дескрипторов, порядок, барьеры, загрузку и выгрузку — составляет
// `originator::device_queue` из этого объявления (§8). Ни одной команды Vulkan площадка не отдаёт.
//
// Что доказывается числами:
//   1. результат ОСТАЁТСЯ на устройстве, и величина эта больше не на совести площадки: её называет
//      ПЛАН. Рядом строится второй план из тех же вызовов с другой границей — он скачивает всё, и
//      отношение двух чисел и есть смысл §5 п.5;
//   2. РОД РЕСУРСА ВЫВОДИТСЯ (§6.2/6.3): из четырёх полей растра картинкой становится РОВНО ОДНО —
//      то, которое читают фильтром. Остальные остаются буферами, потому что по всем прочим пунктам
//      буфер лучше, и никто этого не объявлял;
//   3. свёртка ЦЕЛЫМИ через атомики воспроизводима, хотя порядок прихода групп не закреплён;
//   4. у той же очереди есть путь на CPU, и на целочисленных решениях два пути совпадают ТОЧНО.
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

// =============================================================================================
// ТЕЛА ВЫЗОВОВ НА УСТРОЙСТВЕ.
//
// Это ТЕЛА, а не шейдеры: привязки собирает `build_device_shader`, потому что род каждого поля
// выводится из ВСЕЙ очереди, а не из одного вызова. Поэтому здесь нет ни одного `layout(...)` — к
// полю обращаются аксессором, и тот же текст работает и над буфером, и над картинкой.
//
// Длина входа тоже не приезжает параметром: `in_0_length()` — это длина ПРИВЯЗКИ. Число сайтов
// параметром было бы вторым источником той же истины.
// =============================================================================================

// ПРОХОД 1: разметка Вороного. Для каждого пикселя ищется ближайший сайт; в одно поле уезжает НОМЕР
// области, в другое — разница расстояний до ближайшего и второго, то есть близость к границе.
//
// Апертура `gather`: пиксель читает произвольные сайты и пишет свой.
constexpr std::string_view label_body = R"glsl(
  uint width = args.extent_x;
  uint height = args.extent_y;
  vec2 position = (vec2(float(index % width), float(index / width)) + vec2(0.5)) /
                  vec2(float(width), float(height));

  float nearest = 1e30;
  float second = 1e30;
  uint winner = 0u;
  uint site_count = in_0_length();
  for (uint i = 0u; i < site_count; ++i) {
    vec2 delta = vec2(in_0_at(i), in_1_at(i)) - position;
    float distance = dot(delta, delta);
    if (distance < nearest) {
      second = nearest;
      nearest = distance;
      winner = i;
    } else if (distance < second) {
      second = distance;
    }
  }

  out_0_set(index, winner);
  out_1_set(index, sqrt(second) - sqrt(nearest));
)glsl";

// ПРОХОД 2: сглаживание С ФИЛЬТРОМ. Единственный вызов, объявляющий свой вход ФИЛЬТРУЕМЫМ, — и
// единственная причина, по которой одно поле растра становится картинкой.
//
// Читается по НЕЦЕЛОЙ координате: четыре отсчёта на половине текселя, и каждый из них аппаратный
// bilinear уже усреднил по четырём соседям. У буфера такого чтения нет — там пришлось бы читать
// шестнадцать элементов вручную.
//
// И это же граница: точность фильтра в Vulkan implementation-defined, поэтому такой проход
// принадлежит классу ПРЕДСТАВЛЕНИЯ и в чанковую генерацию не пускается.
constexpr std::string_view smooth_body = R"glsl(
  vec2 texel = vec2(1.0) / vec2(float(args.extent_x), float(args.extent_y));
  vec2 uv = (vec2(float(index % args.extent_x), float(index / args.extent_x)) + vec2(0.5)) * texel;
  vec2 offset = texel * args.radius;

  float sum = in_0_sample(uv + vec2(-offset.x, -offset.y));
  sum += in_0_sample(uv + vec2(offset.x, -offset.y));
  sum += in_0_sample(uv + vec2(-offset.x, offset.y));
  sum += in_0_sample(uv + vec2(offset.x, offset.y));

  out_0_set(index, sum * 0.25);
)glsl";

// ПРОХОД 3: видимая текстура. Номер области превращается в цвет, сглаженная близость к границе — в
// затемнение. На хост это не едет: дальше её читал бы пост-процесс, и §5 п.5 именно про это.
//
// Поэтому `colour` названа не в `output`, а в `resident`: она ЖИВАЯ, но остаётся на устройстве.
constexpr std::string_view compose_body = R"glsl(
  uint region = in_0_at(index);
  float edge = in_1_at(index);

  // Цвет ВЫВОДИТСЯ из номера области хешем: таблицы цветов у площадки нет и не нужно, а один и тот
  // же номер обязан давать один и тот же цвет.
  uint h = region * 2654435761u;
  vec3 base = vec3(float((h >> 16) & 255u), float((h >> 8) & 255u), float(h & 255u)) / 255.0;

  float shade = smoothstep(0.0, args.border, edge);
  vec3 colour = clamp(base * (0.25 + 0.75 * shade), vec3(0.0), vec3(1.0));
  uvec3 bytes = uvec3(colour * 255.0 + vec3(0.5));
  out_0_set(index, bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (255u << 24u));
)glsl";

// ПРОХОД 4: СВОДКА, и только она уезжает наружу. Гистограмма по областям через атомики.
//
// Апертура `scatter`: пиксель пишет в ЧУЖОЙ индекс. В очередь она пускается ровно потому, что вызов
// объявляет свои записи НЕЗАВИСИМЫМИ ОТ ПОРЯДКА — целочисленное сложение коммутативно. Плавающее
// накопление объявить так нельзя, и план это отклоняет.
//
// Из того же объявления следует, что счётчики ЗАГРУЖАЮТСЯ: накопитель читает то, во что пишет,
// значит начальное значение обязано приехать с хоста. Без этого атомик прибавлял бы к мусору
// аллокатора, и сводка вышла бы правдоподобной и неверной.
constexpr std::string_view histogram_body = R"glsl(
  uint region = in_0_at(index);
  if (region >= out_0_length()) return;
  out_0_add(region, 1u);
)glsl";

// =============================================================================================
// ТЕ ЖЕ ЧЕТЫРЕ ПРОХОДА НА CPU.
//
// Нужны не для скорости: очередь ОБЯЗАНА иметь путь на CPU (§4.6), и без него объявление было бы
// объявлением для одного устройства. Заодно они и есть эталон сверки — та же работа, посчитанная
// другой арифметикой.
// =============================================================================================

using accessor = originator::const_field_accessor;

float sample_bilinear(const accessor& field, const size_t width, const size_t height, const float u,
                      const float v) {
  // Повторяет то, что делает сэмплер: координата в текселях смещена на полтексела, край зажат.
  // ТОЧНОГО совпадения с устройством это не даёт и дать не может — точность фильтра в Vulkan
  // implementation-defined, и §6.3 называет это вслух.
  const float x = u * float(width) - 0.5f;
  const float y = v * float(height) - 0.5f;
  const auto x0 = int64_t(std::floor(x));
  const auto y0 = int64_t(std::floor(y));
  const float fx = x - float(x0);
  const float fy = y - float(y0);

  const auto at = [&](int64_t xi, int64_t yi) {
    xi = std::clamp<int64_t>(xi, 0, int64_t(width) - 1);
    yi = std::clamp<int64_t>(yi, 0, int64_t(height) - 1);
    return float(field.get(size_t(yi) * width + size_t(xi)));
  };

  const float top = at(x0, y0) * (1.0f - fx) + at(x0 + 1, y0) * fx;
  const float bottom = at(x0, y0 + 1) * (1.0f - fx) + at(x0 + 1, y0 + 1) * fx;
  return top * (1.0f - fy) + bottom * fy;
}

void cpu_label(const originator::queue_call& call, const std::string_view&, thread::atomic_pool*) {
  const auto sites_x = call.inputs[0].read();
  const auto sites_y = call.inputs[1].read();
  const auto region = call.outputs[0].write();
  const auto edge = call.outputs[1].write();

  const auto extent = call.outputs[0].extent();
  const size_t width = extent.x;
  const size_t height = extent.y;
  const size_t sites = sites_x.count();

  for (size_t index = call.range_begin; index < call.range_end; ++index) {
    const float px = (float(index % width) + 0.5f) / float(width);
    const float py = (float(index / width) + 0.5f) / float(height);

    float nearest = 1e30f;
    float second = 1e30f;
    uint32_t winner = 0;
    for (size_t i = 0; i < sites; ++i) {
      const float dx = float(sites_x.get(i)) - px;
      const float dy = float(sites_y.get(i)) - py;
      const float distance = dx * dx + dy * dy;
      if (distance < nearest) {
        second = nearest;
        nearest = distance;
        winner = uint32_t(i);
      } else if (distance < second) {
        second = distance;
      }
    }

    region.set(index, double(winner));
    edge.set(index, double(std::sqrt(second) - std::sqrt(nearest)));
  }
}

void cpu_smooth(const originator::queue_call& call, const std::string_view&, thread::atomic_pool*) {
  const auto source = call.inputs[0].read();
  const auto target = call.outputs[0].write();
  const auto extent = call.outputs[0].extent();
  const size_t width = extent.x;
  const size_t height = extent.y;
  const float radius = float(call.params.number("radius", 1.0));

  for (size_t index = call.range_begin; index < call.range_end; ++index) {
    const float texel_x = 1.0f / float(width);
    const float texel_y = 1.0f / float(height);
    const float u = (float(index % width) + 0.5f) * texel_x;
    const float v = (float(index / width) + 0.5f) * texel_y;
    const float ox = texel_x * radius;
    const float oy = texel_y * radius;

    float sum = sample_bilinear(source, width, height, u - ox, v - oy);
    sum += sample_bilinear(source, width, height, u + ox, v - oy);
    sum += sample_bilinear(source, width, height, u - ox, v + oy);
    sum += sample_bilinear(source, width, height, u + ox, v + oy);
    target.set(index, double(sum * 0.25f));
  }
}

float smoothstep_at(const float edge0, const float edge1, const float value) {
  const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

void cpu_compose(const originator::queue_call& call, const std::string_view&, thread::atomic_pool*) {
  const auto region = call.inputs[0].read();
  const auto smoothed = call.inputs[1].read();
  const auto colour = call.outputs[0].write();
  const float border = float(call.params.number("border", 0.02));

  for (size_t index = call.range_begin; index < call.range_end; ++index) {
    const uint32_t label = uint32_t(region.get(index));
    const uint32_t h = label * 2654435761u;
    const float base[3] = {float((h >> 16) & 255u) / 255.0f, float((h >> 8) & 255u) / 255.0f,
                           float(h & 255u) / 255.0f};

    const float shade = smoothstep_at(0.0f, border, float(smoothed.get(index)));
    uint32_t packed = 255u << 24;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const float value = std::clamp(base[channel] * (0.25f + 0.75f * shade), 0.0f, 1.0f);
      packed |= uint32_t(value * 255.0f + 0.5f) << (channel * 8);
    }
    colour.set(index, double(packed));
  }
}

void cpu_histogram(const originator::queue_call& call, const std::string_view&, thread::atomic_pool*) {
  const auto region = call.inputs[0].read();
  const auto counts = call.outputs[0].write();
  const size_t buckets = counts.count();

  // ОДНИМ ПОТОКОМ намеренно: накопление здесь целочисленное, а значит от порядка не зависит, и
  // однопоточный обход даёт ровно тот же ответ, что и атомики на устройстве. Разбивать эталон на
  // потоки значило бы заводить второй механизм ради того же числа.
  for (size_t index = call.range_begin; index < call.range_end; ++index) {
    const auto label = size_t(region.get(index));
    if (label >= buckets) continue;
    counts.set(label, counts.get(label) + 1.0);
  }
}

// =============================================================================================
// БУФЕРЫ И ОЧЕРЕДЬ.
// =============================================================================================

struct scene {
  originator::buffer sites;
  originator::buffer pixels;
  originator::buffer summary;
};

uint32_t hash_u32(uint32_t value) noexcept {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

using field_pair = std::pair<std::string_view, std::string_view>;

// Сайты расставляет ХОСТ, и это ровно тот случай, который правилом разрешён: множество маленькое и
// перечислено самим хостом, а не обходом плотного буфера.
scene make_scene(const options& opts) {
  scene result;

  const std::vector<field_pair> site_fields = {{"x", "v1"}, {"y", "v1"}};
  result.sites = originator::buffer(
    "sites", originator::make_buffer_layout(originator::storage_kind::soa, site_fields, "sites"), size_t(opts.sites));

  // РАСТР ОБЪЯВЛЯЕТ СВОЮ ФОРМУ, а не получает ширину параметром: `extent` — единственный источник
  // этого числа, и из него же следует, что поле МОЖЕТ стать картинкой (§6.2).
  //
  // `region` и `colour` — целые не для экономии: номер области это метка, а цвет это четыре байта.
  // Считать метку во float значило бы усреднять номера, чего никто не хочет.
  const std::vector<field_pair> pixel_fields = {
    {"region", "ui1"}, {"edge", "v1"}, {"smoothed", "v1"}, {"colour", "ui1"}};
  result.pixels = originator::buffer(
    "pixels", originator::make_buffer_layout(originator::storage_kind::soa, pixel_fields, "pixels"),
    originator::buffer_extent{opts.size, opts.size, 0});

  const std::vector<field_pair> summary_fields = {{"count", "ui1"}};
  result.summary = originator::buffer(
    "summary", originator::make_buffer_layout(originator::storage_kind::soa, summary_fields, "summary"),
    size_t(opts.sites));

  auto x = result.sites.field(result.sites.find_field("x"));
  auto y = result.sites.field(result.sites.find_field("y"));
  for (uint32_t i = 0; i < opts.sites; ++i) {
    x.set(i, double(hash_u32(opts.seed ^ (i * 2u + 1u)) >> 8) / double(1u << 24));
    y.set(i, double(hash_u32(opts.seed ^ (i * 2u + 2u)) >> 8) / double(1u << 24));
  }
  return result;
}

// Гистограмма обнуляется ЯВНО, и план делает это видимым: накопитель читает то, во что пишет,
// поэтому `summary.count` попадает в загрузку очереди наравне с сайтами. Мусор в счётчиках стал бы
// частью ответа, а по правдоподобным числам этого не видно.
void reset_summary(scene& target) {
  auto counts = target.summary.field(target.summary.find_field("count"));
  for (size_t i = 0; i < counts.count(); ++i) {
    counts.set(i, 0.0);
  }
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

// Где проходит граница очереди. Различаются два плана РОВНО этим — набором вызовов они одинаковы, и
// в этом весь смысл сравнения: `output` не меняет ни одного вычисления, он меняет только передачу.
enum class boundary {
  production, // наружу едет только сводка, картинка остаётся на устройстве
  inspect,    // наружу едет всё: так площадка получает материал для сверки и для глаза
};

originator::computation_queue build_queue(scene& target, const options& opts, const boundary where) {
  const size_t pixels = size_t(opts.size) * size_t(opts.size);

  originator::computation_queue queue;
  queue.name = "texture";

  {
    originator::queue_call call;
    call.label = "voronoi_label";
    call.body = cpu_label;
    call.shape = originator::aperture::gather;
    call.inputs = {readable(target.sites, "x"), readable(target.sites, "y")};
    call.outputs = {writable(target.pixels, "region"), writable(target.pixels, "edge")};
    call.range_end = pixels;
    call.device_body = std::string(label_body);
    queue.calls.push_back(std::move(call));
  }

  {
    originator::queue_call call;
    call.label = "smooth_edges";
    call.body = cpu_smooth;
    call.shape = originator::aperture::gather;
    call.inputs = {readable(target.pixels, "edge")};
    call.outputs = {writable(target.pixels, "smoothed")};
    call.params.set_number("radius", 1.0);
    call.range_end = pixels;
    call.device_body = std::string(smooth_body);
    call.device_params = {{"radius", 1.0}};
    // ЕДИНСТВЕННОЕ ОБЪЯВЛЕНИЕ, ИЗ КОТОРОГО СЛЕДУЕТ КАРТИНКА. Не «сделай `edge` образом», а «этот вход
    // читают между элементами»; в картинку его превращает план, потому что больше ничем картинка от
    // буфера в лучшую сторону не отличается.
    call.device_filtered_inputs = {0};
    queue.calls.push_back(std::move(call));
  }

  {
    originator::queue_call call;
    call.label = "compose";
    call.body = cpu_compose;
    call.shape = originator::aperture::pointwise;
    call.inputs = {readable(target.pixels, "region"), readable(target.pixels, "smoothed")};
    call.outputs = {writable(target.pixels, "colour")};
    call.params.set_number("border", 0.02);
    call.range_end = pixels;
    call.device_body = std::string(compose_body);
    call.device_params = {{"border", 0.02}};
    queue.calls.push_back(std::move(call));
  }

  {
    originator::queue_call call;
    call.label = "histogram";
    call.body = cpu_histogram;
    call.shape = originator::aperture::scatter;
    call.inputs = {readable(target.pixels, "region")};
    call.outputs = {writable(target.summary, "count")};
    call.range_end = pixels;
    call.device_body = std::string(histogram_body);
    // Целочисленное накопление коммутативно — только это и пускает scatter в очередь.
    call.order_free_writes = true;
    queue.calls.push_back(std::move(call));
  }

  if (where == boundary::production) {
    queue.output = {writable(target.summary, "count")};
    // ЖИВАЯ, НО ОСТАЁТСЯ. Читатель у неё есть — пост-процесс, — просто он живёт на устройстве, и
    // очередь его не видит, ровно как не видит читателя из `output`.
    queue.resident = {writable(target.pixels, "colour")};
    return queue;
  }

  queue.output = {writable(target.summary, "count"), writable(target.pixels, "region"),
                  writable(target.pixels, "edge"), writable(target.pixels, "smoothed"),
                  writable(target.pixels, "colour")};
  return queue;
}

double milliseconds_since(const std::chrono::steady_clock::time_point start) {
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

std::vector<uint32_t> read_field(const originator::buffer& source, const std::string_view& name) {
  const auto field = source.field(source.find_field(name));
  std::vector<uint32_t> values(field.count());
  for (size_t i = 0; i < values.size(); ++i) {
    values[i] = uint32_t(field.get(i));
  }
  return values;
}

std::vector<float> read_floats(const originator::buffer& source, const std::string_view& name) {
  const auto field = source.field(source.find_field(name));
  std::vector<float> values(field.count());
  for (size_t i = 0; i < values.size(); ++i) {
    values[i] = float(field.get(i));
  }
  return values;
}

bool write_ppm(const std::string& path, const std::vector<uint32_t>& packed, const uint32_t width,
               const uint32_t height) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << width << " " << height << "\n255\n";
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    out.put(char(packed[i] & 255u));
    out.put(char((packed[i] >> 8) & 255u));
    out.put(char((packed[i] >> 16) & 255u));
  }
  return bool(out);
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

void print_plan(const std::string_view& title, const originator::device_queue& plan,
                const originator::device_report& report) {
  std::cout << "  " << title << ":\n";
  std::cout << "    вызовов " << report.calls << ", барьеров " << report.barriers << ", картинок "
            << report.images << "\n";

  std::cout << "    загружено ";
  for (const auto& name : plan.uploaded_fields()) {
    std::cout << name << " ";
  }
  std::cout << "(" << report.upload_bytes << " байт)\n";

  std::cout << "    выгружено ";
  for (const auto& name : plan.downloaded_fields()) {
    std::cout << name << " ";
  }
  std::cout << "(" << report.download_bytes << " байт)\n";
}

int run_once(const options& opts) {
  painter::compute_context_config config;
  config.app_name = "GN04";
  painter::compute_context ctx(config);

  auto target = make_scene(opts);
  reset_summary(target);

  std::cout << "GN04: текстура " << opts.size << "x" << opts.size << ", областей " << opts.sites
            << ", устройство '" << ctx.device_name() << "'\n";

  auto declared = build_queue(target, opts, boundary::production);
  originator::device_queue plan(ctx, declared);

  const auto at = std::chrono::steady_clock::now();
  const auto report = plan.run();
  const double wall_ms = milliseconds_since(at);

  print_plan("план", plan, report);
  std::cout << "    запись " << report.record_ms << " мс, отправка " << report.submit_ms
            << " мс, всего " << wall_ms << " мс\n";

  // ВТОРОЙ ПЛАН из ТЕХ ЖЕ вызовов и с другой границей. Он НЕ ИСПОЛНЯЕТСЯ, и в этом суть: вся передача
  // выведена уже при составлении, поэтому сравнивать надо именно ПЛАНЫ. Работа у них одна и та же —
  // отличается только то, что объявлено уезжающим.
  auto everything = build_queue(target, opts, boundary::inspect);
  originator::device_queue full(ctx, everything);
  std::cout << "  та же работа с границей 'всё наружу': выгрузка " << full.download_byte_count()
            << " байт\n";
  std::cout << "  ОТНОШЕНИЕ " << (double(full.download_byte_count()) / double(report.download_bytes))
            << ":1 — во столько раз больше уехало бы, если бы картинка считалась результатом\n";

  const auto histogram = read_field(target.summary, "count");
  uint32_t smallest = UINT32_MAX;
  uint32_t largest = 0;
  size_t total = 0;
  for (const auto count : histogram) {
    smallest = std::min(smallest, count);
    largest = std::max(largest, count);
    total += count;
  }
  std::cout << "  области             от " << smallest << " до " << largest << " пикселей, сумма " << total
            << "\n";
  std::cout << "  картинкой стало     " << report.images << " поле из четырёх ('pixels.edge' — "
            << (plan.is_image("pixels.edge") ? "да" : "нет") << ", 'pixels.region' — "
            << (plan.is_image("pixels.region") ? "да" : "нет") << ")\n";

  if (!opts.dump.empty()) {
    // Картинку приходится посчитать ВТОРЫМ планом: у производственного она с устройства не уезжает,
    // и это не обходной путь, а буквально то, что план объявляет. Накопитель перед прогоном
    // обнуляется — он читает то, во что пишет, и счётчики прошлого прогона удвоили бы сводку.
    reset_summary(target);
    full.run();
    const auto colour = read_field(target.pixels, "colour");
    if (!write_ppm(opts.dump, colour, opts.size, opts.size)) {
      utils::error{}("GN04: could not write '{}'", opts.dump);
    }
    std::cout << "  картинка для глаза  " << opts.dump << " (через границу 'всё наружу', не через план)\n";
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

  const size_t pixels = size_t(opts.size) * size_t(opts.size);

  auto device_scene = make_scene(opts);
  auto host_scene = make_scene(opts);

  auto production = build_queue(device_scene, opts, boundary::production);
  originator::device_queue plan(ctx, production);

  // 1. ПЛАН ВЫВЕДЕН, а не сказан. Загружается ровно то, что вызовы читают и чего до них никто не
  //    писал: два поля сайтов и — потому что накопитель читает то, во что пишет, — счётчики.
  const std::vector<std::string> expected_upload = {"sites.x", "sites.y", "summary.count"};
  check(plan.uploaded_fields() == expected_upload, "загружаются входы очереди и начальное состояние накопителя");

  // 2. Выгружается ровно `output`. Растр не уезжает вовсе, хотя посчитан целиком.
  const std::vector<std::string> expected_download = {"summary.count"};
  check(plan.downloaded_fields() == expected_download, "выгружается ровно объявленная граница передачи");

  // 3. РОД ВЫВЕДЕН. Картинкой стало ровно одно поле — то, которое читают фильтром. Проверка нужна
  //    именно потому, что план, разложивший в картинки всё подряд, считает то же самое.
  check(plan.image_count() == 1, "картинкой стало ровно одно поле");
  check(plan.is_image("pixels.edge"), "картинкой стало то поле, которое читают фильтром");
  check(!plan.is_image("pixels.region"), "поле, которое читают по своему индексу, осталось буфером");
  check(!plan.is_image("pixels.colour"), "поле, которое никто не читает фильтром, осталось буфером");

  // 4. Барьеры выведены тем же вопросом, что проверка мёртвой работы: три прохода из четырёх читают
  //    то, что записал предыдущий, у первого читать нечего.
  check(plan.barrier_count() == 3, "барьер стоит ровно там, где есть зависимость");

  reset_summary(device_scene);
  const auto report = plan.run();

  // 5. То, ради чего всё это. Та же работа с другой границей выгружает на три порядка больше.
  auto everything = build_queue(device_scene, opts, boundary::inspect);
  originator::device_queue full(ctx, everything);
  check(full.downloaded_fields().size() == 5, "второй план объявляет наружу всё");

  // НАКОПИТЕЛЬ ОБНУЛЯЕТСЯ ПЕРЕД КАЖДЫМ ПРОГОНОМ, и об этом нельзя забыть: он читает то, во что
  // пишет, поэтому счётчики с прошлого прогона уехали бы на устройство начальным значением и сводка
  // удвоилась бы. План делает это видимым — `summary.count` стоит в списке ЗАГРУЖАЕМЫХ.
  reset_summary(device_scene);
  const auto full_report = full.run();
  check(report.download_bytes * 1000 < full_report.download_bytes,
        "производственная граница уезжает наружу на три порядка меньше");

  // Второй план посчитал всё заново и скачал: дальше сверяется именно он.
  const auto device_region = read_field(device_scene.pixels, "region");
  const auto device_edge = read_floats(device_scene.pixels, "edge");
  const auto device_smoothed = read_floats(device_scene.pixels, "smoothed");
  const auto device_summary = read_field(device_scene.summary, "count");

  // 6. Каждый пиксель попал в какую-то область, и ни одна не пуста.
  size_t total = 0;
  bool all_present = true;
  for (const auto count : device_summary) {
    total += count;
    all_present = all_present && count > 0;
  }
  check(total == pixels, "гистограмма по областям сходится с числом пикселей");
  check(all_present, "ни одна область не осталась пустой");

  // 7. ТОТ ЖЕ НАБОР ВЫЗОВОВ НА CPU. Не «похожая проверка», а буквально та же очередь: у неё два пути,
  //    и второй обязан существовать (§4.6).
  auto host_queue = build_queue(host_scene, opts, boundary::inspect);
  reset_summary(host_scene);
  originator::run_queue(host_queue, nullptr);

  const auto host_region = read_field(host_scene.pixels, "region");
  const auto host_edge = read_floats(host_scene.pixels, "edge");
  const auto host_smoothed = read_floats(host_scene.pixels, "smoothed");

  // 8. РЕШЕНИЕ ЦЕЛОЧИСЛЕННОЕ — значит совпадение обязано быть ТОЧНЫМ, кроме пикселей ровно на
  //    границе, где решение принимает последний бит.
  size_t label_differences = 0;
  for (size_t i = 0; i < pixels; ++i) {
    label_differences += size_t(device_region[i] != host_region[i]);
  }
  check(label_differences * 1000 < pixels, "разметка совпадает с путём на CPU (кроме пикселей на границе)");
  std::cout << "  разметка: " << label_differences << " пикселей из " << pixels
            << " решены иначе (граничные)\n";

  // Сводка сверяется НЕ с гистограммой другого пути, а с гистограммой ТЕХ ЖЕ меток, посчитанной на
  // хосте. Иначе проверка была бы бессмысленной там, где она нужнее всего: у пикселей на границе
  // метки законно расходятся, и любое расхождение сводки списывалось бы на них. Здесь же метки одни
  // и те же, поэтому равенство обязано быть ТОЧНЫМ — и проверяет оно ровно атомарный разброс.
  std::vector<uint32_t> expected(opts.sites, 0);
  for (const auto label : device_region) {
    if (label < expected.size()) ++expected[label];
  }
  check(device_summary == expected, "атомарная свёртка сосчитала ровно те метки, что записаны в поле");

  // 9. Плавающая часть совпадает в пределах float — и ровно до фильтра. §6.3 объявил, что точность
  //    фильтра в Vulkan implementation-defined, поэтому у сглаженного поля допуск ШИРЕ, и это не
  //    поблажка, а объявленная граница.
  double worst_edge = 0.0;
  double worst_smoothed = 0.0;
  for (size_t i = 0; i < pixels; ++i) {
    worst_edge = std::max(worst_edge, std::abs(double(device_edge[i]) - double(host_edge[i])));
    worst_smoothed = std::max(worst_smoothed, std::abs(double(device_smoothed[i]) - double(host_smoothed[i])));
  }
  check(worst_edge < 1e-5, "близость к границе совпадает в пределах float");
  check(worst_smoothed < 1e-2, "сглаженное фильтром совпадает лишь приблизительно — и это объявлено");
  std::cout << "  расхождение путей: близость " << worst_edge << ", сглаженное " << worst_smoothed << "\n";

  // 10. ФИЛЬТР ДЕЙСТВИТЕЛЬНО РАБОТАЛ. Иначе проход выборки был бы дорогим копированием, и заметить
  //     это по картинке нельзя.
  size_t changed = 0;
  double widest = 0.0;
  float lowest = device_edge[0];
  float highest = device_edge[0];
  for (size_t i = 0; i < pixels; ++i) {
    const double delta = std::abs(double(device_smoothed[i]) - double(device_edge[i]));
    widest = std::max(widest, delta);
    changed += size_t(delta > 1e-6);
    lowest = std::min(lowest, device_edge[i]);
    highest = std::max(highest, device_edge[i]);
  }
  check(changed * 10 > pixels, "чтение с фильтром изменило поле, а не скопировало его");
  check(widest > 1e-4, "изменение фильтра заметно по величине");

  // 11. Сглаживание не выводит значения за пределы исходного диапазона: среднее четырёх отсчётов
  //     лежит в выпуклой оболочке, а нарушение означало бы чтение мимо картинки.
  bool inside = true;
  for (const auto value : device_smoothed) {
    inside = inside && value >= lowest - 1e-5f && value <= highest + 1e-5f;
  }
  check(inside, "сглаженное поле остаётся внутри диапазона исходного");

  // 12. СВЁРТКА ЦЕЛЫМИ ЧЕРЕЗ АТОМИКИ ВОСПРОИЗВОДИМА. Порядок прихода групп ничем не закреплён, но
  //     целочисленное сложение от порядка не зависит — а плавающее зависит, и §4.3 называет это
  //     единственным больным местом свёрток.
  reset_summary(device_scene);
  plan.run();
  const auto again = read_field(device_scene.summary, "count");
  check(again == expected, "гистограмма через атомики повторяется прогон в прогон");

  // 13. Компиляция случилась РОВНО ЧЕТЫРЕ раза на два плана: тексты те же, значит кэш по тексту
  //     сработал. Без этой проверки он перестал бы работать незаметно.
  check(ctx.compiled_programs() == 4, "четыре программы скомпилированы один раз на оба плана");

  // 14. Цвет собран, а не оставлен нулём: у видимой текстуры обязан быть непрозрачный альфа-канал и
  //     больше одного оттенка, иначе проход сборки ничего не сделал бы и остался бы незамеченным.
  const auto colour = read_field(device_scene.pixels, "colour");
  size_t opaque = 0;
  uint32_t first = colour[0];
  bool varied = false;
  for (const auto value : colour) {
    opaque += size_t((value >> 24) == 255u);
    varied = varied || value != first;
  }
  check(opaque == pixels, "у собранной текстуры непрозрачный альфа-канал");
  check(varied, "собранная текстура не одноцветная");

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
