#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <glm/geometric.hpp>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_resource.h"
#include "devils_engine/originator/pipeline.h"
#include "devils_engine/originator/primitives.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/script_program.h"
#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/kd_tree.h"

#include "names.h"
#include "package.h"
#include "planet_tools.h"
#include "viewer.h"
#include "visual.h"

// GN02 — генератор планеты. Вторая площадка кампании генератора и первый её ПОТРЕБИТЕЛЬ: GN01
// отвечала на вопрос «как устроен генератор», GN02 отвечает на вопрос «что из этого получается мир».
//
// Площадка headless. Планета здесь — данные: клетки на замкнутой поверхности, плиты, рельеф, климат,
// провинции, морские зоны, культуры и короткая история. Итог генерации уходит в ПАКЕТ на диск, и
// именно пакет, а не картинка, является результатом: генератор одноразовый, играют уже с него.
//
// Что проверяется числами вместо глаз: воспроизводимость (то же зерно => тот же отпечаток),
// независимость от числа потоков, свойства топологии (симметричное соседство, связность), и
// свойства мира, которые обязаны держаться при любом зерне — доля суши, связность провинций,
// покрытие суши провинциями, лето не холоднее зимы.

namespace {

namespace fs = std::filesystem;
using namespace devils_engine;

struct options {
  // Разрешение поверхности по умолчанию. 262144 клетки — это 44 км на клетку, и это НИЖНЯЯ граница
  // для мира, а не выбор ради красоты: острова здесь физического происхождения, и постройка горячей
  // точки поперёк в две сотни километров должна занимать хотя бы несколько клеток. На 65536 (88 км)
  // она занимает две, на 16384 (176 км) — не существует вовсе. Миллион клеток тоже работает: 19 с и
  // 370 MB, что для одноразового генератора приемлемо.
  size_t cells = 262144;
  size_t plates = 24;
  // Провинций больше, чем клеток на них уходит: число провинций — решение ИГРЫ, а не решётки, и от
  // разрешения зависеть не должно. При 262144 клетках и 40% суши на провинцию приходится около
  // тридцати клеток, то есть форму области видно.
  size_t provinces = 3200;
  // Морских областей ВДВОЕ МЕНЬШЕ, чем было: акватория — самое крупное названное место после океана,
  // и при 320 запрошенных выходило 640 фактических, то есть область размером с небольшое море. Имя у
  // такой области звучит натянуто, а ходить через неё — одно движение. 180 запрошенных дают около
  // 380 фактических: превышение здесь штатное, каждый замкнутый водоём получает свою область добором.
  size_t sea_zones = 180;
  size_t cultures = 64;
  // Два порога географии. Живут в options, а не читаются из пайплайна, потому что у СОБРАННОГО
  // пайплайна значений конфига уже не спросить: они разошлись по параметрам шагов. Оба нужны только
  // синтезу названий — выбрать «остров» или «море» вместо безымянного массива и океана.
  size_t continent_min_provinces = 40;
  size_t ocean_zones = 36;
  size_t threads = 0; // 0 => по числу ядер
  uint64_t seed = 20260901;
  fs::path dump;
  std::string stats;
  bool verify = false;
  bool report = true;
  bool map = false;
  bool quiet = false;

  // Просмотрщик. Площадка остаётся headless по сути: окно — это ещё один способ задать вопрос тем же
  // данным, а не второй режим работы генератора.
  bool view = false;
  gn02::viewer_options viewer{};
  // Значения, заданные из командной строки поверх конфига. Тот же механизм, что крутит настройки в
  // окне: правка числа мира не должна требовать правки файла.
  std::vector<std::pair<std::string, double>> overrides;
};

fs::path resource_root() {
  return fs::path(GN02_RESOURCE_ROOT);
}

// Генератор приезжает через demiurg — тем же путём, каким приехал бы из мода игрока, а не чтением
// файлов по именам, известным площадке. Хост знает ровно ОДНО имя, `generator/planet`; всё остальное
// (значения, буферы, тела шагов, программы) называет сама точка входа.
//
// Модуль здесь один и лежит папкой, но это ничего не меняет: тот же набор в zip'е и тот же набор,
// переопределённый модом, дают ту же точку входа с тем же id.
struct generator_registry {
  demiurg::module_system modules;
  demiurg::resource_system resources;
  originator::generator_config config;

  generator_registry() : modules(resource_root().generic_string() + "/") {
    modules.load_modules({demiurg::module_system::list_entry{"gn02/", "", ""}});
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
    config = originator::load_generator(resources, "generator/planet");
  }
};

// Один реестр на процесс: описание перечитывается на каждую перегенерацию (значения меняются из окна
// и из командной строки), а вот файлы читать заново незачем — они те же.
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
    } else if (argument == "--quiet") {
      result.quiet = true;
      result.report = false;
    } else if (argument == "--map") {
      result.map = true;
    } else if (argument == "--view") {
      result.view = true;
    } else if (argument == "--validation") {
      result.viewer.validation = true;
    } else if (argument == "--uncapped") {
      result.viewer.uncapped = true;
    } else if (starts_with(argument, "--width=")) {
      result.viewer.width = uint32_t(std::stoul(std::string(argument.substr(8))));
    } else if (starts_with(argument, "--height=")) {
      result.viewer.height = uint32_t(std::stoul(std::string(argument.substr(9))));
    } else if (starts_with(argument, "--frames=")) {
      // Число кадров ЗАМОРАЖИВАЕТ и вращение, и это не побочный эффект, а требование: кадр с
      // ограниченным числом кадров снимается ради сравнения снимков, а у вращающейся планеты два
      // прогона дают разные картинки, и любое сравнение превращается в угадывание.
      result.viewer.frames = uint32_t(std::stoul(std::string(argument.substr(9))));
      result.viewer.fixed_rotation = true;
    } else if (starts_with(argument, "--distance=")) {
      // Расстояние камеры ключом, а не только колесом: без него снимок с приближения не повторить, а
      // именно на приближении и видно, показывает ли карта мир или свою сетку.
      result.viewer.camera_distance = std::stof(std::string(argument.substr(11)));
    } else if (starts_with(argument, "--smoothing=")) {
      // `stof`, а не `atof`: у остальных ключей разбор громкий, а `atof` на мусоре молча даёт нуль —
      // то есть «самую угловатую границу», и понять, что ключ не понят, было бы нельзя.
      result.viewer.border_smoothing = std::stof(std::string(argument.substr(12)));
    } else if (starts_with(argument, "--mesh=")) {
      result.viewer.subdivisions = uint32_t(std::stoul(std::string(argument.substr(7))));
    } else if (starts_with(argument, "--mode=")) {
      result.viewer.mode = size_t(std::stoul(std::string(argument.substr(7))));
    } else if (starts_with(argument, "--step=")) {
      result.viewer.step_limit = size_t(std::stoul(std::string(argument.substr(7))));
    } else if (starts_with(argument, "--frame-dump=")) {
      result.viewer.dump_path = std::string(argument.substr(13));
    } else if (starts_with(argument, "--cells=")) {
      result.cells = size_t(std::stoull(std::string(argument.substr(8))));
    } else if (starts_with(argument, "--plates=")) {
      result.plates = size_t(std::stoull(std::string(argument.substr(9))));
    } else if (starts_with(argument, "--provinces=")) {
      result.provinces = size_t(std::stoull(std::string(argument.substr(12))));
    } else if (starts_with(argument, "--sea-zones=")) {
      result.sea_zones = size_t(std::stoull(std::string(argument.substr(12))));
    } else if (starts_with(argument, "--cultures=")) {
      result.cultures = size_t(std::stoull(std::string(argument.substr(11))));
    } else if (starts_with(argument, "--threads=")) {
      result.threads = size_t(std::stoull(std::string(argument.substr(10))));
    } else if (starts_with(argument, "--seed=")) {
      result.seed = std::stoull(std::string(argument.substr(7)));
    } else if (starts_with(argument, "--stats=")) {
      result.stats = std::string(argument.substr(8));
    } else if (starts_with(argument, "--set")) {
      // Обе формы: `--set name=value` и `--set=name=value`. Первая набирается руками чаще, вторая
      // удобнее в скриптах, и отказывать одной из них незачем.
      std::string_view body = argument.substr(5);
      if (body.empty()) {
        if (i + 1 >= argc) {
          utils::error{}("GN02: --set needs name=value");
        }
        body = argv[++i];
      } else if (body.front() == '=') {
        body = body.substr(1);
      }

      const auto split = body.find('=');
      if (split == std::string_view::npos) {
        utils::error{}("GN02: --set expects name=value, got '{}'", body);
      }
      result.overrides.emplace_back(std::string(body.substr(0, split)),
                                    std::stod(std::string(body.substr(split + 1))));
    } else if (starts_with(argument, "--dump=")) {
      result.dump = fs::path(std::string(argument.substr(7)));
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "GN02 planet generator lab\n"
                << "  --cells=N       planet cells (default 65536)\n"
                << "  --plates=N      tectonic plates (24)\n"
                << "  --provinces=N   land provinces (900)\n"
                << "  --sea-zones=N   sea zones (90)\n"
                << "  --cultures=N    cultures (48)\n"
                << "  --seed=N        world seed\n"
                << "  --threads=N     worker threads (0 = one per core)\n"
                << "  --dump=PATH     write the planet package\n"
                << "  --map           print the ASCII climate map\n"
                << "  --stats=A,B     print min/max/mean of cells fields\n"
                << "  --view          open the planet viewer\n"
                << "  --step=N        show the world after step N (0 = all)\n"
                << "  --mode=N        field to display (0 = climate)\n"
                << "  --mesh=N        icosphere subdivisions (0 = pick from cell count)\n"
                << "  --distance=R    camera distance in planet radii (default 2.6)\n"
                << "  --uncapped      draw without the 60 FPS limit (for measuring)\n"
                << "  --width/--height/--frames/--frame-dump/--validation - window and frame dump\n"
                << "  --smoothing=W   border smoothing in lattice steps (0.70 angular .. 1.24 smooth)\n"
                << "  --verify        run the contract checks\n"
                << "  --set NAME=VALUE  override a value from values.tavl (repeatable)\n"
                << "  --quiet         no report\n";
      std::exit(0);
    } else {
      utils::error{}("GN02: unknown argument '{}'", argument);
    }
  }
  return result;
}

originator::size_table make_sizes(const options& opts) {
  originator::size_table sizes;
  sizes.set("cell_count", opts.cells);
  sizes.set("cell_count_plus_one", opts.cells + 1);
  // Ёмкость под дуги соседства. Симметризованный граф K ближайших даёт в среднем чуть больше 2K
  // дуг на клетку у краевых случаев и ровно 2K в среднем; запас берётся кратным, а нехватка падает
  // громко с нужным числом в сообщении.
  sizes.set("arc_capacity", opts.cells * 10);

  // Мантийные плюмы: их число объявлено в values.tavl, а не ключом командной строки, потому что это
  // свойство ПЛАНЕТЫ, а не запрос игры — сколько провинций нарезать, решает игра, сколько горячих
  // точек в мантии, решает мир. Запас вдвое, как у плит.
  sizes.set("hotspot_capacity", 128);
  // Виды областей рельефа: их одиннадцать, но запас берётся с головой — таблица крошечная, а
  // добавление вида не должно требовать правки C++.
  sizes.set("landform_capacity", 32);
  sizes.set("landform_capacity_plus_one", 33);
  sizes.set("plate_capacity", opts.plates * 2);
  sizes.set("plate_capacity_plus_one", opts.plates * 2 + 1);
  // Ёмкость под области берётся с БОЛЬШИМ запасом, и запас этот не перестраховка. Число областей
  // получается больше запрошенного по трём причинам сразу: пуассоновский выбор считает расстояние по
  // площади, а не по счёту; каждый остров получает свою область добором; и верхняя граница размера
  // делит крупные провинции. Измерено: при цели 900 выходит 1235, при цели 220 — больше 440, на чём
  // старый запас в два раза и сломался (громко, на группировке по ключу — но сломался).
  sizes.set("province_capacity", opts.provinces * 4 + 1024);
  sizes.set("province_capacity_plus_one", opts.provinces * 4 + 1025);
  // Ёмкость морских зон зависит и от РАЗРЕШЕНИЯ, а не только от запрошенного числа зон, и это
  // исправление настоящего обрыва. Каждый замкнутый водоём получает свою зону добором, а число
  // замкнутых водоёмов растёт с разрешением: на мелкой решётке разрешаются пруды, которых на грубой
  // просто нет. Затопленные нагорья добавили их ещё около двух сотен. Измерено: при 180 запрошенных
  // зонах и миллионе клеток метка дошла до 1302 при объявленных 1232 корзинах — падение громкое, с
  // нужным числом в сообщении, но случаться на поддерживаемом разрешении оно не должно.
  const size_t sea_capacity = opts.sea_zones * 4 + 512 + opts.cells / 512;
  sizes.set("sea_capacity", sea_capacity);
  sizes.set("sea_capacity_plus_one", sea_capacity + 1);
  // Раскладка по ПОЛЮ ЗАТРАВКИ обслуживает и провинции, и морские зоны по очереди, поэтому ёмкость у
  // неё общая — по большей из двух.
  sizes.set("seed_capacity_plus_one", std::max<size_t>(opts.provinces * 4 + 1024, sea_capacity) + 1);
  // Дуги графа соседства ОБЛАСТЕЙ. У провинции в среднем шесть соседей, симметризация даёт вдвое;
  // запас берётся кратным, а нехватка падает громко с нужным числом в сообщении.
  sizes.set("province_arc_capacity", (opts.provinces * 4 + 1024) * 16);
  sizes.set("sea_arc_capacity", sea_capacity * 16);
  // Уровни географической иерархии. Ёмкость с запасом: у каждого уровня число областей выводится из
  // числа провинций, а добор непокрытых кусков (остров, отрезанный водой) добавляет свои.
  sizes.set("land_mass_capacity", 1024);
  sizes.set("continent_capacity", 1024);
  sizes.set("historical_capacity", opts.provinces + 512);
  sizes.set("ocean_capacity", 256);
  // Титулы де-юре и державы де-факто. Ёмкости выведены из числа провинций теми же соотношениями,
  // какими заданы сами уровни, и умножены с запасом: добор непокрытых кусков добавляет свои области
  // сверх расчёта, и обрыв по ёмкости обязан быть невозможен, а не маловероятен.
  sizes.set("duchy_capacity", opts.provinces + 512);
  sizes.set("empire_capacity", opts.provinces / 4 + 256);
  sizes.set("realm_capacity", opts.provinces + 512);
  sizes.set("barony_capacity", opts.provinces * 12 + 1024);
  sizes.set("culture_capacity", opts.cultures * 3);
  sizes.set("culture_capacity_plus_one", opts.cultures * 3 + 1);
  sizes.set("history_capacity", 4096);
  sizes.set("single", 1);
  return sizes;
}

