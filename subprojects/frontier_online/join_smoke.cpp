// ПРИСОЕДИНЕНИЕ — регрессионный минимум.
//
// Два конца, настоящий транспорт по петле, ОДИН процесс и ОДИН поток. Это не упрощение ради
// удобства, а требование библиотеки: у GNS один диспетчер на интерфейс и один поток-владелец,
// поэтому два конца в одном процессе обязаны качаться из одного места. Зато код на пути ровно
// тот же, что у двух настоящих процессов, — отличается только, кто вызывает `pump()`.
//
// Что доказывается:
//   1. Присоединение доходит до конца, и корень причинного состояния у присоединившегося
//      СОВПАДАЕТ с авторитетским. Это и есть «тот же мир», а не «байты доехали».
//   2. Несовместимость даёт ИМЕННО СВОЮ причину до единого байта состояния. «Другая сборка» не
//      должна значить «молча не работает» — версия протокола и содержимое отвергаются порознь.
//
// Прогон двух настоящих процессов (и тем более двух машин) этим НЕ заменяется: он проверяет
// развёртывание, а это — механизм.
#include <chrono>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <thread>
#include <vector>

#include <devils_engine/network/state_digest.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/timeline.h>
#include <spdlog/spdlog.h>

#include "core/actor_checkpoint.h"
#include "core/actor_simulation.h"
#include "core/causal_content.h"
#include "net/actor_component_wire.h"
#include "net/authority.h"
#include "net/follower.h"

using namespace devils_engine;
namespace fo = frontier_online::core;
namespace fnet = frontier_online::net;

namespace {

int failures = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
      ++failures;                                             \
    }                                                         \
  } while (0)

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

struct case_result {
  bool joined = false;
  uint64_t follower_root = 0;
  uint64_t tick = 0;
  uint32_t state_bytes = 0;
  network::session_refusal_reason refusal = network::session_refusal_reason::none;
  fnet::join_status status = fnet::join_status::checkpoint_refused;
};

void check_actor_component_wire_contract() {
  namespace net = devils_engine::network;
  net::component_wire_table table;
  CHECK(fnet::declare_actor_components(table) == net::component_wire_status::ok);
  CHECK(table.entries().size() == 2);
  const auto position_decl = net::component_descriptor<fo::actor_position>();
  const auto state_decl = net::component_descriptor<fo::actor_state>();
  CHECK(table.find(position_decl.id) != nullptr);
  CHECK(table.find(state_decl.id) != nullptr);
  CHECK(position_decl.id != state_decl.id);
  CHECK(position_decl.layout_hash == utils::serial::detail::layout_hash<fo::actor_position>());
  CHECK(position_decl.max_value_bytes == 8);
  CHECK(state_decl.max_value_bytes == 8);

  auto reversed = std::array{state_decl, position_decl};
  net::component_wire_table other;
  CHECK(other.assign(reversed) == net::component_wire_status::ok);
  CHECK(other.fingerprint() == table.fingerprint());
  reversed[0].revision++;
  CHECK(other.assign(reversed) == net::component_wire_status::ok);
  CHECK(other.fingerprint() != table.fingerprint());
  CHECK(other.assign(std::array{reversed[0], reversed[0]}) ==
        net::component_wire_status::duplicate_id);
  CHECK(other.fingerprint() != table.fingerprint()); // failed assignment kept prior table
  reversed[0].max_value_bytes = net::component_value_absolute_max_bytes + 1;
  CHECK(other.assign(reversed) == net::component_wire_status::invalid_descriptor);
  table.freeze();
  CHECK(table.frozen());
  CHECK(table.assign(reversed) == net::component_wire_status::frozen);

  std::vector<std::byte> bytes;
  const fo::actor_position position{{12.25f, -3.5f}};
  fo::actor_position decoded_position;
  CHECK(net::encode_component_value(table, position, bytes));
  CHECK(bytes.size() == 8);
  CHECK(net::decode_component_value(table, bytes, decoded_position));
  CHECK(decoded_position.value.x == position.value.x);
  CHECK(decoded_position.value.y == position.value.y);
  auto nonfinite = bytes;
  nonfinite[0] = std::byte{0};
  nonfinite[1] = std::byte{0};
  nonfinite[2] = std::byte{0xc0};
  nonfinite[3] = std::byte{0x7f}; // canonical f32 quiet NaN
  CHECK(!net::decode_component_value(table, nonfinite, decoded_position));
  CHECK(decoded_position.value.x == position.value.x); // no partial publication
  CHECK(!net::decode_component_value(std::span<const std::byte>(bytes).first(7),
                                     decoded_position));
  CHECK(decoded_position.value.x == position.value.x);
  CHECK(!net::encode_component_value(position, bytes, 7));
  CHECK(bytes.empty()); // no partial value may be sent
  fo::actor_position invalid{{std::numeric_limits<float>::quiet_NaN(), 0.0f}};
  CHECK(!net::encode_component_value(invalid, bytes));

  const fo::actor_state state{UINT64_C(0x123456789abcdef0)};
  fo::actor_state decoded_state;
  CHECK(net::encode_component_value(table, state, bytes));
  CHECK(net::decode_component_value(table, bytes, decoded_state));
  CHECK(decoded_state.state == state.state);
  CHECK(!net::encode_component_value(table, fo::actor_velocity{{1.0f, 2.0f}}, bytes));
}

