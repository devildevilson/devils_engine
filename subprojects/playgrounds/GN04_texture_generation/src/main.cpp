#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/device_executor.h"
#include "devils_engine/originator/generator_resource.h"
#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/primitives.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/painter/compute_context.h"
#include "devils_engine/utils/core.h"

// GN04 — ФОНОВЫЕ ТЕКСТУРЫ НА УСТРОЙСТВЕ.
//
// Случай выбран не наугад. `ORIGINATOR_GPGPU.md` §5 п.5 называет именно его, потому что здесь НЕ
// МЕШАЕТ ни одна слабость очереди: результат остаётся на устройстве (передача не платится),
// детерминизм не требуется (это ПРЕДСТАВЛЕНИЕ, а не симуляция), свёртка целочисленная (порядок
// прихода групп не важен), цепочка короткая. Проверять конструкцию надо там, где её слабости ни при
// чём — иначе замер меряет слабости.
//
// ПЛОЩАДКА НЕ СОДЕРЖИТ НИ ОДНОЙ СТРОКИ GLSL И НИ ОДНОЙ КОМАНДЫ VULKAN. Пайплайн приезжает конфигом,
// как у остальных площадок кампании: буферы объявлены в `buffers.tavl`, пороги в `values.tavl`, а
// очередь — в теле шага. Устройственные формы всех шести проходов написаны заранее и лежат в
// библиотеке рядом с их телами на CPU: что считает `voronoi_label` или `count_by`, решили
// разработчики движка, и никакой шейдер сюда не приезжает.
//
// Что доказывается числами:
//   1. результат ОСТАЁТСЯ на устройстве, и величину эту называет ПЛАН, а не площадка: тот же набор
//      вызовов с другой объявленной границей скачивает на три порядка больше;
//   2. РОД РЕСУРСА ВЫВОДИТСЯ: из четырёх полей растра картинкой становится ровно одно — то, которое
//      читают фильтром;
//   3. свёртка ЦЕЛЫМИ через атомики воспроизводима, хотя порядок прихода групп не закреплён;
//   4. у той же очереди есть путь на CPU, и на целочисленных решениях два пути совпадают ТОЧНО.

namespace {

namespace fs = std::filesystem;
using namespace devils_engine;

struct options {
  size_t size = 1024;
  size_t sites = 64;
  uint64_t seed = 20260904;
  bool verify = false;
  std::string dump;
};

fs::path resource_root() {
  return fs::path(GN04_RESOURCE_ROOT);
}

// Генератор приезжает через demiurg: хост знает одно имя, `generator/texture`, а что этот генератор
// разложен на документ значений, документ буферов и папку скриптов — знает сама точка входа.
struct generator_registry {
  demiurg::module_system modules;
  demiurg::resource_system resources;
  originator::generator_config config;