originator::pipeline_description load_description(const options& opts) {
  originator::pipeline_description description = generator().config.description;

  // Значения, известные только из командной строки. Всё остальное живёт в конфиге: числа мира не
  // должны быть спрятаны в C++, иначе автор мира не сможет его настроить.
  description.values.set_number("cell_count", double(opts.cells));
  description.values.set_number("plate_count", double(opts.plates));
  description.values.set_number("province_count", double(opts.provinces));
  description.values.set_number("sea_zone_count", double(opts.sea_zones));
  description.values.set_number("culture_count", double(opts.cultures));

  // Правки из командной строки идут ПОСЛЕДНИМИ: они перекрывают и конфиг, и подставленные числа,
  // потому что человек, который их написал, знает больше обоих.
  for (const auto& [name, value] : opts.overrides) {
    if (!description.values.has(name)) {
      utils::error{}("GN02: --set names '{}', but the generator has no such value", name);
    }
    description.values.set_number(name, value);
  }
  return description;
}

void load_bodies(originator::script_host& host, const originator::pipeline_description& description) {
  const auto& package = generator().config;
  for (const auto& step : description.steps) {
    if (step.body.empty()) {
      utils::error{}("GN02: step '{}' has no body", step.name);
    }
    // Имя чанка для lua — это demiurg-id тела: в сообщении об ошибке скрипта стоит ровно тот адрес,
    // по которому этот скрипт лежит в модуле.
    host.load_body(step.name, package.source(step.body), step.body);
    for (const auto& [name, id] : step.programs) {
      host.load_program(name, package.source(id));
    }
  }
}

std::vector<std::string> split_words(const std::string_view& text) {
  std::vector<std::string> result;
  size_t position = 0;
  while (position < text.size()) {
    while (position < text.size() && (text[position] == ' ' || text[position] == ',' || text[position] == '\n')) {
      ++position;
    }
    const size_t start = position;
    while (position < text.size() && text[position] != ' ' && text[position] != ',' && text[position] != '\n') {
      ++position;
    }
    if (position > start) {
      result.emplace_back(text.substr(start, position - start));
    }
  }
  return result;
}

std::vector<std::byte> snapshot(const originator::buffer& source) {
  std::vector<std::byte> bytes(source.byte_size());
  std::memcpy(bytes.data(), source.base_pointer(), bytes.size());
  return bytes;
}

std::vector<std::vector<std::byte>> snapshot_all(originator::pipeline& source) {
  std::vector<std::vector<std::byte>> result;
  result.reserve(source.buffer_count());
  for (size_t i = 0; i < source.buffer_count(); ++i) {
    result.push_back(snapshot(source.buffer_at(i)));
  }
  return result;
}

// Готовый мир: пайплайн плюс всё, что нужно, чтобы его расспросить.
struct world {
  std::unique_ptr<originator::pipeline> line;
  double milliseconds = 0.0;
  // Время по шагам. Генератор — не кадр: у него нет бюджета в миллисекундах, зато есть вопрос «где
  // время», и ответ на него меняет решения. Здесь он видно сразу: у планеты время уходит не в
  // поэлементную арифметику, а в последовательные заливки.
  std::vector<std::pair<std::string, double>> steps;
};

// step_limit == 0 означает «все шаги». Ограничение существует ради просмотра по шагам: мир,
// посчитанный до шага N, — это тот же мир, у которого следующие поля ещё нулевые.
world generate(const options& opts, const originator::tool_registry& tools,
               const originator::pipeline_description& description, thread::atomic_pool* pool,
               const size_t step_limit = 0) {
  originator::script_host host(const_cast<originator::tool_registry&>(tools), pool);
  load_bodies(host, description);

  world result;
  result.line = std::make_unique<originator::pipeline>(description, make_sizes(opts), opts.seed);

  // Шаги исполняются по одному, чтобы каждый был измерен отдельно. Порядок и результат от этого не
  // меняются: run() делает ровно то же самое, просто без замера.
  const auto invoker = host.invoker();
  const size_t steps = step_limit == 0 ? result.line->step_count()
                                       : std::min(step_limit, result.line->step_count());
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < steps; ++i) {
    const auto step_start = std::chrono::steady_clock::now();
    result.line->run_step(i, invoker);
    const auto step_stop = std::chrono::steady_clock::now();
    result.steps.emplace_back(result.line->step_at(i).name,
                              std::chrono::duration<double, std::milli>(step_stop - step_start).count());
  }
  const auto stop = std::chrono::steady_clock::now();
  result.milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
  return result;
}

double field_value(originator::pipeline& line, const std::string_view& buffer_name,
                   const std::string_view& field_name, const size_t element, const uint32_t component = 0) {
  auto* buffer = line.find_buffer(buffer_name);
  if (buffer == nullptr) {
    utils::error{}("GN02: no buffer '{}'", buffer_name);
  }
  const size_t index = buffer->find_field(field_name);
  if (index == originator::buffer_layout::npos) {
    utils::error{}("GN02: buffer '{}' has no field '{}'", buffer_name, field_name);
  }
  return buffer->field(index).get(element, component);
}

// Аксессор поля по именам: удобно для проверок, где важна ясность, а не скорость.
originator::const_field_accessor field_of(originator::pipeline& line, const std::string_view& buffer_name,
                                          const std::string_view& field_name) {
  auto* buffer = line.find_buffer(buffer_name);
  if (buffer == nullptr) {
    utils::error{}("GN02: no buffer '{}'", buffer_name);
  }
  const size_t index = buffer->find_field(field_name);
  if (index == originator::buffer_layout::npos) {
    utils::error{}("GN02: buffer '{}' has no field '{}'", buffer_name, field_name);
  }
  return buffer->field(index);
}

struct climate_names {
  const char* name;
  char symbol;
};

// Порядок совпадает с идентификаторами в scripts/S06_climate_zone.ds: там правило, здесь только имена.
// Имена латиницей: весь вывод программы — и отчёт, и оверлей — читается одним шрифтом и одной
// раскладкой, а кириллица в атласе шрифта площадок отсутствует вовсе.
constexpr climate_names climate_table[] = {
  {"ocean", '~'},      {"sea ice", '*'},   {"ice cap", '#'},  {"tundra", '-'},
  {"taiga", 'T'},      {"forest", 'F'},    {"steppe", 's'},   {"desert", '.'},
  {"savanna", 'v'},    {"jungle", 'J'},    {"alpine", '^'},
};

constexpr size_t climate_count = sizeof(climate_table) / sizeof(climate_table[0]);
// Одно множество классов перечислено ТРИЖДЫ: правило в `S06_climate_zone.ds`, имена здесь, палитра в
// `visual.cpp`. Связать первое с остальными может только проверка (правило — данные), а вот имена и
// палитру связывает уже компилятор.
static_assert(climate_count == gn02::climate_class_count);

// Виды областей рельефа. Порядок совпадает с идентификаторами в scripts/S04_landform_water.ds и
// scripts/S04_landform_land.ds и с порядком имён в теле шага landforms: правило там, имена здесь, числа
// усиления в values.tavl.
constexpr const char* landform_names[] = {
  "abyssal plain", "ocean ridge", "shelf", "archipelago", "trench",
  "coastal plain", "lowland", "plateau", "hills", "mountains", "volcanic island",
};

constexpr size_t landform_count = sizeof(landform_names) / sizeof(landform_names[0]);
static_assert(landform_count == gn02::landform_class_count);

void print_step_times(const world& value) {
  std::cout << "  step timings:\n";
  for (const auto& [name, milliseconds] : value.steps) {
    const double share = value.milliseconds <= 0.0 ? 0.0 : 100.0 * milliseconds / value.milliseconds;
    std::cout << "    " << std::format("{:<12}", name) << std::format("{:>9.1f}", milliseconds) << " ms  "
              << std::format("{:>5.1f}", share) << "%\n";
  }
}

// Связные куски суши. Считаются по тому же графу соседства, что и всё остальное, и нужны не для
// красоты: остров, в который не влезает провинция, для игры не существует, а один сверхматерик на
// пол-планеты означает, что остальная половина — пустая вода.
struct land_masses {
  size_t components = 0;
  size_t largest = 0;
  size_t playable = 0; // кусков, вмещающих хотя бы одну провинцию
  size_t specks = 0;   // кусков меньше трёх клеток
  // Океанические острова и их ФОРМА. Форма здесь не украшение: жалоба на генератор звучала как
  // «дуга выходит длинной ровной полосой суши», а полоса и цепь островов при одинаковой площади
  // различаются ровно двумя числами — сколько отдельных кусков и насколько каждый вытянут.
  size_t oceanic = 0;        // кусков на океанической коре
  size_t oceanic_cells = 0;  // их суммарная площадь в клетках
  size_t strips = 0;         // из них вытянутых сильнее чем вчетверо
  double median_elongation = 0.0;
  double median_oceanic_size = 0.0;
  // СКОПЛЕНИЯ островов на континентальной коре — затопленные нагорья вроде Эгейского моря. Мерятся
  // отдельно от океанических, потому что и происхождение у них другое, и главный вопрос другой: у
  // цепи спрашивают, разбита ли она на куски, а у скопления — сгруппированы ли куски. Поэтому здесь
  // считается число СОСЕДЕЙ в пределах небольшого угла: у одинокого острова их нет, у архипелага
  // много, и одно это число отличает россыпь от скопления.
  size_t shelf_islands = 0;
  size_t shelf_cells = 0;
  double median_shelf_size = 0.0;
  double median_neighbours = 0.0;
  size_t clustered = 0; // островов, у которых не меньше трёх соседей рядом
};

// Вытянутость куска: корень из отношения двух первых собственных значений матрицы разброса его
// клеток. У круглого острова единица, у полосы длиной L и шириной w — примерно L/w. Считается по
// положениям на сфере, поэтому третье собственное значение (толщина сферического слоя) отбрасывается
// само собой: оно на порядки меньше двух первых.
double elongation_of(const std::vector<std::array<double, 3>>& points) {
  if (points.size() < 4) {
    return 1.0;
  }
  std::array<double, 3> mean{0.0, 0.0, 0.0};
  for (const auto& p : points) {
    for (size_t k = 0; k < 3; ++k) {
      mean[k] += p[k];
    }
  }
  for (size_t k = 0; k < 3; ++k) {
    mean[k] /= double(points.size());
  }

  double covariance[3][3] = {};
  for (const auto& p : points) {
    const std::array<double, 3> d{p[0] - mean[0], p[1] - mean[1], p[2] - mean[2]};
    for (size_t i = 0; i < 3; ++i) {
      for (size_t j = 0; j < 3; ++j) {
        covariance[i][j] += d[i] * d[j];
      }
    }
  }
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      covariance[i][j] /= double(points.size());
    }
  }

  // Собственные значения симметричной матрицы 3x3 по замкнутой формуле: степенной метод здесь не
  // нужен, а библиотечная зависимость ради трёх чисел тем более.
  const double p1 = covariance[0][1] * covariance[0][1] + covariance[0][2] * covariance[0][2] +
                    covariance[1][2] * covariance[1][2];
  const double trace = covariance[0][0] + covariance[1][1] + covariance[2][2];
  if (p1 <= 0.0) {
    std::array<double, 3> diagonal{covariance[0][0], covariance[1][1], covariance[2][2]};
    std::sort(diagonal.begin(), diagonal.end(), std::greater<double>());
    return diagonal[1] <= 0.0 ? 1.0 : std::sqrt(diagonal[0] / diagonal[1]);
  }
  const double q = trace / 3.0;
  const double p2 = (covariance[0][0] - q) * (covariance[0][0] - q) + (covariance[1][1] - q) * (covariance[1][1] - q) +
                    (covariance[2][2] - q) * (covariance[2][2] - q) + 2.0 * p1;
  const double p = std::sqrt(p2 / 6.0);
  double b[3][3];
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) {
      b[i][j] = (covariance[i][j] - (i == j ? q : 0.0)) / p;
    }
  }
  const double determinant = b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) -
                             b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) +
                             b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0]);
  const double phi = std::acos(std::clamp(determinant / 2.0, -1.0, 1.0)) / 3.0;
  const double first = q + 2.0 * p * std::cos(phi);
  const double third = q + 2.0 * p * std::cos(phi + 2.0 * std::numbers::pi / 3.0);
  const double second = trace - first - third;
  std::array<double, 3> values{first, second, third};
  std::sort(values.begin(), values.end(), std::greater<double>());
  return values[1] <= 0.0 ? 1.0 : std::sqrt(values[0] / values[1]);
}

