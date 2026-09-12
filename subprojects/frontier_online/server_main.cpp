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

#include <algorithm>
#include <array>
#include <ranges>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <devils_engine/network/state_digest.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/timeline.h>

#include "core/actor_checkpoint.h"
#include "core/actor_simulation.h"
#include "core/causal_content.h"
#include "core/terrain.h"

#ifdef FRONTIER_ONLINE_HAS_NETWORK
#  include "net/authority.h"
#  include "net/follower.h"
#endif

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
  // Проба земли: посчитать квадрат radius x radius чанков и напечатать их корень. Земля по сети не
  // едет, поэтому единственный способ узнать, что две стороны видят один мир, — сверить этот
  // корень; здесь же проверяется само право так делать (чанк не зависит от порядка вычисления).
  uint32_t terrain_probe = 0;
  uint32_t chunk_size = 16;
  // Зерно мира. Раньше его роль играл --ticks, и это путало ровно там, где путать нельзя:
  // земля — функция зерна, а число тиков к ней отношения не имеет.
  uint64_t world_seed = 20260911;
  std::string generator = "generator/world";
  std::string resource_root = FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT;
  std::string dump_path;     // куда положить канонический документ состояния (для сверки байтами)
  // Смесь стартовых префабов: i-й актор = cycle[i % size]. Ключ существует потому, что состав
  // мира РЕШАЕТ, какие пути симуляции вообще исполнятся: без prey в мире некого есть, и весь
  // арбитраж поедания остаётся непройденным.
  std::vector<std::string> prefab_cycle{"prey", "prey", "prey", "actor"};

  // ─── сеть ───
  // Три режима, взаимоисключающие: считать в одиночку (по умолчанию), слушать, присоединиться.
  bool listen = false;
  uint16_t listen_port = 0;   // 0 при --listen без числа — порт спрашивается у ОС
  std::string join;           // "host:port"
  uint32_t expect_joins = 1;  // сколько присоединений ждёт авторитет, прежде чем выйти
  uint32_t timeout_s = 30;    // предел ожидания; молчаливое зависание — не результат
  // Испортить одно поле совместимости. Существует ради проверки, что отказ приходит С ПРИЧИНОЙ.
  std::string break_compat;
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
    if (const auto v = value_of("--terrain="); !v.empty()) {
      if (!parse_u64(v, number) || number == 0 || number > 64) return false;
      out.terrain_probe = uint32_t(number);
      continue;
    }
    if (const auto v = value_of("--world-seed="); !v.empty()) {
      if (!parse_u64(v, number)) return false;
      out.world_seed = number;
      continue;
    }
    if (const auto v = value_of("--chunk-size="); !v.empty()) {
      if (!parse_u64(v, number) || number == 0 || number > 256) return false;
      out.chunk_size = uint32_t(number);
      continue;
    }
    if (const auto v = value_of("--generator="); !v.empty()) {
      out.generator = std::string(v);
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
    if (arg == "--listen") {
      out.listen = true;
      continue;
    }
    if (const auto v = value_of("--listen="); !v.empty()) {
      if (!parse_u64(v, number) || number == 0 || number > 65535) return false;
      out.listen = true;
      out.listen_port = uint16_t(number);
      continue;
    }
    if (const auto v = value_of("--join="); !v.empty()) {
      out.join = std::string(v);
      continue;
    }
    if (const auto v = value_of("--expect-joins="); !v.empty()) {
      if (!parse_u64(v, number) || number > 64) return false;
      out.expect_joins = uint32_t(number);
      continue;
    }
    if (const auto v = value_of("--timeout="); !v.empty()) {
      if (!parse_u64(v, number) || number == 0 || number > 3600) return false;
      out.timeout_s = uint32_t(number);
      continue;
    }
    if (const auto v = value_of("--break-compat="); !v.empty()) {
      if (v != "protocol" && v != "content") return false;
      out.break_compat = std::string(v);
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

// ПРОБА ЗЕМЛИ. Отвечает на три вопроса, и все три нужны раньше, чем сеть.
//
//   1. Считается ли земля вообще без окна и без единой текстуры (она причинная, значит обязана).
//   2. Чанк — чистая функция ключа: тот же ключ даёт те же байты.
//   3. Чанк не зависит от ПОРЯДКА вычисления. Это обещание originator, и проверить его надо на
//      своём генераторе: обратный обход обязан дать тот же мир, иначе два игрока, идущие с разных
//      сторон, увидят разную землю на одном и том же месте.
//
// Корень печатается, чтобы его можно было сверить между процессами: земля по сети не едет, и это
// единственный способ убедиться, что стороны считают один мир.
int probe_terrain(const options& opts, const fo::causal_content& content) {
  fo::terrain_source source(content.resources(), opts.generator, opts.chunk_size,
                            opts.world_seed);

  const int32_t side = int32_t(opts.terrain_probe);
  const auto digest_of = [](const std::vector<fo::tile_chunk>& chunks) {
    std::string bytes;
    for (const auto& chunk : chunks) {
      for (const auto& t : chunk.tiles) bytes.push_back(char(t.terrain));
    }
    return utils::murmur_hash64A(bytes);
  };

  std::vector<fo::tile_chunk> forward;
  std::array<uint64_t, 4> histogram{};
  // Диапазоны СЫРЫХ полей. Именно они отвечают, верен ли фиксированный перевод шума в [0,1]:
  // «воды нет» — это утверждение не про порог, а про то, доходит ли до него поле.
  double height_min = 1e30, height_max = -1e30, moisture_min = 1e30, moisture_max = -1e30;
  std::vector<double> height_samples, moisture_samples;
  forward.reserve(size_t(side) * side);
  for (int32_t y = 0; y < side; ++y) {
    for (int32_t x = 0; x < side; ++x) {
      forward.push_back(source.generate(fo::chunk_coord{x, y}));
      for (const auto& t : forward.back().tiles) {
        if (t.terrain < histogram.size()) ++histogram[t.terrain];
      }
      for (size_t i = 0; i < forward.back().tiles.size(); ++i) {
        const double h = source.raw_field("height", i);
        const double m = source.raw_field("moisture", i);
        height_min = std::min(height_min, h);
        height_max = std::max(height_max, h);
        moisture_min = std::min(moisture_min, m);
        moisture_max = std::max(moisture_max, m);
        height_samples.push_back(h);
        moisture_samples.push_back(m);
      }
    }
  }

  // Обратный обход тем же источником: порядок другой, мир обязан быть тем же.
  std::vector<fo::tile_chunk> backward(forward.size());
  for (int32_t y = side - 1; y >= 0; --y) {
    for (int32_t x = side - 1; x >= 0; --x) {
      backward[size_t(y) * side + x] = source.generate(fo::chunk_coord{x, y});
    }
  }

  const uint64_t forward_root = digest_of(forward);
  const uint64_t backward_root = digest_of(backward);
  const uint64_t tiles = uint64_t(side) * side * opts.chunk_size * opts.chunk_size;

  std::printf("terrain.generator = %s\n", opts.generator.c_str());
  std::printf("terrain.world_seed = %llu\n", static_cast<unsigned long long>(opts.world_seed));
  std::printf("terrain.chunk_size = %u\n", opts.chunk_size);
  std::printf("terrain.chunks = %u x %u\n", opts.terrain_probe, opts.terrain_probe);
  std::printf("terrain.tiles = %llu\n", static_cast<unsigned long long>(tiles));
  std::printf("terrain.world_fingerprint = %llu\n",
              static_cast<unsigned long long>(source.fingerprint()));
  std::printf("terrain.root = %llu\n", static_cast<unsigned long long>(forward_root));
  std::printf("terrain.height_range = [%.4f, %.4f]\n", height_min, height_max);
  std::printf("terrain.moisture_range = [%.4f, %.4f]\n", moisture_min, moisture_max);
  // ДЕЦИЛИ, а не только края. Порог задаёт ДОЛЮ («сколько воды»), и подобрать его по диапазону
  // нельзя: у поля с длинным хвостом край говорит о редком выбросе, а не о том, где лежит масса.
  const auto deciles = [](std::vector<double>& samples, const char* label) {
    if (samples.empty()) return;
    std::sort(samples.begin(), samples.end());
    std::printf("terrain.%s_deciles =", label);
    for (int q = 1; q <= 9; ++q) {
      std::printf(" %.3f", samples[samples.size() * size_t(q) / 10]);
    }
    std::printf("\n");
  };
  deciles(height_samples, "height");
  deciles(moisture_samples, "moisture");
  for (size_t i = 0; i < histogram.size(); ++i) {
    std::printf("terrain.class.%zu = %llu (%.1f%%)\n", i,
                static_cast<unsigned long long>(histogram[i]),
                tiles == 0 ? 0.0 : 100.0 * double(histogram[i]) / double(tiles));
  }

  if (forward_root != backward_root) {
    std::fprintf(stderr,
                 "frontier_online_server: terrain depends on generation ORDER (%llu vs %llu)\n",
                 static_cast<unsigned long long>(forward_root),
                 static_cast<unsigned long long>(backward_root));
    return EXIT_FAILURE;
  }
  // Весь мир из одного класса — это не «мир», а сорванная калибровка порогов: генератор
  // отработал, картинки нет. Молчать об этом нельзя, иначе оно доедет до экрана.
  const auto used = std::ranges::count_if(histogram, [](const uint64_t n) { return n != 0; });
  if (used < 2) {
    std::fprintf(stderr, "frontier_online_server: terrain is uniform, %lld class(es) in use\n",
                 static_cast<long long>(used));
    return EXIT_FAILURE;
  }

  std::printf("frontier_online_server: terrain ok\n");
  return EXIT_SUCCESS;
}


#ifdef FRONTIER_ONLINE_HAS_NETWORK

namespace fnet = frontier_online::net;

// Снимок живого причинного состояния. Считается ровно тем же кодом, что печатает `state.root` в
// одиночном режиме: два разных способа получить «тот же» корень означали бы, что сравниваются две
// реализации хеша, а не два состояния мира.
class live_checkpoint final : public fnet::checkpoint_source {
public:
  live_checkpoint(const fo::actor_world_slice& actors, const utils::timelines& clocks,
                  const uint64_t& tick) noexcept
    : actors_(actors), clocks_(clocks), tick_(tick) {}

  bool capture(fnet::checkpoint_begin& header, std::vector<std::byte>& document) override {
    fo::actor_checkpoint_buffers buffers;
    network::state_digest_report<uint64_t> digest;
    digest.sections.reserve(fo::actor_checkpoint_section_count);
    if (!fo::actor_checkpoint_digest(actors_, clocks_, buffers, digest)) return false;
    if (buffers.document.size() > UINT32_MAX) return false;
    header.tick = tick_;
    header.root = digest.root;
    header.total_bytes = uint32_t(buffers.document.size());
    header.schema_fingerprint = fo::actor_checkpoint_schema_fingerprint();
    document = std::move(buffers.document);
    return true;
  }

private:
  const fo::actor_world_slice& actors_;
  const utils::timelines& clocks_;
  const uint64_t& tick_;
};

// Приём состояния на стороне присоединяющегося: загрузить в СВОЙ мир и посчитать корень заново.
class live_join final : public fnet::checkpoint_sink {
public:
  live_join(fo::actor_world_slice& actors, utils::timelines& clocks, const fo::brain_config& brains)
    : actors_(actors), clocks_(clocks), brains_(brains) {}

  bool load(const fnet::checkpoint_begin& header, const std::span<const std::byte> document,
            uint64_t& root) override {
    if (header.schema_fingerprint != fo::actor_checkpoint_schema_fingerprint()) return false;
    if (!fo::load_actor_checkpoint(actors_, clocks_, document, brains_).loaded()) return false;
    fo::actor_checkpoint_buffers buffers;
    network::state_digest_report<uint64_t> digest;
    digest.sections.reserve(fo::actor_checkpoint_section_count);
    if (!fo::actor_checkpoint_digest(actors_, clocks_, buffers, digest)) return false;
    root = digest.root;
    return true;
  }

private:
  fo::actor_world_slice& actors_;
  utils::timelines& clocks_;
  const fo::brain_config& brains_;
};

// АВТОРИТЕТ СЛУШАЕТ. Симуляция идёт своим ходом, обслуживание сети — раз в тик. Выход по числу
// состоявшихся присоединений либо по пределу ожидания: процесс, который ждёт молча и вечно, —
// это не стенд, а зависание.
int serve_session(const options& opts, const fo::causal_content& content) {
  const auto terrain = content.make_terrain(opts.generator, opts.chunk_size, opts.world_seed);
  const auto world = fnet::declare_world(*terrain, opts.generator);

  fnet::net_runtime runtime(1);
  fnet::authority_config config;
  // Объявленный порт — для прогона, где участники на других машинах: слушаем на всех интерфейсах.
  // Порт «какой дадут» — это локальный опыт, и уезжать за пределы петли ему незачем.
  config.port = opts.listen_port;
  config.host = opts.listen_port == 0 ? 0x7f000001u : 0u;
  config.max_peers = opts.expect_joins == 0 ? 8 : opts.expect_joins;
  config.verbose = !opts.verify;
  fnet::authority authority(runtime, content, world, config);

  std::printf("listen.port = %u\n", authority.port());
  std::printf("listen.session = %llu\n", static_cast<unsigned long long>(authority.session()));
  std::printf("listen.content_root = %s\n",
              fo::short_digest(content.manifest().root).c_str());
  std::printf("listen.content_files = %zu (%zu bytes)\n", content.manifest().files,
              content.manifest().bytes);
  std::printf("listen.world_fingerprint = %llu\n",
              static_cast<unsigned long long>(world.generator_fingerprint));
  std::printf("listen.world_probe = %llu\n", static_cast<unsigned long long>(world.probe_root));
  std::fflush(stdout);

  const utils::simulation_rate rate(opts.rate);
  utils::timelines clocks(rate);
  thread::atomic_pool pool(opts.workers);

  fo::actor_world_slice actors;
  actors.init(opts.actors, {0.5f, 0.5f}, {opts.extent, opts.extent}, opts.textures,
              content.config(), opts.prefab_cycle);

  uint64_t tick_counter = 0;
  live_checkpoint source(actors, clocks, tick_counter);

  const auto tick_period = std::chrono::nanoseconds(std::chrono::seconds(1)) / opts.rate;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opts.timeout_s);
  auto next_tick = std::chrono::steady_clock::now() + tick_period;

  while (true) {
    const auto tick = clocks.simulation_now() + utils::simulation_duration{1};
    const auto metrics = actors.update(tick, clocks.advance_simulation(tick), nullptr, pool);
    tick_counter = metrics.ticks;

    runtime.pump();
    authority.step(source);

    if (authority.completed_joins() >= opts.expect_joins &&
        authority.connected_peers() == 0) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    // Авторитет ВЫДЕРЖИВАЕТ такт всегда, а не только по --paced: он ждёт живого участника, и
    // прокрутить за это время миллион тиков означало бы сжечь ядро вместо ожидания.
    std::this_thread::sleep_until(next_tick);
    next_tick += tick_period;
  }

  const auto joins = authority.joins();
  std::printf("listen.ticks = %llu\n", static_cast<unsigned long long>(tick_counter));
  std::printf("listen.joins = %u of %u\n", authority.completed_joins(), opts.expect_joins);
  std::printf("listen.refused = %u\n", authority.refused_joins());
  std::printf("listen.checkpoint_bytes_sent = %llu\n",
              static_cast<unsigned long long>(authority.checkpoint_bytes_sent()));
  for (const auto& join : joins) {
    std::printf("listen.join.%llu = %s, tick %llu, root %llu\n",
                static_cast<unsigned long long>(join.peer_id), fnet::describe(join.report.status),
                static_cast<unsigned long long>(join.report.tick),
                static_cast<unsigned long long>(join.report.root));
  }
  authority.close_all();

  if (authority.completed_joins() < opts.expect_joins) {
    std::fprintf(stderr, "frontier_online_server: %u of %u joins completed\n",
                 authority.completed_joins(), opts.expect_joins);
    return EXIT_FAILURE;
  }
  std::printf("frontier_online_server: authority ok\n");
  return EXIT_SUCCESS;
}

// КЛИЕНТ ПРИСОЕДИНЯЕТСЯ. Здесь он безглазый нарочно: сравнивается ПРИЧИННОЕ состояние, и окно к
// этому сравнению ничего не добавляет.
int join_session(const options& opts, const fo::causal_content& content) {
  uint32_t host = 0;
  uint16_t port = 0;
  if (!fnet::parse_endpoint(opts.join, host, port)) {
    std::fprintf(stderr, "frontier_online_server: bad --join endpoint '%s'\n", opts.join.c_str());
    return EXIT_FAILURE;
  }

  fnet::net_runtime runtime(1);
  fnet::follower_config config;
  config.host = host;
  config.port = port;
  config.verbose = !opts.verify;
  if (opts.break_compat == "protocol") config.protocol_version_override = fnet::protocol_version + 1;
  if (opts.break_compat == "content") config.corrupt_content_root = true;

  fnet::follower follower(runtime, content, config);
  std::printf("join.content_root = %s\n", fo::short_digest(content.manifest().root).c_str());
  std::fflush(stdout);
  if (!follower.start()) return EXIT_FAILURE;

  utils::timelines clocks(utils::simulation_rate(opts.rate));
  fo::actor_world_slice actors;
  live_join sink(actors, clocks, content.config());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opts.timeout_s);
  while (!follower.finished()) {
    runtime.pump();
    follower.step(sink);
    if (std::chrono::steady_clock::now() >= deadline) {
      std::fprintf(stderr, "frontier_online_server: join timed out in phase %u\n",
                   unsigned(follower.phase()));
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // Дослать отчёт. Выход сразу после «присоединился» унёс бы его с собой.
  const auto flush_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  bool acknowledged = false;
  while (!acknowledged && std::chrono::steady_clock::now() < flush_until) {
    runtime.pump();
    acknowledged = follower.drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  const auto& report = follower.report();
  std::printf("join.acknowledged = %s\n", acknowledged ? "yes" : "no");
  std::printf("join.session = %llu\n", static_cast<unsigned long long>(follower.session()));
  std::printf("join.peer = %llu\n", static_cast<unsigned long long>(follower.peer_id()));
  std::printf("join.status = %s\n", fnet::describe(report.status));
  std::printf("join.refusal = %s\n", fnet::describe(follower.refusal()));
  std::printf("join.tick = %llu\n", static_cast<unsigned long long>(report.tick));
  std::printf("join.root = %llu\n", static_cast<unsigned long long>(report.root));
  std::printf("join.world_fingerprint = %llu\n",
              static_cast<unsigned long long>(report.world_fingerprint));
  std::printf("join.world_probe = %llu\n", static_cast<unsigned long long>(report.probe_root));
  std::printf("join.state_bytes = %u of %u\n", follower.received_bytes(),
              follower.expected_bytes());

  // Нарочно испорченная совместимость обязана дать ИМЕННО свой отказ. Присоединение в этом режиме
  // — провал: проверяется, что «другая сборка» не значит «молча не работает».
  if (!opts.break_compat.empty()) {
    const auto expected = opts.break_compat == "protocol"
                            ? network::session_refusal_reason::protocol_version_mismatch
                            : network::session_refusal_reason::content_mismatch;
    if (follower.refusal() != expected) {
      std::fprintf(stderr, "frontier_online_server: expected refusal '%s', got '%s'\n",
                   fnet::describe(expected), fnet::describe(follower.refusal()));
      return EXIT_FAILURE;
    }
    std::printf("frontier_online_server: refused as declared\n");
    return EXIT_SUCCESS;
  }

  if (follower.phase() != fnet::follower_phase::joined) {
    std::fprintf(stderr, "frontier_online_server: join failed: %s\n", fnet::describe(report.status));
    return EXIT_FAILURE;
  }
  std::printf("frontier_online_server: join ok\n");
  return EXIT_SUCCESS;
}

#endif

} // namespace

int main(int argc, char** argv) {
  options opts;
  if (!parse_options(argc, argv, opts)) return EXIT_FAILURE;

  fo::causal_content content(opts.resource_root);

  if (opts.terrain_probe != 0) {
    return probe_terrain(opts, content);
  }

  if (opts.listen && !opts.join.empty()) {
    std::fprintf(stderr, "frontier_online_server: --listen and --join are exclusive\n");
    return EXIT_FAILURE;
  }
#ifdef FRONTIER_ONLINE_HAS_NETWORK
  // Занятый порт, недостижимый адрес, отсутствующий генератор — это обычные обстоятельства
  // оператора, а не нарушение инварианта. Сообщение и код возврата, а не terminate.
  try {
    if (opts.listen) return serve_session(opts, content);
    if (!opts.join.empty()) return join_session(opts, content);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "frontier_online_server: %s\n", error.what());
    return EXIT_FAILURE;
  }
#else
  if (opts.listen || !opts.join.empty()) {
    std::fprintf(stderr, "frontier_online_server: built without the GNS backend\n");
    return EXIT_FAILURE;
  }
#endif

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
