// frontier_online — headless-авторитет (SERVER-01).
//
// Отдельный процесс, который выполняет ПОЛНУЮ причинную симуляцию и не имеет ни окна, ни Vulkan,
// ни звука, ни UI. Список библиотек в CMakeLists у этой цели — одна строка: `frontier_causal`.
// Поэтому цель работает и как исполняемый файл, и как ПРОВЕРКА раскола: если презентационная
// зависимость снова просочится в причинный код, сломается сборка, а не поведение в бою.
//
// Тик здесь фиксированный и НЕ выводится из настенного времени: у авторитета нет кадров, которые
// надо успевать рисовать, поэтому он считает объявленное число тиков так быстро, как может
// (`--rate` включает реальную выдержку, когда нужно смотреть на живой процесс). Настенное время
// остаётся только источником пейсинга, ровно как это зафиксировал TIME-02.
//
// Наблюдаемый результат среза: авторитет доходит до объявленного тика и печатает КОРЕНЬ своего
// причинного состояния. Тот же корень на том же зерне обязан получиться у клиента и у любой
// другой сборки — это и есть общая основа, на которую потом ложится сеть.

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <devils_engine/acumen/goap_resource.h>
#include <devils_engine/act/script_resource.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/mood/fsm_resource.h>
#include <devils_engine/network/state_digest.h>
#include <devils_engine/prefab/resource.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/timeline.h>

#include "core/actor_checkpoint.h"
#include "core/actor_simulation.h"
#include "core/brain_config_loader.h"
#include "core/script_environment.h"

using namespace devils_engine;
namespace fo = frontier_online::core;

namespace {

#ifndef FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT
#  define FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT "resources/modules/"
#endif

struct options {
  uint64_t ticks = 600;      // сколько причинных тиков выполнить
  uint32_t rate = 60;        // тиков в секунду (логическая координата, не настенная)
  uint32_t actors = 512;     // стартовый спавн
  uint32_t workers = 4;      // размер пула
  uint32_t textures = 1;     // авторитету неоткуда взять палитру: слот всегда 0
  float extent = 64.0f;      // квадрат мира [0, extent]^2
  bool paced = false;        // выдерживать настоящий такт (для наблюдения за живым процессом)
  bool verify = false;       // режим проверки: печатать только итог и вернуть код
  bool trace = false;        // печатать корень КАЖДЫЙ тик: ищем ПЕРВЫЙ разошедшийся тик, а не
                             // финальный (итоговый корень говорит только, что расхождение было)
  std::string resource_root = FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT;
  std::string dump_path;     // куда положить канонический документ состояния (для сверки байтами)
  // Смесь стартовых префабов: i-й актор = cycle[i % size]. Ключ существует потому, что состав
  // мира РЕШАЕТ, какие пути симуляции вообще исполнятся: без prey в мире некого есть, и весь
  // арбитраж поедания остаётся непройденным.
  std::vector<std::string> prefab_cycle{"prey", "prey", "prey", "actor"};
};

bool parse_u64(const std::string_view text, uint64_t& out) {
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto result = std::from_chars(first, last, out);
  return result.ec == std::errc{} && result.ptr == last;
}

// Значение опции разбирается ЗДЕСЬ и отвергается сразу: авторитет, молча подставивший дефолт
// вместо непонятого аргумента, посчитал бы не тот мир и сообщил бы об этом корнем, который никто
// не сможет объяснить.
bool parse_options(const int argc, char** argv, options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto value_of = [&](const std::string_view name) -> std::string_view {
      if (!arg.starts_with(name)) return {};
      return arg.substr(name.size());
    };

    if (arg == "--verify") {
      out.verify = true;
      continue;
    }
    if (arg == "--paced") {
      out.paced = true;
      continue;
    }
    if (arg == "--trace") {
      out.trace = true;
      continue;
    }
    uint64_t number = 0;
    if (const auto v = value_of("--ticks="); !v.empty()) {
      if (!parse_u64(v, number)) return false;
      out.ticks = number;
      continue;
    }
    if (const auto v = value_of("--rate="); !v.empty()) {
      if (!parse_u64(v, number) || number == 0 || number > 1000) return false;
      out.rate = uint32_t(number);
      continue;
    }
    if (const auto v = value_of("--actors="); !v.empty()) {
      if (!parse_u64(v, number)) return false;
      out.actors = uint32_t(number);
      continue;
    }
    if (const auto v = value_of("--workers="); !v.empty()) {
      if (!parse_u64(v, number) || number > 64) return false;
      out.workers = uint32_t(number);
      continue;
    }
    if (const auto v = value_of("--dump="); !v.empty()) {
      out.dump_path = std::string(v);
      continue;
    }
    if (const auto v = value_of("--resources="); !v.empty()) {
      out.resource_root = std::string(v);
      continue;
    }
    if (arg.starts_with("--prefabs=")) {
      const auto list = arg.substr(std::string_view("--prefabs=").size());
      out.prefab_cycle.clear();
      for (size_t at = 0; at <= list.size();) {
        const auto comma = list.find(',', at);
        const auto end = comma == std::string_view::npos ? list.size() : comma;
        if (end > at) out.prefab_cycle.emplace_back(list.substr(at, end - at));
        if (comma == std::string_view::npos) break;
        at = comma + 1;
      }
      continue;
    }
    std::fprintf(stderr, "frontier_online_server: unknown argument '%.*s'\n",
                 int(arg.size()), arg.data());
    return false;
  }
  return true;
}