land_masses measure_land_masses(originator::pipeline& line, const size_t count, const size_t province_cells) {
  const auto land = field_of(line, "cells", "land");
  const auto offsets = field_of(line, "cell_offsets", "start");
  const auto arcs = field_of(line, "cell_arcs", "cell");
  const auto crust = field_of(line, "cells", "crust");
  const auto positions = field_of(line, "cells", "position");

  std::vector<double> elongations;
  std::vector<double> oceanic_sizes;
  std::vector<double> shelf_sizes;
  std::vector<std::array<double, 3>> shelf_centres;
  std::vector<std::array<double, 3>> points;

  // Порог «мелкого» острова — доля от всей суши, а не число клеток: иначе он зависел бы от
  // разрешения. Радиус скопления — в радианах, как все расстояния конфига.
  const auto shelf_island_limit = std::max<size_t>(size_t(0.004 * double(count)), 4);
    // Радиус скопления — размер настоящего архипелага, а не радиус «рядом»: Эгейское море это примерно
  // 500 на 600 километров, то есть 0.12 радиана по земному радиусу. На меньшем радиусе счёт соседей
  // говорил бы не о том, скопление ли это, а о том, помещаются ли два острова в одну клетку решётки.
  constexpr double cluster_radius = 0.12;

  land_masses result;
  std::vector<uint8_t> seen(count, 0);
  std::vector<uint32_t> stack;
  for (size_t start = 0; start < count; ++start) {
    if (seen[start] != 0 || land.get(start) == 0.0) {
      continue;
    }
    stack.clear();
    stack.push_back(uint32_t(start));
    seen[start] = 1;
    size_t size = 0;
    double crust_sum = 0.0;
    points.clear();
    while (!stack.empty()) {
      const auto cell = stack.back();
      stack.pop_back();
      ++size;
      crust_sum += crust.get(cell);
      points.push_back({positions.get(cell, 0), positions.get(cell, 1), positions.get(cell, 2)});
      const auto first = size_t(offsets.get(cell));
      const auto last = size_t(offsets.get(size_t(cell) + 1));
      for (size_t k = first; k < last; ++k) {
        const auto other = uint32_t(arcs.get(k));
        if (other < count && seen[other] == 0 && land.get(other) != 0.0) {
          seen[other] = 1;
          stack.push_back(other);
        }
      }
    }

    ++result.components;
    result.largest = std::max(result.largest, size);
    result.playable += size >= province_cells ? 1 : 0;
    result.specks += size < 3 ? 1 : 0;

    // Океанический остров — тот, что стоит на океанической коре, а не осколок материка. Порог по
    // средней континентальности куска: у дуги и горячей точки она близка к нулю, у обломка материка
    // близка к единице.
    const double mean_crust = size == 0 ? 1.0 : crust_sum / double(size);
    // Островом СКОПЛЕНИЯ считается мелкий кусок на континентальной коре: крупные куски — это сами
    // материки, и группировать их незачем.
    if (mean_crust >= 0.35 && size >= 2 && size <= shelf_island_limit) {
      ++result.shelf_islands;
      result.shelf_cells += size;
      shelf_sizes.push_back(double(size));
      std::array<double, 3> centre{0.0, 0.0, 0.0};
      for (const auto& point : points) {
        for (size_t k = 0; k < 3; ++k) {
          centre[k] += point[k];
        }
      }
      const double length = std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
      if (length > 0.0) {
        for (auto& value : centre) {
          value /= length;
        }
        shelf_centres.push_back(centre);
      }
    }
    if (mean_crust < 0.35 && size >= 4) {
      ++result.oceanic;
      result.oceanic_cells += size;
      oceanic_sizes.push_back(double(size));
      const double elongation = elongation_of(points);
      elongations.push_back(elongation);
      result.strips += elongation > 4.0 ? 1 : 0;
    }
  }

  // Медиана, а не среднее: один сверхдлинный хребет сдвинул бы среднее так, что по нему нельзя было
  // бы судить о типичном острове.
  const auto median = [](std::vector<double>& values) {
    if (values.empty()) {
      return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  };
  result.median_elongation = median(elongations);
  result.median_oceanic_size = median(oceanic_sizes);

  // Соседи считаются по УГЛУ между центрами островов, а не по расстоянию в графе: острова скопления
  // разделены водой, и по графу суши они друг другу не соседи вовсе.
  const double cosine_limit = std::cos(cluster_radius);
  std::vector<double> neighbours;
  neighbours.reserve(shelf_centres.size());
  for (size_t i = 0; i < shelf_centres.size(); ++i) {
    size_t near = 0;
    for (size_t j = 0; j < shelf_centres.size(); ++j) {
      if (i == j) {
        continue;
      }
      const double dot = shelf_centres[i][0] * shelf_centres[j][0] + shelf_centres[i][1] * shelf_centres[j][1] +
                         shelf_centres[i][2] * shelf_centres[j][2];
      near += dot >= cosine_limit ? 1 : 0;
    }
    neighbours.push_back(double(near));
    result.clustered += near >= 3 ? 1 : 0;
  }
  result.median_shelf_size = median(shelf_sizes);
  result.median_neighbours = median(neighbours);
  return result;
}

// География: иерархия названных мест и её проверка на месте.
//
// Печатается не «сколько чего», а РАЗМЕР уровня в единицах уровня ниже: смысл исторической области в
// том, что она объединяет десяток-два провинций, и увидеть это можно только так. Плюс главное
// свойство иерархии — что родитель у всех членов уровня один, — здесь именно СЧИТАЕТСЯ, а не
// предполагается: если материк перелез в чужой массив, число в строке «split» станет ненулевым.
void print_geography(originator::pipeline& line, const double geography_continent_min,
                     const double geography_ocean_zones) {
  const auto land_masses = size_t(field_value(line, "state", "land_mass_count", 0));
  const auto continents = size_t(field_value(line, "state", "continent_count", 0));
  const auto historical = size_t(field_value(line, "state", "historical_count", 0));
  const auto oceans = size_t(field_value(line, "state", "ocean_count", 0));
  const auto province_count = size_t(field_value(line, "state", "province_count", 0));
  const auto sea_zone_count = size_t(field_value(line, "state", "sea_zone_count", 0));

  const auto province_land_mass = field_of(line, "provinces", "land_mass");
  const auto province_continent = field_of(line, "provinces", "continent");
  const auto province_region = field_of(line, "provinces", "historical_region");
  const auto province_cells = field_of(line, "provinces", "cells");

  // Разрывы иерархии: сколько областей уровня лежат больше чем в одном родителе.
  const auto count_split = [&](const originator::const_field_accessor& child,
                               const originator::const_field_accessor& parent, const size_t child_count) {
    std::vector<size_t> seen(child_count + 1, 0);
    size_t split = 0;
    for (size_t node = 1; node <= province_count && node < child.count(); ++node) {
      const auto index = size_t(child.get(node));
      if (index == 0 || index > child_count) {
        continue;
      }
      const auto owner = size_t(parent.get(node)) + 1;
      if (seen[index] == 0) {
        seen[index] = owner;
      } else if (seen[index] != owner) {
        seen[index] = owner;
        ++split;
      }
    }
    return split;
  };

  const auto mean_size = [](const size_t total, const size_t parts) {
    return parts == 0 ? 0.0 : double(total) / double(parts);
  };

  const auto water_bodies = size_t(field_value(line, "state", "water_body_count", 0));
  const auto lakes = size_t(field_value(line, "state", "lake_count", 0));
  const auto attached = size_t(field_value(line, "state", "attached_provinces", 0));

  std::cout << "  geography           " << land_masses << " land masses -> " << continents
            << " continents -> " << historical << " historical regions -> " << province_count
            << " provinces\n"
            << "  ocean hierarchy     " << water_bodies << " water bodies -> " << oceans << " oceans -> "
            << sea_zone_count << " oceanic regions, of which " << lakes << " bodies are lakes\n"
            << "  level size          "
            << std::format("{:.1f}", mean_size(province_count, land_masses)) << " provinces per land mass, "
            << std::format("{:.1f}", mean_size(province_count, continents)) << " per continent, "
            << std::format("{:.1f}", mean_size(province_count, historical)) << " per historical region, "
            << std::format("{:.1f}", mean_size(sea_zone_count, oceans)) << " regions per ocean\n"
            // Единственный настоящий инвариант иерархии: область лежит в ОДНОМ материке. Материк
            // при этом лежать в одном массиве НЕ обязан — мелкие острова прикрепляются к ближайшему
            // материку намеренно, и число прикреплённых провинций стоит рядом, чтобы уступка была
            // видна, а не спрятана.
            << "  hierarchy           " << count_split(province_region, province_continent, historical)
            << " regions split across continents, " << attached
            << " provinces attached to a continent across water\n";

  // Крупнейшие области каждого уровня: по ним видно, не съел ли уровень свой родитель целиком.
  const auto largest_of = [&](const originator::const_field_accessor& child, const size_t child_count) {
    std::vector<size_t> sizes(child_count + 1, 0);
    for (size_t node = 1; node <= province_count && node < child.count(); ++node) {
      const auto index = size_t(child.get(node));
      if (index != 0 && index <= child_count) {
        sizes[index] += size_t(province_cells.get(node));
      }
    }
    size_t total = 0;
    size_t largest = 0;
    for (size_t i = 1; i <= child_count; ++i) {
      total += sizes[i];
      largest = std::max(largest, sizes[i]);
    }
    return total == 0 ? 0.0 : 100.0 * double(largest) / double(total);
  };

  // Сам граф соседства областей: без этой строки «уровень не вырос» не отличить от «графа нет».
  const auto graph_shape = [&](const std::string_view offsets_name, const size_t node_count) {
    const auto starts = field_of(line, offsets_name, "start");
    size_t arcs_total = 0;
    size_t isolated = 0;
    for (size_t node = 1; node <= node_count && node + 1 < starts.count(); ++node) {
      const auto degree = size_t(starts.get(node + 1)) - size_t(starts.get(node));
      arcs_total += degree;
      isolated += degree == 0 ? 1 : 0;
    }
    return std::make_pair(node_count == 0 ? 0.0 : double(arcs_total) / double(node_count), isolated);
  };
  // Тот же граф, посчитанный НЕЗАВИСИМО от инструмента: если числа расходятся, виноват инструмент, а
  // не данные. Проверка дешёвая — один проход по клеткам, — а ловит она то, что иначе выглядит как
  // «уровень почему-то не вырос».
  const auto graph_by_hand = [&](const std::string_view label_name, const size_t node_count) {
    const auto labels = field_of(line, "cells", label_name);
    const auto offsets = field_of(line, "cell_offsets", "start");
    const auto arcs = field_of(line, "cell_arcs", "cell");
    const auto cells = labels.count();

    std::vector<std::vector<uint32_t>> neighbours(node_count + 1);
    for (size_t i = 0; i < cells; ++i) {
      const auto own = size_t(labels.get(i));
      if (own == 0 || own > node_count) {
        continue;
      }
      const auto first = size_t(offsets.get(i));
      const auto last = size_t(offsets.get(i + 1));
      for (size_t k = first; k < last; ++k) {
        const auto other_cell = size_t(arcs.get(k));
        if (other_cell >= cells) {
          continue;
        }
        const auto other = size_t(labels.get(other_cell));
        if (other != 0 && other != own && other <= node_count) {
          neighbours[own].push_back(uint32_t(other));
        }
      }
    }
    size_t total = 0;
    for (auto& list : neighbours) {
      std::sort(list.begin(), list.end());
      list.erase(std::unique(list.begin(), list.end()), list.end());
      total += list.size();
    }
    return total;
  };

  const auto [province_degree, province_isolated] =
    graph_shape("province_neighbour_offsets", province_count);
  const auto [sea_degree, sea_isolated] = graph_shape("sea_neighbour_offsets", sea_zone_count);
  const auto province_arcs_by_hand = graph_by_hand("province", province_count);
  const auto sea_arcs_by_hand = graph_by_hand("sea_zone", sea_zone_count);
  std::cout << "  area graph check    provinces " << size_t(province_degree * double(province_count) + 0.5)
            << " arcs from the tool, " << province_arcs_by_hand << " by hand; sea zones "
            << size_t(sea_degree * double(sea_zone_count) + 0.5) << " against " << sea_arcs_by_hand << "\n";

  std::cout << "  area graphs         provinces " << std::format("{:.1f}", province_degree)
            << " neighbours (" << province_isolated << " isolated), sea zones "
            << std::format("{:.1f}", sea_degree) << " (" << sea_isolated << " isolated)\n";

  // ТИТУЛЫ ДЕ-ЮРЕ И ДЕРЖАВЫ ДЕ-ФАКТО. Печатаются рядом с географией, потому что первое из неё и
  // выведено: де-юре королевство — это историческая область, и отдельной строкой оно не считается.
  // У держав главное число не количество, а РАЗБРОС РАЗМЕРА: если державы вышли одинаковыми, значит
  // граница нарисована делением площади, а не силой, и голосование по населению не сработало.
  {
    const auto duchies = size_t(field_value(line, "state", "duchy_count", 0));
    const auto empires = size_t(field_value(line, "state", "empire_count", 0));
    const auto realm_count = size_t(field_value(line, "state", "realm_count", 0));
    const auto baronies = size_t(field_value(line, "state", "barony_count", 0));

    const auto realm_provinces = field_of(line, "realms", "provinces");
    const auto realm_native = field_of(line, "realms", "native_provinces");
    std::vector<double> held;
    size_t native_total = 0;
    size_t held_total = 0;
    for (size_t i = 1; i <= realm_count && i < realm_provinces.count(); ++i) {
      const auto size = size_t(realm_provinces.get(i));
      if (size == 0) {
        continue;
      }
      held.push_back(double(size));
      held_total += size;
      native_total += size_t(realm_native.get(i));
    }
    std::sort(held.begin(), held.end());

    std::cout << "  titles              " << empires << " empires -> " << historical
              << " kingdoms (the historical regions) -> " << duchies << " duchies -> " << province_count
              << " counties -> " << baronies << " baronies\n"
              << "  title size          "
              << std::format("{:.1f}", mean_size(province_count, empires)) << " counties per empire, "
              << std::format("{:.1f}", mean_size(province_count, duchies)) << " per duchy, "
              << std::format("{:.1f}", mean_size(baronies, province_count)) << " baronies per county\n"
              << "  de facto realms     " << held.size() << " realms, median "
              << std::format("{:.0f}", held.empty() ? 0.0 : held[held.size() / 2]) << " counties, largest "
              << std::format("{:.0f}", held.empty() ? 0.0 : held.back()) << ", smallest "
              << std::format("{:.0f}", held.empty() ? 0.0 : held.front()) << ", "
              << std::format("{:.0f}", held_total == 0 ? 0.0 : 100.0 * double(native_total) / double(held_total))
              << "% of held counties are of the realm's own culture\n";
  }

  // ОБРАЗЕЦ ИЕРАРХИИ НАЗВАНИЯМИ. Числа выше говорят, что иерархия правильной формы; эта выборка
  // говорит, что она ОСМЫСЛЕННА — а это разные утверждения, и второе числами не проверяется. Берётся
  // крупнейший массив, его материки и области одного материка: смотреть надо на то, читается ли
  // «Северная <материк>» как название места.
  {
    const auto names = gn02::build_place_names(line, size_t(std::max(1.0, geography_continent_min)),
                                         size_t(std::max(1.0, geography_ocean_zones)));
    const auto continent_land_mass = field_of(line, "continents", "land_mass");
    const auto continent_cells = field_of(line, "continents", "cells");
    const auto region_continent_field = field_of(line, "historical_regions", "continent");
    const auto region_cells = field_of(line, "historical_regions", "cells");
    const auto mass_cells = field_of(line, "land_masses", "cells");
    const auto ocean_cells = field_of(line, "oceans", "cells");

    size_t biggest_mass = 0;
    double biggest_mass_cells = -1.0;
    for (size_t i = 1; i <= land_masses && i < mass_cells.count(); ++i) {
      if (mass_cells.get(i) > biggest_mass_cells) {
        biggest_mass_cells = mass_cells.get(i);
        biggest_mass = i;
      }
    }

    std::cout << "  sample hierarchy    " << (biggest_mass == 0 ? "-" : names.land_masses[biggest_mass])
              << " (" << std::format("{:.0f}", std::max(0.0, biggest_mass_cells)) << " cells)\n";

    size_t shown = 0;
    size_t biggest_continent = 0;
    double biggest_continent_cells = -1.0;
    for (size_t i = 1; i <= continents && i < continent_cells.count(); ++i) {
      if (size_t(continent_land_mass.get(i)) != biggest_mass) {
        continue;
      }
      if (continent_cells.get(i) > biggest_continent_cells) {
        biggest_continent_cells = continent_cells.get(i);
        biggest_continent = i;
      }
      if (shown < 6) {
        std::cout << "                        - " << names.continents[i] << " ("
                  << std::format("{:.0f}", continent_cells.get(i)) << " cells)\n";
        ++shown;
      }
    }

    shown = 0;
    for (size_t i = 1; i <= historical && i < region_cells.count() && shown < 8; ++i) {
      if (size_t(region_continent_field.get(i)) != biggest_continent) {
        continue;
      }
      std::cout << "                          . " << names.historical_regions[i] << " ("
                << std::format("{:.0f}", region_cells.get(i)) << " cells)\n";
      ++shown;
    }

    // Титулы того же материка и крупнейшие державы: по этим двум строкам видно, читается ли
    // политика как политика, а не как раскраска.
    const auto duchy_region = field_of(line, "duchies", "historical_region");
    const auto duchy_cells = field_of(line, "duchies", "cells");
    const auto empire_continent = field_of(line, "empires", "continent");
    shown = 0;
    for (size_t i = 1; i < names.empires.size() && shown < 3; ++i) {
      if (size_t(empire_continent.get(i)) != biggest_continent) {
        continue;
      }
      std::cout << "                        # Empire of " << names.empires[i] << "\n";
      ++shown;
    }
    shown = 0;
    for (size_t i = 1; i < names.duchies.size() && shown < 4; ++i) {
      const auto region = size_t(duchy_region.get(i));
      if (region == 0 || region >= names.historical_regions.size() ||
          size_t(field_of(line, "historical_regions", "continent").get(region)) != biggest_continent) {
        continue;
      }
      std::cout << "                        # Duchy of " << names.duchies[i] << " in the Kingdom of "
                << names.historical_regions[region] << " (" << std::format("{:.0f}", duchy_cells.get(i))
                << " cells)\n";
      ++shown;
    }

    const auto realm_size = field_of(line, "realms", "provinces");
    std::vector<std::pair<size_t, size_t>> ranked;
    for (size_t i = 1; i < names.realms.size() && i < realm_size.count(); ++i) {
      ranked.emplace_back(size_t(realm_size.get(i)), i);
    }
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    for (size_t k = 0; k < ranked.size() && k < 4; ++k) {
      std::cout << "                        * " << names.realms[ranked[k].second] << " ("
                << ranked[k].first << " counties)\n";
    }

    shown = 0;
    for (size_t i = 1; i <= oceans && i < ocean_cells.count() && shown < 5; ++i) {
      std::cout << "                        ~ " << names.oceans[i] << " ("
                << std::format("{:.0f}", ocean_cells.get(i)) << " cells)\n";
      ++shown;
    }
  }

  std::cout << "  largest             "
            << std::format("{:.1f}", largest_of(province_land_mass, land_masses)) << "% of land in one mass, "
            << std::format("{:.1f}", largest_of(province_continent, continents)) << "% in one continent, "
            << std::format("{:.1f}", largest_of(province_region, historical)) << "% in one region\n";
}

// Гипсография и изломанность: два числа, которыми меряется правдоподобие рельефа.
//
// Гипсографическая кривая — доля поверхности по высотным полосам. У планеты она ДВУГОРБАЯ: чаще
// всего встречаются абиссальная равнина и прибрежная суша, а континентальный склон между ними
// занимает считанные проценты. Ровный скат в этой таблице виден сразу — полосы склона перестают быть
// редкими, и это значит, что модель растянула самый редкий рельеф на всю ширину шельфа.
//
// Изломанность — средний перепад высоты между соседями. Одно число на всю планету бессмысленно,
// поэтому оно считается отдельно по суше, по мелководью и по глубокой воде: самая ровная поверхность
// планеты — абиссальная равнина, самая неровная — горный пояс, и отношение между ними и есть то, что
// «одинаковая деталь везде» ломает первым делом.
struct relief_band {
  const char* name;
  double lower;
  double upper;
};

constexpr relief_band relief_bands[] = {
  {"above 4000 m", 4000.0, 1e9},   {"2000..4000 m", 2000.0, 4000.0},
  {"1000..2000 m", 1000.0, 2000.0}, {"250..1000 m", 250.0, 1000.0},
  {"0..250 m", 0.0, 250.0},         {"shelf 0..-200", -200.0, 0.0},
  {"slope -200..-2000", -2000.0, -200.0}, {"-2000..-4000", -4000.0, -2000.0},
  {"-4000..-6000", -6000.0, -4000.0}, {"below -6000", -1e9, -6000.0},
};

// Типичный перепад высоты между соседями по трём поверхностям. МЕДИАНА, а не среднее, и это не
// придирка к статистике: жёлоб даёт между соседями три километра на одной дуге, и полтора процента
// таких дуг сдвигают среднее вдвое. Вопрос же стоит про типичную клетку — «насколько здесь неровно», —
// а на него отвечает медиана.
//
// Дуги, а не клетки: у клетки соседей около семи, и «перепад на клетку» зависел бы от степени
// вершины, то есть от решётки, а не от рельефа.
struct relief_steps {
  double land = 0.0;
  double shallow = 0.0;
  double plain = 0.0;
};

double median_of(std::vector<double>& values) {
  if (values.empty()) {
    return 0.0;
  }
  const size_t middle = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + middle, values.end());
  return values[middle];
}

