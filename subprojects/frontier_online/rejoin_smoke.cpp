// Машина повторного входа графического клиента (core/net_client) против настоящего авторитета в том
// же процессе, без окна. Решения автора 2026-11-23: после обрыва клиент возвращается ТЕМ ЖЕ
// участником, пока авторитет держит его слот; окно истекло — новый вход новым участником; конец
// прогона авторитета (`shutdown`) — конец сессии без попыток.
//
//   1. клиент входит в сессию;
//   2. авторитет прекращает обслуживание по сроку (timeout) — клиент возвращается тем же
//      участником, авторитет догоняет его тиками из истории (якорь клиента в ней есть);
//   3. то же, но клиент молчит дольше, чем держит история, — догон со снимка кольца;
//   4. клиент молчит дольше окна удержания — игрок уходит, клиент входит заново новым участником;
//   5. авторитет называет shutdown — сессия кончается, новых попыток нет.
//
// Порт авторитета не освобождается, пока жив клиент: тесты идут параллельно, запасной диапазон
// портов у них общий, и клиент, повторяющий вход на освобождённый порт, попадал к чужому
// авторитету (а чужой авторитет — на этот порт).

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/log.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"
#include "core/causal_content.h"
#include "core/net_client.h"
#include "net/authority.h"
#include "net/link.h"
#include "net/live_runtime.h"

namespace fo = frontier_online::core;
namespace fnet = frontier_online::net;
using clock_type = std::chrono::steady_clock;

namespace {

int failures = 0;

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("CHECK failed at line %d: %s\n", __LINE__, #cond); \
      ++failures;                                                    \
    }                                                                \
  } while (false)

// История ~1 с при 60 Гц и снимок каждые 16 тиков: молчание в 2 с выводит клиента из истории, но
// не из окна удержания (4 с); молчание в 5 с выводит и из окна.
constexpr auto hold_window = std::chrono::milliseconds(4000);

std::unique_ptr<fnet::live_authority> make_authority(fnet::net_runtime& runtime,
                                                     const fo::causal_content& content) {
  fnet::live_run_options options;
  options.scripted_inputs = false; // сессия для людей: уход и повторный вход штатны
  options.expected_peers = 2;
  options.actors = 32;
  options.workers = 1;
  options.final_tick = 1'000'000;
  options.history_ticks = 64;
  options.checkpoint_every_ticks = 16;
  fnet::authority_config config;
  config.host = 0x7f000001u;
  config.port = 0; // свободный порт петли
  config.max_peers = options.expected_peers;
  config.verbose = false;
  config.keep_joined_connection = true;
  config.hold_window = hold_window;
  return std::make_unique<fnet::live_authority>(runtime, content, options, config);
}

// Круг теста: сеть, авторитет (если он есть) с тиками по часам, клиент. До условия или срока.
void terminate_live_peers(fnet::live_authority& authority, const fnet::termination_reason reason) {
  for (const auto& join : authority.endpoint().joins())
    if (join.agreed && authority.endpoint().is_live_member(join.peer_id))
      authority.endpoint().terminate(join.peer_id, reason);
}