void check_checkpoint_transfer_contract() {
  fnet::checkpoint_begin header{1, 2, fnet::max_checkpoint_bytes, 3};
  std::vector<std::byte> bytes;
  CHECK(fnet::encode(header, bytes));
  fnet::checkpoint_begin decoded;
  CHECK(fnet::decode(bytes, decoded));
  CHECK(decoded.total_bytes == fnet::max_checkpoint_bytes);

  // A remote length larger than the document budget must fail before follower
  // allocates. The total_bytes field starts after type, tick and root.
  bytes[17] = std::byte{1};
  bytes[18] = std::byte{0};
  bytes[19] = std::byte{0};
  bytes[20] = std::byte{1};
  CHECK(!fnet::decode(bytes, decoded));
  CHECK(decoded.total_bytes == fnet::max_checkpoint_bytes);

  std::array<std::byte, 4> document{};
  const std::array<std::byte, 2> first{std::byte{1}, std::byte{2}};
  const std::array<std::byte, 2> last{std::byte{3}, std::byte{4}};
  uint32_t received = 0;
  CHECK(fnet::append_checkpoint_chunk(document, received, {0, first}));
  CHECK(received == 2);
  CHECK(!fnet::append_checkpoint_chunk(document, received, {0, first})); // duplicate
  CHECK(!fnet::append_checkpoint_chunk(document, received, {1, last})); // overlap
  CHECK(!fnet::append_checkpoint_chunk(document, received, {3, last})); // hole
  CHECK(received == 2);
  CHECK(document[2] == std::byte{0});
  CHECK(fnet::append_checkpoint_chunk(document, received, {2, last}));
  CHECK(received == document.size());
  CHECK(document[3] == std::byte{4});
}

// Один прогон присоединения. Мир авторитета тикает всё это время: присоединяющийся получает
// СНИМОК и не останавливает симуляцию.
case_result run_case(fnet::net_runtime& runtime, const fo::causal_content& authority_content,
                     const fo::causal_content& follower_content, const uint64_t world_seed,
                     const fnet::follower_config& follower_overrides,
                     const size_t actor_count = 128) {
  case_result result;

  const auto terrain = authority_content.make_terrain("generator/world", 16, world_seed);
  const auto world = fnet::declare_world(*terrain, "generator/world");

  fnet::authority_config config;
  config.port = 0; // порт у операционной системы: тест не имеет права драться за чужой номер
  config.host = 0x7f000001;
  config.max_peers = 2;
  config.verbose = false;
  fnet::authority authority(runtime, authority_content, world, config);

  utils::timelines clocks(utils::simulation_rate(60));
  thread::atomic_pool pool(2);
  fo::actor_world_slice actors;
  actors.init(actor_count, {0.5f, 0.5f}, {64.0f, 64.0f}, 1, authority_content.config(),
              {"prey", "prey", "prey", "actor"});
  uint64_t tick_counter = 0;
  live_checkpoint source(actors, clocks, tick_counter);

  auto follower_config = follower_overrides;
  follower_config.host = 0x7f000001;
  follower_config.port = authority.port();
  follower_config.verbose = false;
  fnet::follower follower(runtime, follower_content, follower_config);
  if (!follower.start()) return result;

  utils::timelines follower_clocks(utils::simulation_rate(60));
  fo::actor_world_slice follower_actors;
  live_join sink(follower_actors, follower_clocks, follower_content.config());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto tick = clocks.simulation_now() + utils::simulation_duration{1};
    tick_counter = actors.update(tick, clocks.advance_simulation(tick), nullptr, pool).ticks;

    runtime.pump();
    authority.step(source);
    follower.step(sink);

    if (authority.completed_joins() >= 1) break;
    if (follower.phase() == fnet::follower_phase::failed && authority.refused_joins() >= 1) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  result.refusal = follower.refusal();
  result.status = follower.report().status;
  result.follower_root = follower.report().root;
  result.tick = follower.report().tick;
  result.state_bytes = follower.received_bytes();
  if (!authority.joins().empty()) {
    result.joined = authority.joins().front().agreed;
  }
  // Сравнение корней сделал АВТОРИТЕТ, и его вывод лежит в `agreed`. Пересчитывать здесь было бы
  // не только лишним, но и неверным: мир с момента снимка ушёл вперёд, и «сравнить с текущим»
  // значило бы сравнить не то.
  authority.close_all();
  return result;
}

} // namespace