relief_steps measure_relief_steps(originator::pipeline& line, const size_t count) {
  const auto height = field_of(line, "cells", "height");
  const auto land = field_of(line, "cells", "land");
  const auto offsets = field_of(line, "cell_offsets", "start");
  const auto arcs = field_of(line, "cell_arcs", "cell");
  // Абиссальная равнина опознаётся по ВОЗРАСТУ дна, а не только по глубине: глубина ловит заодно
  // фланг хребта, а он молодой и потому наклонный.
  const auto age = field_of(line, "cells", "age");
  const double sea_level = field_value(line, "state", "sea_level", 0);

  std::vector<double> land_steps;
  std::vector<double> shallow_steps;
  std::vector<double> plain_steps;

  for (size_t i = 0; i < count; ++i) {
    const double relative = height.get(i) - sea_level;
    const bool is_land = land.get(i) != 0.0;

    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs.get(k));
      if (other <= i || other >= count) {
        continue; // каждая пара соседей лежит в CSR дважды
      }
      const double step = std::abs(height.get(i) - height.get(other));
      const double other_relative = height.get(other) - sea_level;
      const bool other_land = land.get(other) != 0.0;
      if (is_land && other_land) {
        land_steps.push_back(step);
      } else if (!is_land && !other_land) {
        if (age.get(i) > 0.9 && age.get(other) > 0.9 && relative < -3000.0 && other_relative < -3000.0) {
          plain_steps.push_back(step);
        } else if (relative > -2000.0 && other_relative > -2000.0) {
          shallow_steps.push_back(step);
        }
      }
    }
  }

  return relief_steps{median_of(land_steps), median_of(shallow_steps), median_of(plain_steps)};
}

void print_relief_profile(originator::pipeline& line, const size_t count) {
  const auto height = field_of(line, "cells", "height");
  const auto land = field_of(line, "cells", "land");
  const double sea_level = field_value(line, "state", "sea_level", 0);

  std::array<size_t, sizeof(relief_bands) / sizeof(relief_bands[0])> bands{};
  double land_height_sum = 0.0;
  size_t land_cells = 0;

  for (size_t i = 0; i < count; ++i) {
    // Высоты меряются ОТ УРОВНЯ МОРЯ: сама отметка ищется бисекцией и от прогона к прогону разная,
    // поэтому абсолютные метры сравнивать между мирами нельзя, а глубины под водой — можно.
    const double relative = height.get(i) - sea_level;
    for (size_t band = 0; band < bands.size(); ++band) {
      if (relative >= relief_bands[band].lower && relative < relief_bands[band].upper) {
        bands[band] += 1;
        break;
      }
    }
    if (land.get(i) != 0.0) {
      land_height_sum += relative;
      ++land_cells;
    }
  }

  // Уклон, а не перепад между соседями: перепад зависит от шага решётки, и одна и та же планета в
  // грубом и мелком разрешении давала бы разные числа при одинаковом рельефе. Шаг решётки —
  // sqrt(4pi/N) радиан, сто километров — 0.0157 радиана.
  const auto steps = measure_relief_steps(line, count);
  const double spacing = std::sqrt(4.0 * std::numbers::pi / double(count));
  const double per_hundred_km = 0.0157 / spacing;
  std::cout << "  land elevation      mean "
            << std::format("{:.0f}", land_cells == 0 ? 0.0 : land_height_sum / double(land_cells))
            << " m above sea level\n"
            << "  typical slope       land " << std::format("{:.0f}", steps.land * per_hundred_km)
            << ", shallow water " << std::format("{:.0f}", steps.shallow * per_hundred_km)
            << ", abyssal plain " << std::format("{:.0f}", steps.plain * per_hundred_km)
            << " m per 100 km (median)\n"
            << "  hypsometry (share of the surface, from sea level):\n";
  for (size_t band = 0; band < bands.size(); ++band) {
    if (bands[band] == 0) {
      continue;
    }
    std::cout << "    " << std::format("{:<18}", relief_bands[band].name)
              << std::format("{:>7}", bands[band]) << " ("
              << std::format("{:.1f}", 100.0 * double(bands[band]) / double(count)) << "%)\n";
  }
}