  generator_registry() : modules(resource_root().generic_string() + "/") {
    modules.load_modules({demiurg::module_system::list_entry{"gn04/", "", ""}});
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
    config = originator::load_generator(resources, "generator/texture");
  }
};

const generator_registry& generator() {
  static const generator_registry registry;
  return registry;
}

bool starts_with(const std::string_view& text, const std::string_view& prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

options parse_options(const int argc, const char** argv) {
  options result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--verify") {
      result.verify = true;
    } else if (starts_with(argument, "--size=")) {
      result.size = std::stoul(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--sites=")) {
      result.sites = std::stoul(std::string(argument.substr(8)));
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--dump=")) {
      result.dump = std::string(argument.substr(7));
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN04 texture generation lab\n"
                << "  --size=N    сторона текстуры (по умолчанию 1024)\n"
                << "  --sites=N   число областей (по умолчанию 64)\n"
                << "  --seed=N    зерно пайплайна\n"
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

originator::size_table make_sizes(const options& opts) {
  originator::size_table sizes;
  // Сторона растра как КОНСТАНТА РАЗМЕРА: `pixels` объявляет ею свою форму, и она же становится
  // единственным источником ширины — ни один инструмент не получает её параметром.
  sizes.set("side", opts.size);
  sizes.set("site_count", opts.sites);
  return sizes;
}

// Что ИЗ МАНИФЕСТА меняет площадка: путь исполнения и объявленную границу. Оба — числа конфига, а не
// решения по машине: §6.4 требует, чтобы ветку выбирало объявление, одинаковое всюду.
originator::pipeline_description load_description(const bool on_device, const bool download_all) {
  originator::pipeline_description description = generator().config.description;
  description.values.set_number("on_device", on_device ? 1.0 : 0.0);
  description.values.set_number("download_all", download_all ? 1.0 : 0.0);
  return description;
}

void load_bodies(originator::script_host& host, const originator::pipeline_description& description) {
  const auto& package = generator().config;
  for (const auto& step : description.steps) {
    host.load_body(step.name, package.source(step.body), step.body);
    for (const auto& [program_name, id] : step.programs) {
      host.load_program(program_name, package.source(id));
    }
  }
}

originator::tool_registry& tools() {
  static originator::tool_registry registry;
  if (registry.size() == 0) {
    registry.add_standard_tools();
    originator::add_all_primitives(registry);
  }
  return registry;
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

double milliseconds_since(const std::chrono::steady_clock::time_point start) {
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

bool write_ppm(const std::string& path, const std::vector<uint32_t>& packed, const size_t side) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "P6\n" << side << " " << side << "\n255\n";
  for (size_t i = 0; i < side * side; ++i) {
    out.put(char(packed[i] & 255u));
    out.put(char((packed[i] >> 8) & 255u));
    out.put(char((packed[i] >> 16) & 255u));
  }
  return bool(out);
}

// ОДИН ПРОГОН ПАЙПЛАЙНА. Устройство приходит СНАРУЖИ: ядро генератора про Vulkan не знает, а
// исполнителя устройственных очередей ставит тот, кто собрал приложение.
struct run_outcome {
  double wall_ms = 0.0;
  size_t plans = 0;
  size_t reuse = 0;
};

run_outcome run_pipeline(originator::pipeline& p,
                         const originator::pipeline_description& description,
                         originator::device_executor* executor,
                         const size_t repeats = 1) {
  originator::script_host host(tools(), nullptr);
  if (executor != nullptr) {
    host.set_device_executor(executor);
  }
  load_bodies(host, description);

  run_outcome outcome;
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < repeats; ++i) {
    p.run(host.invoker());
  }
  outcome.wall_ms = milliseconds_since(start);
  if (executor != nullptr) {
    outcome.plans = executor->plan_count();
    outcome.reuse = executor->reuse_count();
  }
  return outcome;
}

void print_plan(const originator::device_queue& plan, const originator::device_report& report) {
  std::cout << "  вызовов " << report.calls << ", барьеров " << report.barriers << ", картинок "
            << report.images << "\n";
  std::cout << "  загружено ";
  for (const auto& name : plan.uploaded_fields()) {
    std::cout << name << " ";
  }
  std::cout << "(" << report.upload_bytes << " байт)\n";
  std::cout << "  выгружено ";
  for (const auto& name : plan.downloaded_fields()) {
    std::cout << name << " ";
  }
  std::cout << "(" << report.download_bytes << " байт)\n";
}

int run_once(const options& opts) {
  painter::compute_context_config config;
  config.app_name = "GN04";
  painter::compute_context ctx(config);
  originator::device_executor executor(ctx);

  std::cout << "GN04: текстура " << opts.size << "x" << opts.size << ", областей " << opts.sites
            << ", устройство '" << ctx.device_name() << "'\n";

  const auto sizes = make_sizes(opts);
  const auto description = load_description(true, !opts.dump.empty());
  originator::pipeline p(description, sizes, opts.seed);
  const auto outcome = run_pipeline(p, description, &executor);

  print_plan(*executor.last_plan(), executor.last_report());
  std::cout << "  запись " << executor.last_report().record_ms << " мс, отправка "
            << executor.last_report().submit_ms << " мс, весь пайплайн " << outcome.wall_ms << " мс\n";

  // ВТОРОЙ ПЛАН из ТЕХ ЖЕ вызовов и с другой границей. Он НЕ исполняется: вся передача выведена уже
  // при составлении, поэтому сравнивать надо именно ПЛАНЫ.
  const auto other_description = load_description(true, opts.dump.empty());
  originator::pipeline other(other_description, sizes, opts.seed);
  originator::device_executor other_executor(ctx);
  run_pipeline(other, other_description, &other_executor);
  const auto here = executor.last_plan()->download_byte_count();
  const auto there = other_executor.last_plan()->download_byte_count();
  std::cout << "  та же работа с другой границей: выгрузка " << there << " байт, ОТНОШЕНИЕ "
            << (double(std::max(here, there)) / double(std::min(here, there)))
            << ":1 — во столько раз больше уехало бы, если бы картинка считалась результатом\n";

  const auto* summary = p.find_buffer("summary");
  const auto histogram = read_field(*summary, "count");
  uint32_t smallest = UINT32_MAX;
  uint32_t largest = 0;
  size_t total = 0;
  for (const auto count : histogram) {
    smallest = std::min(smallest, count);
    largest = std::max(largest, count);
    total += count;
  }
  std::cout << "  области    от " << smallest << " до " << largest << " пикселей, сумма " << total << "\n";
  std::cout << "  картинкой стало " << executor.last_report().images << " поле из четырёх ('pixels.edge' — "
            << (executor.last_plan()->is_image("pixels.edge") ? "да" : "нет") << ", 'pixels.region' — "
            << (executor.last_plan()->is_image("pixels.region") ? "да" : "нет") << ")\n";

  if (!opts.dump.empty()) {
    const auto* pixels = p.find_buffer("pixels");
    const auto colour = read_field(*pixels, "colour");
    if (!write_ppm(opts.dump, colour, opts.size)) {
      utils::error{}("GN04: could not write '{}'", opts.dump);
    }
    std::cout << "  картинка для глаза " << opts.dump
              << " (по объявленной границе 'картинка уезжает', не по производственной)\n";
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
  originator::device_executor executor(ctx);

  std::cout << "GN04 verify: текстура " << opts.size << "x" << opts.size << ", областей " << opts.sites
            << ", устройство '" << ctx.device_name() << "'\n";

  const size_t pixels_count = opts.size * opts.size;
  const auto sizes = make_sizes(opts);

  // ПРОИЗВОДСТВЕННАЯ ГРАНИЦА: наружу едет только сводка.
  const auto production = load_description(true, false);
  originator::pipeline device_run(production, sizes, opts.seed);
  run_pipeline(device_run, production, &executor);

  const auto* plan = executor.last_plan();
  if (plan == nullptr) {
    utils::error{}("GN04: the device queue never ran — the manifest asked for a device");
  }

  // 1. ПЛАН ВЫВЕДЕН, а не сказан. Загружаются только сайты: счётчики пишет `fill` ВНУТРИ очереди,
  //    поэтому передавать их с хоста не нужно вовсе.
  const std::vector<std::string> expected_upload = {"sites.position"};
  check(plan->uploaded_fields() == expected_upload, "загружается только то, что очередь читает и не пишет сама");

  // 2. Выгружается ровно `output`. Растр не уезжает, хотя посчитан целиком.
  const std::vector<std::string> expected_download = {"summary.count"};
  check(plan->downloaded_fields() == expected_download, "выгружается ровно объявленная граница передачи");

  // 3. РОД ВЫВЕДЕН. Картинкой стало ровно одно поле — то, которое читают фильтром.
  check(plan->image_count() == 1, "картинкой стало ровно одно поле");
  check(plan->is_image("pixels.edge"), "картинкой стало то поле, которое читают фильтром");
  check(!plan->is_image("pixels.region"), "поле, которое читают по своему индексу, осталось буфером");
  check(!plan->is_image("pixels.colour"), "поле, которое никто не читает фильтром, осталось буфером");

  // 4. Барьеры выведены тем же вопросом, что проверка мёртвой работы.
  check(executor.last_report().barriers > 0 && executor.last_report().barriers < 6,
        "барьеры стоят там, где есть зависимость, и не везде");

  // 5. ПЛАН ПЕРЕИСПОЛЬЗУЕТСЯ. Тот же пайплайн ещё дважды: план строится один раз, дальше меняются
  //    только числа push-константы. Без этого у стримингового генератора каждый чанк заводил бы свои
  //    буферы устройства и свой набор дескрипторов.
  const auto repeated = run_pipeline(device_run, production, &executor, 2);
  check(repeated.plans == 1, "план построен один раз на все прогоны");
  check(repeated.reuse >= 2, "повторные прогоны переиспользовали план");

  // 6. ТА ЖЕ РАБОТА С ДРУГОЙ ГРАНИЦЕЙ. Отличается ровно одно число конфига.
  const auto inspect = load_description(true, true);
  originator::pipeline device_full(inspect, sizes, opts.seed);
  originator::device_executor full_executor(ctx);
  run_pipeline(device_full, inspect, &full_executor);
  check(full_executor.last_plan()->downloaded_fields().size() == 5, "граница осмотра объявляет наружу весь растр");
  check(plan->download_byte_count() * 1000 < full_executor.last_plan()->download_byte_count(),
        "производственная граница уезжает наружу на три порядка меньше");
  std::cout << "  выгрузка: " << plan->download_byte_count() << " байт против "
            << full_executor.last_plan()->download_byte_count() << "\n";

  // 7. ТОТ ЖЕ ПАЙПЛАЙН НА CPU. Не «похожая проверка», а буквально та же очередь: путь на CPU обязан
  //    существовать (§4.6), и переключает его одно число манифеста.
  const auto on_cpu = load_description(false, true);
  originator::pipeline host_run(on_cpu, sizes, opts.seed);
  run_pipeline(host_run, on_cpu, nullptr);

  const auto* device_pixels = device_full.find_buffer("pixels");
  const auto* host_pixels = host_run.find_buffer("pixels");
  const auto device_region = read_field(*device_pixels, "region");
  const auto host_region = read_field(*host_pixels, "region");
  const auto device_edge = read_floats(*device_pixels, "edge");
  const auto host_edge = read_floats(*host_pixels, "edge");
  const auto device_smoothed = read_floats(*device_pixels, "smoothed");
  const auto host_smoothed = read_floats(*host_pixels, "smoothed");

  // 8. РЕШЕНИЕ ЦЕЛОЧИСЛЕННОЕ — значит совпадение обязано быть ТОЧНЫМ, кроме пикселей ровно на
  //    границе, где решение принимает последний бит.
  size_t label_differences = 0;
  for (size_t i = 0; i < pixels_count; ++i) {
    label_differences += size_t(device_region[i] != host_region[i]);
  }
  check(label_differences * 1000 < pixels_count, "разметка совпадает с путём на CPU");
  std::cout << "  разметка: " << label_differences << " пикселей из " << pixels_count
            << " решены иначе (граничные)\n";

  // 9. Плавающая часть совпадает в пределах float — и ровно до фильтра. §6.3 объявил точность фильтра
  //    implementation-defined, поэтому у сглаженного поля допуск ШИРЕ, и это не поблажка.
  double worst_edge = 0.0;
  double worst_smoothed = 0.0;
  for (size_t i = 0; i < pixels_count; ++i) {
    worst_edge = std::max(worst_edge, std::abs(double(device_edge[i]) - double(host_edge[i])));
    worst_smoothed = std::max(worst_smoothed, std::abs(double(device_smoothed[i]) - double(host_smoothed[i])));
  }
  check(worst_edge < 1e-4, "близость к границе совпадает в пределах float");
  check(worst_smoothed < 1e-2, "сглаженное фильтром совпадает лишь приблизительно — и это объявлено");
  std::cout << "  расхождение путей: близость " << worst_edge << ", сглаженное " << worst_smoothed << "\n";

  // 10. ФИЛЬТР ДЕЙСТВИТЕЛЬНО РАБОТАЛ. Иначе проход выборки был бы дорогим копированием, и заметить
  //     это по картинке нельзя. Сравнивается ДО каймы: `remap` после фильтра зажимает поле.
  size_t changed = 0;
  for (size_t i = 0; i < pixels_count; ++i) {
    changed += size_t(std::abs(double(device_smoothed[i]) - double(device_edge[i])) > 1e-6);
  }
  check(changed * 10 > pixels_count, "чтение с фильтром изменило поле, а не скопировало его");

  // 11. СВЁРТКА. Сверяется не с гистограммой другого пути, а с гистограммой ТЕХ ЖЕ меток на хосте:
  //     иначе проверка была бы пустой там, где нужна больше всего — у граничных пикселей метки
  //     законно расходятся, и любое расхождение сводки списывалось бы на них.
  const auto device_summary = read_field(*device_full.find_buffer("summary"), "count");
  std::vector<uint32_t> expected(opts.sites, 0);
  for (const auto label : device_region) {
    if (label < expected.size()) ++expected[label];
  }
  check(device_summary == expected, "атомарная свёртка сосчитала ровно те метки, что записаны в поле");

  size_t total = 0;
  bool all_present = true;
  for (const auto count : device_summary) {
    total += count;
    all_present = all_present && count > 0;
  }
  check(total == pixels_count, "гистограмма по областям сходится с числом пикселей");
  check(all_present, "ни одна область не осталась пустой");

  // 12. СВЁРТКА ЦЕЛЫМИ ЧЕРЕЗ АТОМИКИ ВОСПРОИЗВОДИМА: порядок прихода групп ничем не закреплён, но
  //     целое сложение от порядка не зависит.
  run_pipeline(device_full, inspect, &full_executor);
  const auto again = read_field(*device_full.find_buffer("summary"), "count");
  check(again == expected, "гистограмма через атомики повторяется прогон в прогон");

  // 13. Компиляция случилась по разу на программу: тексты те же, значит кэш по тексту сработал.
  check(ctx.compiled_programs() == 6, "шесть программ скомпилированы по одному разу на все планы");

  // 14. Цвет собран, а не оставлен нулём.
  const auto colour = read_field(*device_pixels, "colour");
  size_t opaque = 0;
  bool varied = false;
  for (const auto value : colour) {
    opaque += size_t((value >> 24) == 255u);
    varied = varied || value != colour[0];
  }
  check(opaque == pixels_count, "у собранной текстуры непрозрачный альфа-канал");
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