// Причинный контент: скрипт-предикат, наборы FSM/GOAP и префабы. Ровно то же дерево ресурсов,
// что читает клиент — авторитет НЕ имеет своей особой копии содержимого, иначе расхождение
// сборок было бы заложено в само устройство стенда.
class causal_content {
public:
  explicit causal_content(std::string resource_root) : modules_(std::move(resource_root)) {
    modules_.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});
    resources_.register_type<act::script_resource>("scripts", "tavl", &scripts_);
    resources_.register_type<mood::fsm_resource>("fsm", "tavl");
    resources_.register_type<acumen::goap_resource>("goap", "tavl", &scripts_);
    resources_.register_type<prefab::prefab_resource>("prefab", "tavl");
    resources_.parse_resources(&modules_);
    config_ = fo::load_required_brain_config(resources_, "scripts/actor_is_hungry", "fsm", "goap",
                                             "prefab/");
  }

  const fo::brain_config& config() const noexcept {
    return config_;
  }

private:
  fo::script_environment scripts_;
  demiurg::module_system modules_;
  demiurg::resource_system resources_;
  fo::brain_config config_;
};

} // namespace

int main(int argc, char** argv) {
  options opts;
  if (!parse_options(argc, argv, opts)) return EXIT_FAILURE;

  causal_content content(opts.resource_root);

  const utils::simulation_rate rate(opts.rate);
  utils::timelines clocks(rate);
  thread::atomic_pool pool(opts.workers);

  fo::actor_world_slice actors;
  actors.init(opts.actors, {0.5f, 0.5f}, {opts.extent, opts.extent}, opts.textures,
              content.config(), opts.prefab_cycle);

  if (!opts.verify) {
    std::printf("frontier_online authority: headless, no window/vulkan/audio/ui\n");
    std::printf("  ticks=%llu rate=%u actors=%u workers=%u\n",
                static_cast<unsigned long long>(opts.ticks), opts.rate, opts.actors, opts.workers);
  }

  const auto tick_period = std::chrono::nanoseconds(std::chrono::seconds(1)) / opts.rate;
  auto next_deadline = std::chrono::steady_clock::now() + tick_period;

  fo::actor_metrics last{};
  for (uint64_t i = 0; i < opts.ticks; ++i) {
    const auto tick = clocks.simulation_now() + utils::simulation_duration{1};
    // Слот хвоста тика пуст: у авторитета нет читателя-презентации, и это не урезанный режим,
    // а его нормальное устройство.
    last = actors.update(tick, clocks.advance_simulation(tick), nullptr, pool);

    if (opts.paced) {
      std::this_thread::sleep_until(next_deadline);
      next_deadline += tick_period;
    }
    if (opts.trace) {
      fo::actor_checkpoint_buffers step_buffers;
      network::state_digest_report<uint64_t> step;
      step.sections.reserve(fo::actor_checkpoint_section_count);
      if (fo::actor_checkpoint_digest(actors, clocks, step_buffers, step)) {
        std::printf("trace %llu %llu", static_cast<unsigned long long>(last.ticks),
                    static_cast<unsigned long long>(step.root));
        for (const auto& section : step.sections)
          std::printf(" %u:%llu", section.id, static_cast<unsigned long long>(section.root));
        std::printf(" actors=%u eating=%u\n", last.actors, last.eating);
      } else {
        std::printf("trace %llu refused\n", static_cast<unsigned long long>(last.ticks));
      }
    } else if (!opts.verify && last.ticks % 100 == 0) {
      std::printf("  tick=%llu actors=%u eating=%u\n",
                  static_cast<unsigned long long>(last.ticks), last.actors, last.eating);
    }
  }

  // Корень считается по КАНОНИЧНЫМ байтам полного причинного checkpoint'а, а не по чему-то
  // производному: HOT-01 отдельно записал, что дайджест нельзя брать с квантованных величин.
  // Секционные корни печатаются рядом с общим: при расхождении двух процессов первый вопрос —
  // КАКАЯ секция разошлась, и общий корень на него не отвечает.
  fo::actor_checkpoint_buffers buffers;
  network::state_digest_report<uint64_t> digest;
  digest.sections.reserve(fo::actor_checkpoint_section_count);
  if (!fo::actor_checkpoint_digest(actors, clocks, buffers, digest)) {
    std::fprintf(stderr, "frontier_online_server: causal checkpoint digest failed\n");
    return EXIT_FAILURE;
  }

  std::printf("state.tick = %llu\n", static_cast<unsigned long long>(last.ticks));
  std::printf("state.actors = %u\n", last.actors);
  std::printf("state.root = %llu\n", static_cast<unsigned long long>(digest.root));
  std::printf("state.checkpoint_bytes = %zu\n", buffers.document.size());
  std::printf("compat.state_schema_fingerprint = %u\n", fo::actor_checkpoint_schema_fingerprint());
  for (const auto& section : digest.sections) {
    std::printf("state.section.%u = %llu (%llu bytes)\n", section.id,
                static_cast<unsigned long long>(section.root),
                static_cast<unsigned long long>(section.canonical_size));
  }

  if (!opts.dump_path.empty()) {
    std::FILE* out = std::fopen(opts.dump_path.c_str(), "wb");
    if (out == nullptr) {
      std::fprintf(stderr, "frontier_online_server: cannot open '%s'\n", opts.dump_path.c_str());
      return EXIT_FAILURE;
    }
    std::fwrite(buffers.document.data(), 1, buffers.document.size(), out);
    std::fclose(out);
  }

  if (opts.verify) {
    // Проверка отвечает на один вопрос: авторитет дошёл до объявленного тика и его состояние
    // непусто. Сравнение корней между процессами — предмет следующего среза, здесь его негде
    // взять честно.
    if (last.ticks != opts.ticks) {
      std::fprintf(stderr, "frontier_online_server: reached tick %llu of %llu\n",
                   static_cast<unsigned long long>(last.ticks),
                   static_cast<unsigned long long>(opts.ticks));
      return EXIT_FAILURE;
    }
    if (last.actors == 0 || buffers.document.empty()) {
      std::fprintf(stderr, "frontier_online_server: authority produced an empty world\n");
      return EXIT_FAILURE;
    }
    if (last.instances != 0) {
      std::fprintf(stderr,
                   "frontier_online_server: authority reported %u presentation instances; the "
                   "tick tail must be empty without a reader\n",
                   last.instances);
      return EXIT_FAILURE;
    }
    std::printf("frontier_online_server: ok\n");
  }
  return EXIT_SUCCESS;
}