void print_report(originator::pipeline& line, const options& opts, const double milliseconds) {
  const size_t count = opts.cells;

  std::cout << "GN02: " << count << " cells, seed " << opts.seed << ", " << std::format("{:.1f}", milliseconds)
            << " ms, memory " << std::format("{:.1f}", double(line.total_byte_size()) / (1024.0 * 1024.0)) << " MB\n";

  const auto land = field_of(line, "cells", "land");
  const auto height = field_of(line, "cells", "height");
  const auto summer = field_of(line, "cells", "temperature_summer");
  const auto winter = field_of(line, "cells", "temperature_winter");
  const auto precipitation = field_of(line, "cells", "precipitation");
  const auto climate = field_of(line, "cells", "climate");
  const auto population = field_of(line, "cells", "population");
  const auto lat_sin = field_of(line, "cells", "lat_sin");

  size_t land_cells = 0;
  double highest = -1e9;
  double deepest = 1e9;
  double temperature_sum = 0.0;
  double land_precipitation = 0.0;
  double driest = 1e9;
  double wettest = -1e9;
  double population_sum = 0.0;
  std::array<size_t, climate_count> zones{};
  // Широтные полосы: то самое место, где видно связь суши с вращением — распределение суши по
  // широте не должно быть равномерным, если плиты дрейфуют поперёк широт.
  std::array<size_t, 6> land_by_latitude{};
  std::array<size_t, 6> cells_by_latitude{};

  for (size_t i = 0; i < count; ++i) {
    const bool is_land = land.get(i) != 0.0;
    land_cells += is_land ? 1 : 0;
    highest = std::max(highest, height.get(i));
    deepest = std::min(deepest, height.get(i));
    temperature_sum += 0.5 * (summer.get(i) + winter.get(i));
    population_sum += population.get(i);
    // Осадки считаются ПО СУШЕ: они и нормированы на среднее по суше, поэтому среднее по планете
    // (где океан всегда мокрый) не значит ничего.
    if (is_land) {
      land_precipitation += precipitation.get(i);
      driest = std::min(driest, precipitation.get(i));
      wettest = std::max(wettest, precipitation.get(i));
    }

    const auto zone = size_t(climate.get(i));
    if (zone < climate_count) {
      zones[zone] += 1;
    }

    const double latitude = std::asin(std::clamp(lat_sin.get(i), -1.0, 1.0)) * 180.0 / std::numbers::pi;
    const auto band = size_t(std::clamp((latitude + 90.0) / 30.0, 0.0, 5.999));
    cells_by_latitude[band] += 1;
    land_by_latitude[band] += is_land ? 1 : 0;
  }

  // Океан считается ОБЪЁМОМ, а не только долей поверхности, и это разные требования к миру. Доля
  // поверхности — вопрос игры: жизнь идёт по суше, и океан на карте воспринимается меньше, чем он есть.
  // Объём — вопрос физики: воды у планеты столько, сколько её есть, и если суши стало больше, океан
  // обязан стать глубже, а не исчезнуть. Земной ориентир: 1.335e9 км³ при средней глубине 3688 м.
  const double planet_radius_km = 6371.0;
  const double cell_area_km2 = 4.0 * std::numbers::pi * planet_radius_km * planet_radius_km / double(count);
  const double sea_level_value = field_value(line, "state", "sea_level", 0);
  double water_depth_sum = 0.0;
  size_t water_cells = 0;
  for (size_t i = 0; i < count; ++i) {
    if (land.get(i) == 0.0) {
      water_depth_sum += std::max(0.0, sea_level_value - height.get(i));
      ++water_cells;
    }
  }
  const double ocean_volume_km3 = water_depth_sum * 0.001 * cell_area_km2;
  constexpr double earth_ocean_volume_km3 = 1.335e9;

  std::cout << "  land                " << land_cells << " cells ("
            << std::format("{:.1f}", 100.0 * double(land_cells) / double(count)) << "%)\n"
            << "  relief              " << std::format("{:.0f}", deepest) << " .. "
            << std::format("{:.0f}", highest) << " m, sea level "
            << std::format("{:.0f}", field_value(line, "state", "sea_level", 0)) << " m\n"
            << "  mean temperature    " << std::format("{:.1f}", temperature_sum / double(count)) << " C\n"
            << "  land precipitation  " << std::format("{:.2f}", land_cells == 0 ? 0.0 : driest) << " .. "
            << std::format("{:.2f}", land_cells == 0 ? 0.0 : wettest) << ", mean "
            << std::format("{:.2f}", land_cells == 0 ? 0.0 : land_precipitation / double(land_cells))
            << " (shares of the land mean)\n"
            << "  population          " << std::format("{:.0f}", population_sum) << "\n"
            << "  ocean               " << std::format("{:.1f}", 100.0 * double(water_cells) / double(count))
            << "% of the surface, mean depth "
            << std::format("{:.0f}", water_cells == 0 ? 0.0 : water_depth_sum / double(water_cells))
            << " m, volume " << std::format("{:.3f}", ocean_volume_km3 / earth_ocean_volume_km3) << " Earth\n";

  print_relief_profile(line, count);

  // Покрытие культурами считается от ПРИГОДНОЙ суши, а не от всей: культура не занимает ледник и
  // пустыню, через них ходят. Без этой доли непонятно, мало культур или мало пригодной земли.
  const auto culture = field_of(line, "cells", "culture");
  const auto culture_mask = field_of(line, "cells", "culture_mask");
  size_t habitable_cells = 0;
  size_t cultured_cells = 0;
  for (size_t i = 0; i < count; ++i) {
    habitable_cells += culture_mask.get(i) != 0.0 ? 1 : 0;
    cultured_cells += culture.get(i) != 0.0 ? 1 : 0;
  }

  std::cout << "  habitable land      " << habitable_cells << " cells ("
            << std::format("{:.1f}", land_cells == 0 ? 0.0 : 100.0 * double(habitable_cells) / double(land_cells))
            << "% of land), cultures hold "
            << std::format("{:.1f}", habitable_cells == 0 ? 0.0 : 100.0 * double(cultured_cells) / double(habitable_cells))
            << "% of it\n";

  const auto province_count = size_t(field_value(line, "state", "province_count", 0));
  const auto min_cells = size_t(field_value(line, "state", "province_min_cells", 0));
  const auto max_cells = size_t(field_value(line, "state", "province_max_cells", 0));

  std::cout << "  plates              " << size_t(field_value(line, "state", "plate_count", 0)) << "\n"
            << "  provinces           " << province_count << "\n"
            << "  sea zones           " << size_t(field_value(line, "state", "sea_zone_count", 0)) << "\n"
            << "  cultures            " << size_t(field_value(line, "state", "culture_count", 0)) << "\n"
            << "  history events      " << size_t(field_value(line, "state", "event_count", 0)) << "\n";

  // География: иерархия названных мест. В отчёте она стоит рядом с числом провинций намеренно —
  // каждый уровень задан РАЗМЕРОМ в провинциях, поэтому проверять его надо в тех же единицах.
  print_geography(line, opts.continent_min_provinces, opts.ocean_zones);

  // Куски суши и размеры провинций: две величины, ради которых генерация вообще усредняется. Если
  // острова мельче провинции, а провинции разбегаются по размеру, играть на карте нельзя, сколько бы
  // физики в ней ни было.
  const auto masses = measure_land_masses(line, count, min_cells);
  std::cout << "  land masses         " << masses.components << " components, largest "
            << std::format("{:.1f}", land_cells == 0 ? 0.0 : 100.0 * double(masses.largest) / double(land_cells))
            << "% of land, " << masses.playable << " hold a province, " << masses.specks << " specks\n";
  std::cout << "  oceanic islands     " << masses.oceanic << " pieces on oceanic crust, "
            << std::format("{:.1f}", land_cells == 0 ? 0.0 : 100.0 * double(masses.oceanic_cells) / double(land_cells))
            << "% of land, median " << std::format("{:.0f}", masses.median_oceanic_size) << " cells, elongation "
            << std::format("{:.1f}", masses.median_elongation) << ", " << masses.strips << " strips over 4x\n";
  // Сам бассейн, а не его следствия. Без этой строки «скопления не получаются» не отличить от
  // «бассейн вообще не тонет»: острова считаются по готовой суше, и по их числу нельзя понять, где
  // именно механизм не сработал.
  {
    const auto basin = field_of(line, "cells", "basin");
    const auto heights = field_of(line, "cells", "height");
    const auto land_mask = field_of(line, "cells", "land");
    const double sea_level = field_value(line, "state", "sea_level", 0);

    size_t inside = 0;
    size_t inside_land = 0;
    double depth_sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
      if (basin.get(i) <= 0.5) {
        continue;
      }
      ++inside;
      if (land_mask.get(i) != 0.0) {
        ++inside_land;
      } else {
        depth_sum += sea_level - heights.get(i);
      }
    }
    const size_t inside_water = inside - inside_land;
    std::cout << "  back-arc basins     " << std::format("{:.2f}", 100.0 * double(inside) / double(count))
              << "% of the surface, " << std::format("{:.0f}", inside == 0 ? 0.0 : 100.0 * double(inside_land) / double(inside))
              << "% of it is land, water there is "
              << std::format("{:.0f}", inside_water == 0 ? 0.0 : depth_sum / double(inside_water)) << " m deep\n";
  }

  std::cout << "  shelf archipelagos  " << masses.shelf_islands << " small pieces on continental crust, "
            << std::format("{:.1f}", land_cells == 0 ? 0.0 : 100.0 * double(masses.shelf_cells) / double(land_cells))
            << "% of land, median " << std::format("{:.0f}", masses.median_shelf_size) << " cells, "
            << std::format("{:.0f}", masses.median_neighbours) << " neighbours within 0.12 rad, "
            << masses.clustered << " in clusters\n";

  const auto province_cells_field = field_of(line, "provinces", "cells");
  size_t smallest_province = std::numeric_limits<size_t>::max();
  size_t largest_province = 0;
  size_t below = 0;
  size_t above = 0;
  size_t counted = 0;
  // Записи областей нумеруются С ЕДИНИЦЫ: строка 0 — это корзина «метки нет» у group_by и
  // accumulate (для провинций там лежит вся вода). Соглашение объявлено в buffers.tavl.
  for (size_t i = 1; i <= province_count && i < province_cells_field.count(); ++i) {
    const auto size = size_t(province_cells_field.get(i));
    if (size == 0) {
      continue;
    }
    ++counted;
    smallest_province = std::min(smallest_province, size);
    largest_province = std::max(largest_province, size);
    below += size < min_cells ? 1 : 0;
    above += size > max_cells ? 1 : 0;
  }
  if (counted == 0) {
    smallest_province = 0;
  }

  std::cout << "  province size       " << smallest_province << " .. " << largest_province << " cells (bounds "
            << min_cells << " .. " << max_cells << "), below " << below << ", above " << above << ", merged "
            << size_t(field_value(line, "state", "province_merged", 0)) << "\n";

  // Области рельефа: ради них существует отдельный шаг, и без этой таблицы «области есть» остаётся
  // утверждением, а не измерением. Доля считается от поверхности, а не от суши: половина видов
  // подводные.
  const auto landform = field_of(line, "cells", "landform");
  std::array<size_t, landform_count> landforms{};
  for (size_t i = 0; i < count; ++i) {
    const auto kind = size_t(landform.get(i));
    if (kind < landform_count) {
      landforms[kind] += 1;
    }
  }
  // Уклон СВОЕГО вида — то самое измерение, ради которого шаг усиления и существует. Доля области
  // говорит, что вид опознан; уклон говорит, стал ли он собой. Считается по дугам, у которых ОБА
  // конца одного вида: дуга через границу видов принадлежит обоим и ни одному.
  const auto offsets_field = field_of(line, "cell_offsets", "start");
  const auto arcs_field = field_of(line, "cell_arcs", "cell");
  std::array<std::vector<double>, landform_count> landform_steps{};
  for (size_t i = 0; i < count; ++i) {
    const auto kind = size_t(landform.get(i));
    if (kind >= landform_count) {
      continue;
    }
    const auto first = size_t(offsets_field.get(i));
    const auto last = size_t(offsets_field.get(i + 1));
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs_field.get(k));
      if (other <= i || other >= count || size_t(landform.get(other)) != kind) {
        continue;
      }
      landform_steps[kind].push_back(std::abs(height.get(i) - height.get(other)));
    }
  }

  const double landform_spacing = std::sqrt(4.0 * std::numbers::pi / double(count));
  const double landform_per_hundred = 0.0157 / landform_spacing;
  std::cout << "  landforms (share of the surface, typical slope in m per 100 km):\n";
  for (size_t kind = 0; kind < landform_count; ++kind) {
    if (landforms[kind] == 0) {
      continue;
    }
    std::cout << "    " << std::format("{:<16}", landform_names[kind]) << std::format("{:>7}", landforms[kind])
              << " (" << std::format("{:>4.1f}", 100.0 * double(landforms[kind]) / double(count)) << "%)  slope "
              << std::format("{:>4.0f}", median_of(landform_steps[kind]) * landform_per_hundred) << "\n";
  }

  std::cout << "  climate:\n";
  for (size_t zone = 0; zone < climate_count; ++zone) {
    if (zones[zone] == 0) {
      continue;
    }
    std::cout << "    " << climate_table[zone].symbol << " " << std::format("{:<9}", climate_table[zone].name)
              << std::format("{:>7}", zones[zone]) << " ("
              << std::format("{:.1f}", 100.0 * double(zones[zone]) / double(count)) << "%)\n";
  }

  std::cout << "  land by latitude:\n";
  for (size_t band = 0; band < land_by_latitude.size(); ++band) {
    const double share = cells_by_latitude[band] == 0
                           ? 0.0
                           : 100.0 * double(land_by_latitude[band]) / double(cells_by_latitude[band]);
    std::cout << "    " << std::format("{:>4}", int(-90 + int(band) * 30)) << ".."
              << std::format("{:>4}", int(-60 + int(band) * 30)) << " deg  " << std::format("{:.1f}", share) << "%\n";
  }
}

// Диапазон поля по именам. Существует потому, что headless-генератор отлаживается ЧИСЛАМИ: «население
// 17 миллионов при ёмкости в два» — это ответ, а «мир выглядит странно» — нет.
void print_field_stats(originator::pipeline& line, const options& opts, const std::string& names) {
  for (const auto& name : split_words(names)) {
    const auto field = field_of(line, "cells", name);
    double lowest = std::numeric_limits<double>::infinity();
    double highest = -std::numeric_limits<double>::infinity();
    double total = 0.0;
    for (size_t i = 0; i < opts.cells; ++i) {
      const double value = field.get(i);
      lowest = std::min(lowest, value);
      highest = std::max(highest, value);
      total += value;
    }
    std::cout << "  " << std::format("{:<20}", name) << std::format("{:>14.5g}", lowest) << " .. "
              << std::format("{:>14.5g}", highest) << "   mean " << std::format("{:>12.5g}", total / double(opts.cells))
              << "\n";
  }
}

// ASCII-карта: единственная «картинка» headless-площадки. Существует не для красоты — по ней сразу
// видно то, что числами видно плохо: остались ли материки материками, легли ли пустыни в пояса, есть
// ли дождевая тень за горами.
void print_map(originator::pipeline& line, const options& opts) {
  constexpr size_t width = 128;
  constexpr size_t height_rows = 40;

  const auto positions = field_of(line, "cells", "position");
  const auto climate = field_of(line, "cells", "climate");

  utils::kd_tree<uint32_t, std::array<float, 3>, 3> tree;
  tree.reserve(opts.cells);
  for (size_t i = 0; i < opts.cells; ++i) {
    tree.insert(std::array<float, 3>{float(positions.get(i, 0)), float(positions.get(i, 1)),
                                     float(positions.get(i, 2))},
                uint32_t(i));
  }
  tree.build();

  std::cout << "\n";
  for (size_t row = 0; row < height_rows; ++row) {
    const double latitude = (0.5 - (double(row) + 0.5) / double(height_rows)) * std::numbers::pi;
    std::string line_text;
    line_text.reserve(width);
    for (size_t column = 0; column < width; ++column) {
      const double longitude = ((double(column) + 0.5) / double(width) - 0.5) * 2.0 * std::numbers::pi;
      const std::array<float, 3> query{float(std::cos(latitude) * std::cos(longitude)), float(std::sin(latitude)),
                                       float(std::cos(latitude) * std::sin(longitude))};
      const auto* node = tree.nearest(query, 0.5f, [](const uint32_t&) { return true; });
      if (node == nullptr) {
        line_text.push_back(' ');
        continue;
      }
      const auto zone = size_t(climate.get(node->payload));
      line_text.push_back(zone < climate_count ? climate_table[zone].symbol : '?');
    }
    std::cout << line_text << "\n";
  }
  std::cout << "\n";
}

// Связность метки по графу: все клетки с одной меткой обязаны образовывать ОДНУ область. Для
// провинции это не косметика: разорванная провинция ломает и отрисовку границы, и всякий геймплей,
// который считает соседство.
bool labels_are_connected(originator::pipeline& line, const std::string_view& label_field,
                          const size_t count, const size_t label_limit) {
  const auto offsets = field_of(line, "cell_offsets", "start");
  const auto arcs = field_of(line, "cell_arcs", "cell");
  const auto labels = field_of(line, "cells", label_field);

  std::vector<uint32_t> first_cell(label_limit + 1, uint32_t(-1));
  for (size_t i = 0; i < count; ++i) {
    const auto label = size_t(labels.get(i));
    if (label == 0 || label > label_limit) {
      continue;
    }
    if (first_cell[label] == uint32_t(-1)) {
      first_cell[label] = uint32_t(i);
    }
  }

  std::vector<uint8_t> seen(count, 0);
  std::vector<uint32_t> stack;
  size_t reachable = 0;
  size_t labelled = 0;

  for (size_t i = 0; i < count; ++i) {
    labelled += labels.get(i) != 0.0 ? 1 : 0;
  }

  for (size_t label = 1; label <= label_limit; ++label) {
    if (first_cell[label] == uint32_t(-1)) {
      continue;
    }
    stack.clear();
    stack.push_back(first_cell[label]);
    seen[first_cell[label]] = 1;
    while (!stack.empty()) {
      const auto cell = stack.back();
      stack.pop_back();
      ++reachable;
      const auto begin = size_t(offsets.get(cell));
      const auto end = size_t(offsets.get(size_t(cell) + 1));
      for (size_t k = begin; k < end; ++k) {
        const auto other = uint32_t(arcs.get(k));
        if (other < count && seen[other] == 0 && size_t(labels.get(other)) == label) {
          seen[other] = 1;
          stack.push_back(other);
        }
      }
    }
  }

  return reachable == labelled;
}