template <class Done>
bool drive(fnet::net_runtime& runtime, fnet::live_authority* authority, fo::net_client& client,
           const std::chrono::milliseconds limit, Done&& done) {
  const auto deadline = clock_type::now() + limit;
  const auto period = std::chrono::milliseconds(1000 / 60);
  auto next_tick = clock_type::now() + period;
  while (clock_type::now() < deadline) {
    runtime.pump();
    if (authority != nullptr) {
      authority->service();
      if (clock_type::now() >= next_tick) {
        authority->advance();
        next_tick += period;
      }
    }
    client.service();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

} // namespace

// Авторитет тикает без клиента: клиент «молчит» (его процесс завис или сеть пропала).
void run_authority_alone(fnet::net_runtime& runtime, fnet::live_authority& authority,
                         const std::chrono::milliseconds span) {
  const auto deadline = clock_type::now() + span;
  const auto period = std::chrono::milliseconds(1000 / 60);
  auto next_tick = clock_type::now() + period;
  while (clock_type::now() < deadline) {
    runtime.pump();
    authority.service();
    if (clock_type::now() >= next_tick) {
      authority.advance();
      next_tick += period;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

int main() {
  spdlog::set_level(spdlog::level::warn);
  const fo::causal_content content(fo::project_modules_root());
  fnet::net_runtime runtime(4);
  devils_engine::thread::atomic_pool pool(1);

  auto authority = make_authority(runtime, content);
  const uint16_t port = authority->port();
  fo::join_request_t request;
  request.endpoint = "127.0.0.1:" + std::to_string(port);
  auto client = fo::net_client::connect(request, fo::project_modules_root(), pool, &runtime);
  CHECK(client != nullptr);
  if (!client) return 1;
  const auto& stats = authority->stats();

  // 1. Первый вход. Ключ повторного входа приходит после подтверждения — подождать и его.
  const bool joined = drive(runtime, authority.get(), *client, std::chrono::seconds(10), [&] {
    return client->world() != nullptr && client->tick() > 5 && stats.admitted_peers == 1;
  });
  CHECK(joined);
  drive(runtime, authority.get(), *client, std::chrono::milliseconds(300), [] { return false; });
  CHECK(client->sessions() == 1);
  const uint64_t peer = client->peer();
  CHECK(peer != 0);
  const uint64_t first_tick = client->tick();

  // 2. Срок фазы: клиент возвращается тем же участником, догон тиками из истории.
  terminate_live_peers(*authority, fnet::termination_reason::timeout);
  const bool resumed = drive(runtime, authority.get(), *client, std::chrono::seconds(10), [&] {
    return client->resumes() == 1 && client->tick() > first_tick + 5;
  });
  CHECK(resumed);
  CHECK(client->sessions() == 1);
  CHECK(client->peer() == peer);
  CHECK(stats.admitted_peers == 1); // игрок тот же: канонического присоединения нет
  CHECK(stats.departed_peers == 0);
  CHECK(stats.journal_resumes == 1);
  CHECK(authority->endpoint().resumed_joins() == 1);
  CHECK(client->world() != nullptr &&
        client->world()->player_of(peer) != devils_engine::aesthetics::invalid_entityid);
  const uint64_t resume_tick = client->tick();

  // 3. Клиент молчит дольше истории, но в пределах окна: догон со снимка кольца.
  terminate_live_peers(*authority, fnet::termination_reason::timeout);
  run_authority_alone(runtime, *authority, std::chrono::milliseconds(2000));
  CHECK(authority->endpoint().is_held(peer));
  const bool from_snapshot = drive(runtime, authority.get(), *client, std::chrono::seconds(10), [&] {
    return client->resumes() == 2 && client->tick() > resume_tick + 120;
  });
  CHECK(from_snapshot);
  CHECK(client->peer() == peer);
  CHECK(stats.checkpoint_resumes == 1);
  CHECK(stats.admitted_peers == 1);
  CHECK(stats.departed_peers == 0);
  // Вернувшийся считает тот же мир: тик за тиком корни авторитета проверены ведомым.
  const bool level = drive(runtime, authority.get(), *client, std::chrono::seconds(5), [&] {
    return client->tick() + 2 >= authority->tick();
  });
  CHECK(level);

  // 4. Клиент молчит дольше окна удержания: игрок уходит, клиент входит заново новым участником.
  terminate_live_peers(*authority, fnet::termination_reason::timeout);
  run_authority_alone(runtime, *authority, hold_window + std::chrono::milliseconds(1000));
  CHECK(stats.departed_peers == 1);
  CHECK(!authority->endpoint().is_held(peer));
  const bool entered_anew = drive(runtime, authority.get(), *client, std::chrono::seconds(20), [&] {
    return client->sessions() == 2 && client->world() != nullptr && stats.admitted_peers == 2;
  });
  CHECK(entered_anew);
  CHECK(client->peer() != peer);
  CHECK(client->resumes() == 2);
  drive(runtime, authority.get(), *client, std::chrono::milliseconds(300), [] { return false; });

  // 5. Конец прогона: авторитет называет shutdown — сессия кончается без новых попыток.
  terminate_live_peers(*authority, fnet::termination_reason::shutdown);
  const bool ended = drive(runtime, authority.get(), *client, std::chrono::seconds(5),
                           [&] { return client->ended(); });
  CHECK(ended);
  drive(runtime, authority.get(), *client, std::chrono::milliseconds(1500), [] { return false; });
  CHECK(client->sessions() == 2); // после shutdown повторного входа нет
  CHECK(client->resumes() == 2);

  std::printf("rejoin smoke: peer %llu, %u resumes (%u from history, %u from a snapshot), %u sessions, "
              "%u departed, %s\n",
              static_cast<unsigned long long>(peer), client->resumes(), stats.journal_resumes,
              stats.checkpoint_resumes, client->sessions(), stats.departed_peers,
              failures == 0 ? "ok" : "FAILED");
  client.reset();
  authority.reset();
  return failures == 0 ? 0 : 1;
}