int main() {
  spdlog::set_level(spdlog::level::err); // ресурсные предупреждения здесь только мешают
  check_actor_component_wire_contract();
  check_checkpoint_transfer_contract();

  // ДВА НЕЗАВИСИМЫХ ЭКЗЕМПЛЯРА содержимого — как две установки. Общий объект скрыл бы ровно ту
  // беду, ради которой считается отпечаток.
  fo::causal_content authority_content(FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT);
  fo::causal_content follower_content(FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT);
  CHECK(authority_content.manifest().root == follower_content.manifest().root);
  CHECK(authority_content.manifest().files > 0);
  std::printf("causal content: %zu files, %zu bytes, root %s\n",
              authority_content.manifest().files, authority_content.manifest().bytes,
              fo::short_digest(authority_content.manifest().root).c_str());

  fnet::net_runtime runtime(8);

  {
    const auto result = run_case(runtime, authority_content, follower_content, 20260911, {});
    std::printf("join: %s, tick %llu, %u bytes, root %llu (%s)\n", result.joined ? "ok" : "FAILED",
                static_cast<unsigned long long>(result.tick), result.state_bytes,
                static_cast<unsigned long long>(result.follower_root),
                fnet::describe(result.status));
    CHECK(result.joined);
    CHECK(result.status == fnet::join_status::loaded);
    CHECK(result.state_bytes > 0);
    CHECK(result.tick > 0);
  }

  {
    const auto result = run_case(runtime, authority_content, follower_content, 20260911, {},
                                 1024);
    std::printf("large join: %s, tick %llu, %u bytes\n", result.joined ? "ok" : "FAILED",
                static_cast<unsigned long long>(result.tick), result.state_bytes);
    CHECK(result.joined);
    CHECK(result.status == fnet::join_status::loaded);
    CHECK(result.state_bytes > 100000);
    CHECK(result.state_bytes < fnet::max_checkpoint_bytes);
  }

  // Несовместимость обязана назваться. Проверяются два РАЗНЫХ поля, потому что «версия протокола»
  // и «другие правила мира» — это две разные беды с разными ответами для игрока.
  {
    fnet::follower_config broken;
    broken.protocol_version_override = fnet::protocol_version + 1;
    const auto result = run_case(runtime, authority_content, follower_content, 20260911, broken);
    std::printf("protocol mismatch: refusal = %s\n", fnet::describe(result.refusal));
    CHECK(!result.joined);
    CHECK(result.refusal == network::session_refusal_reason::protocol_version_mismatch);
    CHECK(result.state_bytes == 0);
  }

  {
    fnet::follower_config broken;
    broken.corrupt_content_root = true;
    const auto result = run_case(runtime, authority_content, follower_content, 20260911, broken);
    std::printf("content mismatch: refusal = %s\n", fnet::describe(result.refusal));
    CHECK(!result.joined);
    CHECK(result.refusal == network::session_refusal_reason::content_mismatch);
    CHECK(result.state_bytes == 0);
  }

  {
    fnet::follower_config broken;
    broken.corrupt_component_schema = true;
    const auto result = run_case(runtime, authority_content, follower_content, 20260911, broken);
    std::printf("component schema mismatch: refusal = %s\n", fnet::describe(result.refusal));
    CHECK(!result.joined);
    CHECK(result.refusal == network::session_refusal_reason::component_schema_mismatch);
    CHECK(result.state_bytes == 0);
  }

  std::printf("%s\n", failures == 0 ? "join smoke: ok" : "join smoke: FAILED");
  return failures == 0 ? 0 : 1;
}