int run_verify(const options& opts) {
  std::cout << "GN02 verify: " << opts.cells << " cells, seed " << opts.seed << "\n";

  size_t checks = 0;
  size_t failures = 0;
  const auto check = [&](const bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
      ++failures;
      std::cout << "  FAILED: " << label << "\n";
    }
  };

  originator::tool_registry tools;
  tools.add_standard_tools(); // включает scatter-инструменты
  tools.add_graph_tools();
  originator::add_all_primitives(tools);
  gn02::add_planet_tools(tools);

  const auto description = load_description(opts);
  const size_t count = opts.cells;

  auto reference = generate(opts, tools, description, nullptr);
  auto& line = *reference.line;

  // 1. Топология замкнутой поверхности.
  {
    const auto positions = field_of(line, "cells", "position");
    const auto offsets = field_of(line, "cell_offsets", "start");
    const auto arcs = field_of(line, "cell_arcs", "cell");

    bool unit_length = true;
    for (size_t i = 0; i < count; ++i) {
      const double x = positions.get(i, 0);
      const double y = positions.get(i, 1);
      const double z = positions.get(i, 2);
      unit_length = unit_length && std::abs(std::sqrt(x * x + y * y + z * z) - 1.0) < 1e-5;
    }
    check(unit_length, "every cell sits on the unit sphere");

    bool symmetric = true;
    bool degrees_sane = true;
    size_t total_arcs = 0;
    for (size_t i = 0; i < count && symmetric; ++i) {
      const auto begin = size_t(offsets.get(i));
      const auto end = size_t(offsets.get(i + 1));
      degrees_sane = degrees_sane && end >= begin + 3 && end <= begin + 24;
      total_arcs += end - begin;
      for (size_t k = begin; k < end; ++k) {
        const auto other = size_t(arcs.get(k));
        bool back = false;
        const auto other_begin = size_t(offsets.get(other));
        const auto other_end = size_t(offsets.get(other + 1));
        for (size_t j = other_begin; j < other_end; ++j) {
          back = back || size_t(arcs.get(j)) == i;
        }
        symmetric = symmetric && back && other != i;
      }
    }
    check(symmetric, "adjacency is symmetric and loop free");
    check(degrees_sane, "cell degree stays within sane bounds");
    check(total_arcs == size_t(offsets.get(count)), "CSR offsets agree with the arc count");

    // Связность: одна заливка от одной клетки обязана накрыть планету целиком. Дырявый граф не
    // виден ни в одной локальной проверке, а ломает всё дальнейшее.
    std::vector<uint8_t> seen(count, 0);
    std::vector<uint32_t> stack{0};
    seen[0] = 1;
    size_t reached = 0;
    while (!stack.empty()) {
      const auto cell = stack.back();
      stack.pop_back();
      ++reached;
      const auto begin = size_t(offsets.get(cell));
      const auto end = size_t(offsets.get(size_t(cell) + 1));
      for (size_t k = begin; k < end; ++k) {
        const auto other = uint32_t(arcs.get(k));
        if (other < count && seen[other] == 0) {
          seen[other] = 1;
          stack.push_back(other);
        }
      }
    }
    check(reached == count, std::format("the adjacency graph is connected ({} of {} cells)", reached, count));
  }

  // 2. Тектоника: плита есть у каждой клетки, и суммы по плитам сходятся.
  {
    const auto plate = field_of(line, "cells", "plate");
    const auto plate_offsets = field_of(line, "plate_offsets", "start");
    const auto plate_count = size_t(field_value(line, "state", "plate_count", 0));

    bool assigned = true;
    for (size_t i = 0; i < count; ++i) {
      const auto value = size_t(plate.get(i));
      assigned = assigned && value >= 1 && value <= plate_count;
    }
    check(assigned, "every cell belongs to a plate");
    check(plate_count >= 4, std::format("enough plates came out ({})", plate_count));
    check(size_t(plate_offsets.get(plate_offsets.count() - 1)) == count,
          "the plate layout covers the planet");
    check(labels_are_connected(line, "plate", count, plate_count), "every plate is connected");
  }

  // 3. Поверхность: доля суши держится около заданной, рельеф не выродился.
  {
    const auto land = field_of(line, "cells", "land");
    const auto height = field_of(line, "cells", "height");
    const auto sea_level = field_value(line, "state", "sea_level", 0);

    size_t land_cells = 0;
    size_t above_level = 0;
    double lowest = 1e30;
    double highest = -1e30;
    bool mask_matches = true;
    for (size_t i = 0; i < count; ++i) {
      const bool is_land = land.get(i) != 0.0;
      land_cells += is_land ? 1 : 0;
      lowest = std::min(lowest, height.get(i));
      highest = std::max(highest, height.get(i));
      // Маска суши согласована с уровнем моря В ОДНУ СТОРОНУ, и это не послабление, а следствие
      // прибрежной эрозии: она СНИМАЕТ клетки, которые выше уровня, но почти не имеют суши по
      // соседству. Поэтому «выше уровня ⇒ суша» больше неверно, а «суша ⇒ выше уровня» верно и
      // проверяется точно; запас в 300 метров покрывает поправку на вращение.
      if (is_land) {
        mask_matches = mask_matches && height.get(i) > sea_level - 300.0;
      }
      // Тот же порог, что и у проверки суши: местный уровень отличается от общего не больше чем на
      // треть вздутия, и 300 метров покрывают это с запасом.
      above_level += height.get(i) > sea_level - 300.0 ? 1 : 0;
    }

    const double share = double(land_cells) / double(count);
    const double target = field_value(line, "state", "land_target", 0);
    check(std::abs(share - target) < 0.02,
          std::format("land share {:.3f} is close to the declared {:.3f}", share, target));
    check(mask_matches, "every land cell sits above sea level");
    // Эрозия только СНИМАЕТ сушу, поэтому суши не больше, чем клеток выше уровня. Интересна вторая
    // половина: она не должна съедать заметную часть материков. Четверть — это уже не «смыло крошки»,
    // а другая планета, и порог стоит именно там.
    check(land_cells <= above_level && above_level - land_cells < above_level / 4,
          std::format("coastal erosion only trims the edge ({} land cells of {} above the level)", land_cells,
                      above_level));
    check(highest > 3000.0 && lowest < -3000.0,
          std::format("relief has both mountains and depths ({:.0f} .. {:.0f} m)", lowest, highest));
  }

  // 3a. Форма рельефа, а не только его размах. Три свойства, которые модель обязана давать, и каждое
  // ловит ошибку, которую здесь уже допускали.
  {
    const auto land = field_of(line, "cells", "land");
    const auto height = field_of(line, "cells", "height");
    const auto convergent = field_of(line, "cells", "convergent_distance");
    const double sea_level = field_value(line, "state", "sea_level", 0);

    size_t water_cells = 0;
    size_t deep_cells = 0;
    double high_distance = 0.0;
    size_t high_cells = 0;
    double all_distance = 0.0;

    for (size_t i = 0; i < count; ++i) {
      const double relative = height.get(i) - sea_level;
      const bool is_land = land.get(i) != 0.0;
      water_cells += is_land ? 0 : 1;
      deep_cells += !is_land && relative < -3000.0 ? 1 : 0;
      all_distance += convergent.get(i);
      if (is_land && relative > 2000.0) {
        high_distance += convergent.get(i);
        ++high_cells;
      }
    }

    const auto steps = measure_relief_steps(line, count);
    // Амплитуда детали зависит от места. Пока она была одинаковой, дно выходило таким же бугристым,
    // как суша, — и это видно на глобусе раньше, чем в числах.
    check(steps.plain > 0.0 && steps.plain < steps.land,
          std::format("the abyssal plain is flatter than the land ({:.0f} m against {:.0f} m per neighbour)",
                      steps.plain, steps.land));
    // Двугорбая гипсография: дно океана — это в основном равнина, а не пологий скат от берега.
    const double deep_share = water_cells == 0 ? 0.0 : double(deep_cells) / double(water_cells);
    check(deep_share > 0.5,
          std::format("the sea floor is mostly abyssal ({:.0f}% of water deeper than 3 km)", 100.0 * deep_share));
    // Горы стоят у сходящихся границ. Проверка ловит ровно ту ошибку, из-за которой ширины пояса не
    // значили ничего: скорость сближения бралась у самой клетки, а она ненулевая только на ленте в
    // одну клетку, поэтому «пояс» был лентой, а не поясом.
    const double high_mean = high_cells == 0 ? 0.0 : high_distance / double(high_cells);
    const double all_mean = count == 0 ? 0.0 : all_distance / double(count);
    check(high_cells > 0 && high_mean < 0.5 * all_mean,
          std::format("mountains follow the convergent belts ({:.1f} steps against {:.1f} on average)",
                      high_mean, all_mean));
  }

  // 3b. Области рельефа: они опознаны и они РАЗНЫЕ. Вторая половина важнее первой: опознать можно и
  // одинаковое, а шаг усиления существует ровно ради того, чтобы область стала собой.
  {
    const auto landform = field_of(line, "cells", "landform");
    const auto height = field_of(line, "cells", "height");
    const auto offsets = field_of(line, "cell_offsets", "start");
    const auto arcs = field_of(line, "cell_arcs", "cell");

    constexpr size_t kinds = 11;
    std::array<size_t, kinds> shares{};
    std::array<std::vector<double>, kinds> steps{};
    bool addressable = true;
    for (size_t i = 0; i < count; ++i) {
      const auto kind = size_t(landform.get(i));
      addressable = addressable && kind < kinds;
      if (kind >= kinds) {
        continue;
      }
      shares[kind] += 1;
      const auto first = size_t(offsets.get(i));
      const auto last = size_t(offsets.get(i + 1));
      for (size_t k = first; k < last; ++k) {
        const auto other = size_t(arcs.get(k));
        if (other <= i || other >= count || size_t(landform.get(other)) != kind) {
          continue;
        }
        steps[kind].push_back(std::abs(height.get(i) - height.get(other)));
      }
    }

    size_t present = 0;
    for (size_t kind = 0; kind < kinds; ++kind) {
      present += double(shares[kind]) / double(count) > 0.005 ? 1 : 0;
    }
    check(addressable, "every cell has a landform kind");
    check(present >= 6, std::format("the planet has a variety of landforms ({} kinds above half a percent)", present));

    // Порядок «равнина ровнее холмов ровнее гор» — это и есть определение этих слов. Если он не
    // держится, значит опознание разложило клетки по видам, а усиление их не различило.
    const double plain = median_of(steps[5]);
    const double hills = median_of(steps[8]);
    const double mountains = median_of(steps[9]);
    check(plain > 0.0 && hills > plain && mountains > 2.0 * hills,
          std::format("plains, hills and mountains differ by slope ({:.0f} < {:.0f} < {:.0f} m per neighbour)",
                      plain, hills, mountains));
  }

  // 4. Климат: лето не холоднее зимы, осадки неотрицательны, дождевая тень существует.
  {
    const auto summer = field_of(line, "cells", "temperature_summer");
    const auto winter = field_of(line, "cells", "temperature_winter");
    const auto precipitation = field_of(line, "cells", "precipitation");
    const auto lat_sin = field_of(line, "cells", "lat_sin");
    const auto land = field_of(line, "cells", "land");

    bool ordered = true;
    bool finite = true;
    double equator_temperature = 0.0;
    size_t equator_cells = 0;
    double polar_temperature = 0.0;
    size_t polar_cells = 0;
    for (size_t i = 0; i < count; ++i) {
      ordered = ordered && summer.get(i) >= winter.get(i) - 1e-6;
      finite = finite && std::isfinite(summer.get(i)) && std::isfinite(winter.get(i)) &&
               precipitation.get(i) >= 0.0 && std::isfinite(precipitation.get(i));

      const double s = std::abs(lat_sin.get(i));
      if (s < 0.1) {
        equator_temperature += 0.5 * (summer.get(i) + winter.get(i));
        ++equator_cells;
      }
      if (s > 0.95) {
        polar_temperature += 0.5 * (summer.get(i) + winter.get(i));
        ++polar_cells;
      }
    }
    check(ordered, "summer is nowhere colder than winter");

    // КЛАСС, ВЫДАННЫЙ ПРАВИЛОМ, ОБЯЗАН ИМЕТЬ ИМЯ И ЦВЕТ. Одно множество классов перечислено трижды —
    // правило в `S06_climate_zone.ds`, имена в отчёте, палитра в просмотрщике, — и первое из трёх это
    // ДАННЫЕ, поэтому связать его с остальными может только проверка. Палитра берёт цвет с зажимом
    // по последнему элементу, значит новый класс молча покрасился бы цветом предыдущего, и выглядело
    // бы это как «правило не сработало».
    {
      const auto climate_field = field_of(line, "cells", "climate");
      const auto landform_field = field_of(line, "cells", "landform");
      double highest_zone = 0.0;
      double highest_kind = 0.0;
      for (size_t i = 0; i < count; ++i) {
        highest_zone = std::max(highest_zone, climate_field.get(i));
        highest_kind = std::max(highest_kind, landform_field.get(i));
      }
      check(highest_zone < double(climate_count) && highest_kind < double(landform_count),
            std::format("every class the rules produce has a name and a colour (climate up to {:.0f} of {}, "
                        "landform up to {:.0f} of {})",
                        highest_zone, climate_count, highest_kind, landform_count));
    }
    check(finite, "temperatures and rainfall are finite and non-negative");
    check(equator_cells > 0 && polar_cells > 0 &&
            equator_temperature / double(equator_cells) > polar_temperature / double(polar_cells) + 20.0,
          "the equator is at least 20 C warmer than the poles");

    // Континентальность: сезонная амплитуда внутри материка обязана быть больше, чем на воде. Это
    // проверка не формулы, а того, что расстояние до океана вообще доехало до температуры.
    double inland_amplitude = 0.0;
    size_t inland_cells = 0;
    double ocean_amplitude = 0.0;
    size_t ocean_cells = 0;
    const auto ocean_distance = field_of(line, "cells", "ocean_distance");
    for (size_t i = 0; i < count; ++i) {
      const double amplitude = summer.get(i) - winter.get(i);
      if (land.get(i) != 0.0 && ocean_distance.get(i) > 6.0) {
        inland_amplitude += amplitude;
        ++inland_cells;
      }
      if (land.get(i) == 0.0) {
        ocean_amplitude += amplitude;
        ++ocean_cells;
      }
    }
    check(inland_cells > 0 && ocean_cells > 0 &&
            inland_amplitude / double(inland_cells) > ocean_amplitude / double(ocean_cells) + 1.0,
          "seasonal swing inland exceeds the swing over water");
  }

  // 5. Области: провинции покрывают сушу, морские зоны — воду, и те и другие связны.
  {
    const auto land = field_of(line, "cells", "land");
    const auto province = field_of(line, "cells", "province");
    const auto sea_zone = field_of(line, "cells", "sea_zone");
    const auto province_count = size_t(field_value(line, "state", "province_count", 0));
    const auto sea_zone_count = size_t(field_value(line, "state", "sea_zone_count", 0));

    bool land_covered = true;
    bool water_covered = true;
    bool disjoint = true;
    for (size_t i = 0; i < count; ++i) {
      const bool is_land = land.get(i) != 0.0;
      const bool has_province = province.get(i) != 0.0;
      const bool has_zone = sea_zone.get(i) != 0.0;
      land_covered = land_covered && (!is_land || has_province);
      water_covered = water_covered && (is_land || has_zone);
      disjoint = disjoint && !(has_province && has_zone);
    }
    check(land_covered, "every land cell belongs to a province");
    check(water_covered, "every water cell belongs to a sea zone");
    check(disjoint, "provinces and sea zones do not overlap");
    check(province_count >= 8 && sea_zone_count >= 4,
          std::format("enough areas: {} provinces, {} sea zones", province_count, sea_zone_count));
    check(labels_are_connected(line, "province", count, province_count), "every province is connected");
    check(labels_are_connected(line, "sea_zone", count, sea_zone_count), "every sea zone is connected");

    // Запись области и её суммы описывают ОДНУ И ТУ ЖЕ область.
    //
    // Проверка нужна потому, что сойтись эти две вещи могут только по соглашению, а соглашений было
    // два сразу: group_by и accumulate раскладывают по корзине, равной сырому ключу, а скрипт писал
    // центр и размер по «метка минус один». Ни одна проверка связности такого не ловит — буфер-то
    // валиден. Ловится это сложением: сумма высот по всем записям областей обязана совпасть с суммой
    // высот по клеткам суши, а при сдвиге на единицу в неё попадает корзина 0, то есть вся вода.
    {
      const auto heights = field_of(line, "cells", "height");
      const auto provinces_of = field_of(line, "cells", "province");
      const auto province_height = field_of(line, "provinces", "height_sum");

      double from_cells = 0.0;
      for (size_t i = 0; i < count; ++i) {
        if (provinces_of.get(i) != 0.0) {
          from_cells += heights.get(i);
        }
      }
      double from_records = 0.0;
      for (size_t i = 1; i <= province_count && i < province_height.count(); ++i) {
        from_records += province_height.get(i);
      }
      const double scale = std::max(1.0, std::abs(from_cells));
      check(std::abs(from_records - from_cells) < 1e-6 * scale,
            std::format("province records line up with their sums ({:.0f} from cells, {:.0f} from records)",
                        from_cells, from_records));
      check(province_height.get(0) <= 0.0,
            std::format("record row 0 is the unlabelled bucket, not a province (height sum {:.0f})",
                        province_height.get(0)));
    }

    // ГЕОГРАФИЧЕСКАЯ ИЕРАРХИЯ. Проверяется не «она есть», а два её обещания: что она ПОЛНАЯ (у каждой
    // клетки есть место на каждом уровне) и что её границы СОВПАДАЮТ С ГРАНИЦАМИ ПРОВИНЦИЙ. Второе —
    // главное условие задачи, и проверить его можно только так: пройти по клеткам и убедиться, что
    // внутри одной провинции значение уровня одно. Если уровень когда-нибудь начнут растить по
    // клеткам, эта проверка упадёт первой.
    {
      const auto land_of = field_of(line, "cells", "land");
      const auto provinces_of = field_of(line, "cells", "province");
      const auto zones_of = field_of(line, "cells", "sea_zone");
      const auto mass_of = field_of(line, "cells", "land_mass");
      const auto continent_of = field_of(line, "cells", "continent");
      const auto region_of = field_of(line, "cells", "historical_region");
      const auto ocean_of = field_of(line, "cells", "ocean");

      size_t land_without_place = 0;
      size_t water_without_region = 0;
      for (size_t i = 0; i < count; ++i) {
        if (land_of.get(i) != 0.0) {
          if (mass_of.get(i) == 0.0 || continent_of.get(i) == 0.0 || region_of.get(i) == 0.0) {
            ++land_without_place;
          }
        } else if (zones_of.get(i) == 0.0) {
          ++water_without_region;
        }
      }
      check(land_without_place == 0,
            std::format("every land cell has a land mass, a continent and a historical region ({} without)",
                        land_without_place));
      check(water_without_region == 0,
            std::format("every water cell has an oceanic region ({} without)", water_without_region));

      const auto uniform_inside = [&](const originator::const_field_accessor& owner,
                                      const originator::const_field_accessor& level, const size_t owner_count) {
        std::vector<double> seen(owner_count + 2, -1.0);
        size_t broken = 0;
        for (size_t i = 0; i < count; ++i) {
          const auto key = size_t(owner.get(i));
          if (key == 0 || key >= seen.size()) {
            continue;
          }
          if (seen[key] < 0.0) {
            seen[key] = level.get(i);
          } else if (seen[key] != level.get(i)) {
            ++broken;
          }
        }
        return broken;
      };

      const auto mass_broken = uniform_inside(provinces_of, mass_of, province_count);
      const auto continent_broken = uniform_inside(provinces_of, continent_of, province_count);
      const auto region_broken = uniform_inside(provinces_of, region_of, province_count);
      const auto ocean_broken = uniform_inside(zones_of, ocean_of, sea_zone_count);
      check(mass_broken == 0 && continent_broken == 0 && region_broken == 0 && ocean_broken == 0,
            std::format("geographic borders coincide with area borders ({} cells disagree with their area)",
                        mass_broken + continent_broken + region_broken + ocean_broken));
    }

    // Уровень иерархии лежит в ОДНОМ родителе. Для исторической области это инвариант: она растится
    // внутри материка, и выйти за него не может. Для материка инварианта нет намеренно — мелкие
    // массивы прикрепляются к ближайшему материку через воду.
    {
      const auto province_continent = field_of(line, "provinces", "continent");
      const auto province_region = field_of(line, "provinces", "historical_region");
      const auto historical = size_t(field_value(line, "state", "historical_count", 0));

      std::vector<size_t> owner(historical + 2, 0);
      size_t split = 0;
      for (size_t node = 1; node <= province_count && node < province_region.count(); ++node) {
        const auto region = size_t(province_region.get(node));
        if (region == 0 || region >= owner.size()) {
          continue;
        }
        const auto continent = size_t(province_continent.get(node)) + 1;
        if (owner[region] == 0) {
          owner[region] = continent;
        } else if (owner[region] != continent) {
          owner[region] = continent;
          ++split;
        }
      }
      check(split == 0, std::format("every historical region lies in one continent ({} split)", split));
      check(historical >= 8, std::format("the hierarchy is deep enough: {} historical regions", historical));

      // НАЗВАНИЯ РАЗЛИЧИМЫ. Проверка нужна потому, что название собирается из затравки, а затравок
      // столько же, сколько мест: два места могут получить одно имя, и на карте это выглядит как
      // ошибка данных, хотя данные верны. Внутри материка совпадение недопустимо совсем — по имени
      // области там и ориентируются; по планете в целом допускается, как и в жизни.
      const auto names = gn02::build_place_names(line, opts.continent_min_provinces, opts.ocean_zones);
      std::set<std::string> continent_names;
      size_t continent_clashes = 0;
      for (size_t i = 1; i < names.continents.size(); ++i) {
        if (!names.continents[i].empty() && !continent_names.insert(names.continents[i]).second) {
          ++continent_clashes;
        }
      }
      std::map<std::pair<size_t, std::string>, size_t> region_names;
      size_t region_clashes = 0;
      for (size_t i = 1; i <= historical && i < names.historical_regions.size(); ++i) {
        if (names.historical_regions[i].empty()) {
          continue;
        }
        const auto continent = size_t(field_of(line, "historical_regions", "continent").get(i));
        if (++region_names[{continent, names.historical_regions[i]}] > 1) {
          ++region_clashes;
        }
      }
      check(continent_clashes == 0, std::format("continent names are distinct ({} clashes)", continent_clashes));
      check(region_clashes == 0,
            std::format("historical region names are distinct inside a continent ({} clashes)", region_clashes));
      check(!names.oceans.empty() && (names.oceans.size() < 2 || !names.oceans[1].empty()),
            "oceans are named");

      // ТИТУЛЫ. Проверяется то же, что у географии: полнота и вложенность. Плюс баронства, у которых
      // проверка своя — они не область, а точка, и точка обязана лежать в своём графстве.
      const auto province_duchy = field_of(line, "provinces", "duchy");
      const auto province_empire = field_of(line, "provinces", "empire");
      const auto province_realm = field_of(line, "provinces", "realm");
      const auto province_baronies = field_of(line, "provinces", "baronies");
      const auto province_size = field_of(line, "provinces", "cells");

      size_t without_title = 0;
      size_t without_realm = 0;
      size_t without_barony = 0;
      for (size_t node = 1; node <= province_count && node < province_size.count(); ++node) {
        if (province_size.get(node) == 0.0) {
          continue;
        }
        if (province_duchy.get(node) == 0.0 || province_empire.get(node) == 0.0) {
          ++without_title;
        }
        if (province_realm.get(node) == 0.0) {
          ++without_realm;
        }
        if (province_baronies.get(node) == 0.0) {
          ++without_barony;
        }
      }
      check(without_title == 0,
            std::format("every county holds a duchy and an empire title ({} without)", without_title));
      check(without_realm == 0,
            std::format("every county belongs to a de facto realm ({} without)", without_realm));
      check(without_barony == 0, std::format("every county holds a barony ({} without)", without_barony));

      const auto duchies = size_t(field_value(line, "state", "duchy_count", 0));
      std::vector<size_t> duchy_owner(duchies + 2, 0);
      size_t duchy_split = 0;
      for (size_t node = 1; node <= province_count && node < province_duchy.count(); ++node) {
        const auto duchy = size_t(province_duchy.get(node));
        if (duchy == 0 || duchy >= duchy_owner.size()) {
          continue;
        }
        const auto region = size_t(province_region.get(node)) + 1;
        if (duchy_owner[duchy] == 0) {
          duchy_owner[duchy] = region;
        } else if (duchy_owner[duchy] != region) {
          duchy_owner[duchy] = region;
          ++duchy_split;
        }
      }
      check(duchy_split == 0,
            std::format("every duchy lies in one kingdom ({} split)", duchy_split));

      const auto barony_count = size_t(field_value(line, "state", "barony_count", 0));
      const auto barony_cell = field_of(line, "baronies", "cell");
      const auto barony_province = field_of(line, "baronies", "province");
      const auto cell_province = field_of(line, "cells", "province");
      size_t misplaced = 0;
      for (size_t i = 1; i <= barony_count && i < barony_cell.count(); ++i) {
        const auto cell = size_t(barony_cell.get(i));
        if (cell >= count || cell_province.get(cell) != barony_province.get(i)) {
          ++misplaced;
        }
      }
      check(misplaced == 0, std::format("every barony sits in its own county ({} misplaced)", misplaced));
    }

    // Свойства, которых требует ИГРА, а не физика. Планета может быть безупречно правдоподобной и при
    // этом непригодной: один сверхматерик и пустой океан, провинция на пол-континента, острова, в
    // которые не влезает область. Поэтому усреднение проверяется так же, как всё остальное.
    const auto min_cells = size_t(field_value(line, "state", "province_min_cells", 0));
    const auto max_cells = size_t(field_value(line, "state", "province_max_cells", 0));
    const auto province_sizes = field_of(line, "provinces", "cells");

    size_t oversized = 0;
    size_t largest_province = 0;
    for (size_t i = 1; i <= province_count && i < province_sizes.count(); ++i) {
      const auto size = size_t(province_sizes.get(i));
      largest_province = std::max(largest_province, size);
      oversized += size > max_cells ? 1 : 0;
    }
    check(oversized == 0,
          std::format("no province exceeds the upper size bound ({} cells, largest {})", max_cells,
                      largest_province));

    size_t land_total = 0;
    for (size_t i = 0; i < count; ++i) {
      land_total += land.get(i) != 0.0 ? 1 : 0;
    }
    const auto masses = measure_land_masses(line, count, min_cells);
    check(masses.largest < size_t(0.92 * double(land_total)),
          std::format("land is not one supercontinent (largest mass {:.0f}% of land)",
                      land_total == 0 ? 0.0 : 100.0 * double(masses.largest) / double(land_total)));
    check(masses.playable >= 6,
          std::format("enough separate landmasses can hold a province ({} of {})", masses.playable,
                      masses.components));
  }

  // 6. Люди: население стоит там, где пригодно, культуры покрывают обитаемую сушу.
  {
    const auto habitability = field_of(line, "cells", "habitability");
    const auto population = field_of(line, "cells", "population");
    const auto culture = field_of(line, "cells", "culture");
    const auto land = field_of(line, "cells", "land");
    const auto culture_count = size_t(field_value(line, "state", "culture_count", 0));

    bool water_empty = true;
    bool bounded = true;
    double habitable_population = 0.0;
    double barren_population = 0.0;
    size_t cultured = 0;
    size_t habitable_cells = 0;
    size_t barren_cells = 0;
    for (size_t i = 0; i < count; ++i) {
      const bool is_land = land.get(i) != 0.0;
      water_empty = water_empty && (is_land || population.get(i) <= 1e-6);
      bounded = bounded && habitability.get(i) >= 0.0 && habitability.get(i) <= 1.0 &&
                std::isfinite(population.get(i)) && population.get(i) >= 0.0;
      if (habitability.get(i) > 0.5) {
        habitable_population += population.get(i);
        ++habitable_cells;
      } else if (is_land) {
        barren_population += population.get(i);
        ++barren_cells;
      }
      cultured += culture.get(i) != 0.0 ? 1 : 0;
    }
    check(water_empty, "nobody lives on water");
    check(bounded, "habitability in [0,1], population finite and non-negative");
    // Сравниваются ПЛОТНОСТИ, а не суммы, и это исправление после провала на одном из зёрен: у сухой
    // планеты пригодных клеток мало, поэтому их суммарное население законно меньше, а плотность —
    // нет. Проверять надо то свойство, которое обязано держаться при любом мире.
    const double habitable_density = habitable_cells == 0 ? 0.0 : habitable_population / double(habitable_cells);
    const double barren_density = barren_cells == 0 ? 0.0 : barren_population / double(barren_cells);
    check(habitable_cells > 0 && habitable_density > barren_density,
          std::format("population density is higher in habitable cells ({:.3g} against {:.3g})",
                      habitable_density, barren_density));
    check(culture_count >= 4, std::format("enough cultures came out ({})", culture_count));
    check(cultured > 0, "cultures claimed habitable land");
    check(labels_are_connected(line, "culture", count, culture_count), "every culture is connected");
    check(size_t(field_value(line, "state", "event_count", 0)) >= culture_count,
          "history recorded at least the founding of every culture");
  }

  // 10. Число потоков не влияет ни на один буфер.
  //
  // Проверка идёт на МЕНЬШЕЙ планете, и это не послабление: детерминизм разбиения от разрешения не
  // зависит вовсе — гонка либо есть в инструменте, либо её нет, — а прогонов здесь четыре (эталон и
  // три числа потоков), и на полном разрешении они одни занимали бы больше времени, чем все прочие
  // сорок семь проверок вместе. Остальные проверки остаются на заказанном разрешении, потому что
  // ИХ свойства от него как раз зависят: острова физического происхождения на грубой решётке просто
  // не помещаются.
  {
    // Меняется ТОЛЬКО число клеток. Числа областей и культур трогать нельзя: они уже вошли в описание
    // пайплайна как значения шагов, а размеры буферов считаются из них же — уменьшить одно и не
    // уменьшить другое значит получить метку сверх объявленных корзин, что и случилось при первой
    // попытке (ключ 24 при 24 корзинах).
    auto threaded_options = opts;
    threaded_options.cells = std::min(opts.cells, size_t(16384));

    auto serial = generate(threaded_options, tools, description, nullptr);
    const auto reference = snapshot_all(*serial.line);
    for (const size_t threads : {size_t(1), size_t(3), size_t(7)}) {
      thread::atomic_pool pool(threads);
      auto parallel = generate(threaded_options, tools, description, &pool);
      check(snapshot_all(*parallel.line) == reference,
            std::format("parallel == serial in every buffer, threads {}", threads));
    }
  }

  // 8. Сетка просмотрщика. Проверяется здесь, а не глазами, по той же причине, по которой проверяется
  // всё остальное: «похоже на планету» глаз оценит, а «сетка замкнута и адресует существующие
  // клетки» — нет. Дырка в сетке видна как чёрное пятно только при определённом повороте.
  {
    constexpr uint32_t subdivisions = 4;
    const auto mesh = gn02::build_surface(line, subdivisions);
    const size_t expected_triangles = 20u * size_t(std::pow(4.0, double(subdivisions)));

    check(mesh.size() == expected_triangles * 3,
          std::format("the icosphere produced {} triangles out of {}", mesh.size() / 3, expected_triangles));

    bool addressable = true;
    bool weights_sum = true;
    std::vector<uint8_t> touched(count, 0);
    for (const auto& vertex : mesh) {
      // Все ЧЕТЫРЕ клетки вершины обязаны существовать: сглаживание берёт их все, и одна нулевая
      // клетка среди четырёх — это цвет чужого места, размазанный по четверти веса.
      uint32_t total = 0;
      for (size_t k = 0; k < vertex.cells.size(); ++k) {
        addressable = addressable && vertex.cells[k] < count;
        if (vertex.cells[k] < count) {
          touched[vertex.cells[k]] = 1;
        }
        total += (vertex.weights >> (8 * k)) & 0xffu;
      }
      // Сумма весов ровно 255: шейдер на это опирается и заново не нормирует.
      weights_sum = weights_sum && total == 255;
      const double length = std::sqrt(double(vertex.direction.x) * vertex.direction.x +
                                      double(vertex.direction.y) * vertex.direction.y +
                                      double(vertex.direction.z) * vertex.direction.z);
      addressable = addressable && std::abs(length - 1.0) < 1e-4;
    }
    check(addressable, "every mesh vertex sits on the sphere and points at four real cells");
    check(weights_sum, "the packed blend weights of every mesh vertex sum to 255");

    // Замкнутость: сумма площадей треугольников равна площади сферы. Это единственная проверка,
    // которая ловит и дырку, и вывернутый треугольник, и вершину, уехавшую с поверхности.
    double area = 0.0;
    for (size_t i = 0; i + 2 < mesh.size(); i += 3) {
      const auto& a = mesh[i].direction;
      const auto& b = mesh[i + 1].direction;
      const auto& c = mesh[i + 2].direction;
      // Площадь сферического треугольника через избыток углов (формула Жирара): плоская площадь
      // занижает её тем сильнее, чем крупнее треугольник.
      const auto angle = [](const glm::vec3& p, const glm::vec3& q, const glm::vec3& r) {
        const glm::vec3 u = glm::normalize(glm::cross(p, q));
        const glm::vec3 v = glm::normalize(glm::cross(p, r));
        return std::acos(std::clamp(double(glm::dot(u, v)), -1.0, 1.0));
      };
      area += angle(a, b, c) + angle(b, c, a) + angle(c, a, b) - std::numbers::pi;
    }
    // Допуск выбран между двумя числами, а не «на глаз»: накопленная ошибка формулы на float-вершинах
    // измерена как 5.2e-06 от площади, а ОДИН потерянный треугольник из 5120 — это 1.95e-04. Порог
    // 5e-05 лежит между ними, поэтому проверка ловит дырку и не ловит арифметику.
    check(std::abs(area - 4.0 * std::numbers::pi) < 5e-5 * 4.0 * std::numbers::pi,
          std::format("the mesh tiles the sphere: area {:.9f} against 4pi = {:.9f}", area, 4.0 * std::numbers::pi));

    const size_t addressed = size_t(std::count(touched.begin(), touched.end(), uint8_t(1)));
    // Сетка на 5120 треугольников грубее 16384 клеток намеренно: проверка смотрит не на покрытие, а
    // на то, что выбор ближайшей клетки не сваливается в несколько клеток на всю планету.
    check(addressed >= mesh.size() / 6,
          std::format("nearest-cell choice is spread out: {} cells over {} vertices", addressed, mesh.size()));
  }

  // 8а. Геометрия клеток и ЧЕСТНОСТЬ КАРТЫ.
  //
  // Выбор области делает фрагмент: он находит ближайшую клетку и считает покрытие областей ядром
  // шириной около шага решётки. У такого выбора есть цена — область, у которой в окрестности пикселя
  // мало своих клеток, проигрывает окружению и ПРОПАДАЕТ С КАРТЫ. Верхняя граница ширины выведена из
  // требования «не пропадает никто»: восемь соседей на минимальном расстоянии дают
  // 8*(1 - 1/R^2)^2 < 1 при R < 1.244. Проверка мерит это на настоящей решётке при самой широкой
  // разрешённой ширине, а не на худшем мыслимом кольце.
  //
  // Проверять надо именно так, потому что глазом это не видно: одноклеточный остров, съеденный
  // ядром, выглядит как «его тут и не было», а не как дефект отрисовки. Первая версия ставила ширину
  // 1.5 «на глаз», и такие острова исчезали молча.
  {
    // Ширина берётся САМАЯ БОЛЬШАЯ из разрешённых окном (`max_kernel_width` в шейдере): сглаживание
    // границ настраивается кнопкой, поэтому проверять надо худший случай диапазона. Узкое ядро ни
    // клеток, ни соседей не теряет тем более.
    constexpr float kernel_width = 1.24f;
    const auto graph = gn02::build_cell_geometry(line, count);
    bool geometry_sane = true;
    for (size_t i = 0; i < count && geometry_sane; ++i) {
      const auto& record = graph[i];
      geometry_sane = geometry_sane && record.neighbour_count > 0 &&
                      record.neighbour_count <= record.neighbours.size();
      const double length = std::sqrt(double(record.direction.x) * record.direction.x +
                                      double(record.direction.y) * record.direction.y +
                                      double(record.direction.z) * record.direction.z);
      geometry_sane = geometry_sane && std::abs(length - 1.0) < 1e-4;
      float previous = 0.0f;
      for (uint32_t k = 0; k < record.neighbour_count; ++k) {
        const uint32_t other = record.neighbours[k];
        geometry_sane = geometry_sane && other < count && other != uint32_t(i);
        if (other >= count) {
          break;
        }
        // Порядок по расстоянию — не косметика: шейдер берёт ПЕРВОГО соседа как меру шага решётки.
        const float distance = glm::distance(record.direction, graph[other].direction);
        geometry_sane = geometry_sane && distance >= previous - 1e-6f;
        previous = distance;
      }
    }
    check(geometry_sane, "cell geometry lists real neighbours, sorted by distance, on the unit sphere");

    // ОБРЕЗАНИЕ КОЛЬЦА БЕЗВРЕДНО, и это надо проверять, а не предполагать. Запись клетки держит до
    // восьми соседей, а степень в графе доходит до двадцати четырёх: симметризация kNN добавляет
    // связи в тех местах, где решётка неоднородна. Отброшенный сосед — это отброшенный ВЕС, и если
    // он попадает внутрь ядра, то отрезается он не симметрично относительно границы (у дальней
    // области больше, чем у ближней), а это ровно тот дефект, из-за которого ядро выбрано
    // компактным, а не гауссовым.
    //
    // Проверяется поэтому не «степень мала», а то, что все отброшенные соседи лежат ЗА радиусом
    // ядра при самой широкой разрешённой ширине.
    const auto cell_offsets = field_of(line, "cell_offsets", "start");
    const auto cell_arcs = field_of(line, "cell_arcs", "cell");
    size_t dropped_inside = 0;
    size_t dropped_total = 0;
    for (size_t i = 0; i < count; ++i) {
      const auto first = size_t(cell_offsets.get(i));
      const auto last = size_t(cell_offsets.get(i + 1));
      if (last - first <= graph[i].neighbours.size()) {
        continue;
      }
      const glm::vec3 own = graph[i].direction;
      std::vector<float> ring;
      ring.reserve(last - first);
      for (size_t k = first; k < last; ++k) {
        const auto other = size_t(cell_arcs.get(k));
        if (other < count) {
          ring.push_back(glm::distance(own, graph[other].direction));
        }
      }
      std::sort(ring.begin(), ring.end());
      const float spacing = std::max(ring.front(), 1e-6f);
      for (size_t k = graph[i].neighbours.size(); k < ring.size(); ++k) {
        ++dropped_total;
        dropped_inside += ring[k] < spacing * kernel_width ? 1 : 0;
      }
    }
    check(dropped_inside == 0,
          std::format("the eight-neighbour cut drops nothing the kernel would weigh ({} of {} dropped arcs "
                      "sit inside the widest kernel)",
                      dropped_inside, dropped_total));


    const auto hidden_cells = [&](const std::string_view field_name) {
      const auto labels = field_of(line, "cells", field_name);
      const auto label_of = [&](const uint32_t cell) { return labels.get(cell); };
      size_t hidden = 0;
      for (size_t i = 0; i < count; ++i) {
        // В СВОЁМ ЖЕ центре клетка обязана побеждать: иначе её область в этом месте не нарисована.
        const uint32_t winner = gn02::map_winner(std::span<const gn02::cell_geometry>(graph), uint32_t(i),
                                                 graph[i].direction, kernel_width, label_of);
        hidden += labels.get(winner) == labels.get(i) ? 0 : 1;
      }
      return hidden;
    };

    // Массивы суши — самые мелкие области: одноклеточный остров тут законен и назван.
    const size_t hidden_masses = hidden_cells("land_mass");
    const size_t hidden_provinces = hidden_cells("province");
    check(hidden_masses == 0 && hidden_provinces == 0,
          std::format("no cell is hidden by the map kernel ({} land-mass cells, {} province cells)",
                      hidden_masses, hidden_provinces));
  }

  // 9. Пакет: он и есть результат, поэтому круг «записали — прочитали — сошлось» обязателен.
  {
    const auto section_names = split_words(description.values.string("package"));
    check(!section_names.empty(), "the config lists the package sections");

    const auto written = gn02::build_package(line, section_names, opts.seed, count);
    const auto path = fs::temp_directory_path() / std::format("gn02_verify_{}.planet", opts.seed);
    gn02::write_package(written, path);
    const auto read_back = gn02::read_package(path);

    bool sections_match = read_back.sections.size() == written.sections.size();
    for (size_t i = 0; sections_match && i < written.sections.size(); ++i) {
      sections_match = written.sections[i].name == read_back.sections[i].name &&
                       written.sections[i].count == read_back.sections[i].count &&
                       written.sections[i].bytes == read_back.sections[i].bytes;
    }
    check(sections_match, "the package reads back section by section");
    check(read_back.fingerprint == written.fingerprint && read_back.seed == opts.seed,
          "package fingerprint and seed match");

    // Тот же мир из того же зерна обязан дать тот же отпечаток, а другое зерно — другой. Первое
    // проверяет воспроизводимость, второе — что зерно вообще доехало до формул.
    //
    // Обе идут на МЕНЬШЕЙ планете и по той же причине, что и проверка потоков: воспроизводимость от
    // разрешения не зависит, а это ещё три полных прогона. Меняется только число клеток — числа
    // областей и культур уже вошли в описание пайплайна, и рассогласовать их с размерами буферов
    // значит получить метку сверх объявленных корзин.
    auto twin_options = opts;
    twin_options.cells = std::min(opts.cells, size_t(16384));
    auto twin_reference = generate(twin_options, tools, description, nullptr);
    const auto twin_written =
      gn02::build_package(*twin_reference.line, section_names, twin_options.seed, twin_options.cells);

    auto twin = generate(twin_options, tools, description, nullptr);
    const auto twin_package = gn02::build_package(*twin.line, section_names, twin_options.seed, twin_options.cells);
    check(twin_package.fingerprint == twin_written.fingerprint, "the same seed gives the same fingerprint");

    auto other_options = twin_options;
    other_options.seed = opts.seed + 1;
    auto other = generate(other_options, tools, load_description(other_options), nullptr);
    const auto other_package =
      gn02::build_package(*other.line, section_names, other_options.seed, other_options.cells);
    check(other_package.fingerprint != twin_written.fingerprint, "a different seed gives a different fingerprint");

    fs::remove(path);
  }

  std::cout << "GN02 verify: " << (checks - failures) << "/" << checks << "\n";
  return failures == 0 ? 0 : 1;
}

// Просмотрщик получает не пайплайн, а СПОСОБ его пересчитать: и смена шага, и смена зерна, и правка
// настройки — это новый прогон генератора, а не переключение отображения. Владеть миром должен тот,
// кто его считает, поэтому наружу уходит функция, а не данные.
int run_view(const options& opts) {
  originator::tool_registry tools;
  tools.add_standard_tools();
  tools.add_graph_tools();
  originator::add_all_primitives(tools);
  gn02::add_planet_tools(tools);

  const size_t threads = opts.threads == 0
                           ? std::max<size_t>(std::thread::hardware_concurrency(), 1) - 1
                           : opts.threads;
  auto pool = std::make_unique<thread::atomic_pool>(threads);

  // Настраиваемые значения приходят из ТОГО ЖЕ документа, что и сами значения: границы и шаг объявлены
  // рядом с числом, поэтому список настроек не нужно поддерживать вторым местом в C++.
  const auto& ranges = generator().config.ranges;
  const auto base_description = load_description(opts);
  std::vector<gn02::tunable_value> tunables;
  tunables.reserve(ranges.size());
  for (const auto& range : ranges) {
    if (!base_description.values.has(range.name)) {
      utils::error{}("GN02: the values document declares a range for '{}', but no such value", range.name);
    }
    tunables.push_back(gn02::tunable_value{range, range.clamp(base_description.values.number(range.name))});
  }

  const auto regenerate = [&](const gn02::generation_request& request) {
    auto local = opts;
    local.seed = request.seed;

    auto description = load_description(local);
    for (const auto& [name, value] : request.overrides) {
      description.values.set_number(name, value);
    }
    // Числа, которые host обязан пересчитать сам: их берут из значений, а не из командной строки.
    local.provinces = size_t(std::max(1.0, description.values.number("province_count", double(local.provinces))));
    local.continent_min_provinces = size_t(std::max(
      1.0, description.values.number("continent_min_provinces", double(local.continent_min_provinces))));
    local.ocean_zones = size_t(std::max(1.0, description.values.number("ocean_zones", double(local.ocean_zones))));
    local.sea_zones = size_t(std::max(1.0, description.values.number("sea_zone_count", double(local.sea_zones))));
    local.cultures = size_t(std::max(1.0, description.values.number("culture_count", double(local.cultures))));

    auto produced = generate(local, tools, description, threads == 0 ? nullptr : pool.get(), request.step_limit);

    gn02::generated_world result;
    result.milliseconds = produced.milliseconds;
    result.executed_steps = produced.steps.size();
    result.seed = local.seed;
    result.step_names.reserve(produced.line->step_count());
    for (size_t i = 0; i < produced.line->step_count(); ++i) {
      result.step_names.push_back(produced.line->step_at(i).name);
    }
    result.line = std::move(produced.line);
    return result;
  };

  auto viewer = opts.viewer;
  viewer.seed = opts.seed;
  viewer.continent_min_provinces = size_t(std::max(
    1.0, base_description.values.number("continent_min_provinces", double(opts.continent_min_provinces))));
  viewer.ocean_zones =
    size_t(std::max(1.0, base_description.values.number("ocean_zones", double(opts.ocean_zones))));
  return gn02::run_viewer(viewer, std::move(tunables), regenerate);
}

int run_once(options opts) {
  originator::tool_registry tools;
  tools.add_standard_tools(); // включает scatter-инструменты
  tools.add_graph_tools();
  originator::add_all_primitives(tools);
  gn02::add_planet_tools(tools);

  const auto description = load_description(opts);
  // Пороги географии приходят из конфига: в options они только для того, чтобы синтез названий не
  // спрашивал их у собранного пайплайна, где значений конфига уже нет.
  opts.continent_min_provinces = size_t(std::max(
    1.0, description.values.number("continent_min_provinces", double(opts.continent_min_provinces))));
  opts.ocean_zones = size_t(std::max(1.0, description.values.number("ocean_zones", double(opts.ocean_zones))));

  const size_t threads = opts.threads == 0
                           ? std::max<size_t>(std::thread::hardware_concurrency(), 1) - 1
                           : opts.threads;
  thread::atomic_pool pool(threads);

  auto result = generate(opts, tools, description, threads == 0 ? nullptr : &pool);

  if (opts.report) {
    print_report(*result.line, opts, result.milliseconds);
    print_step_times(result);
  }
  if (!opts.stats.empty()) {
    print_field_stats(*result.line, opts, opts.stats);
  }
  if (opts.map) {
    print_map(*result.line, opts);
  }

  if (!opts.dump.empty()) {
    const auto section_names = split_words(description.values.string("package"));
    const auto value = gn02::build_package(*result.line, section_names, opts.seed, opts.cells);
    gn02::write_package(value, opts.dump);
    if (!opts.quiet) {
      std::cout << "  package             " << opts.dump.string() << ", fingerprint "
                << std::format("{:#018x}", value.fingerprint) << "\n";
    }
  }

  return 0;
}

} // namespace

int main(const int argc, const char** argv) {
  try {
    const auto opts = parse_options(argc, argv);
    if (opts.verify) {
      return run_verify(opts);
    }
    if (opts.view) {
      return run_view(opts);
    }
    return run_once(opts);
  } catch (const std::exception& error) {
    std::cerr << "GN02: " << error.what() << "\n";
    return 1;
  }
}
