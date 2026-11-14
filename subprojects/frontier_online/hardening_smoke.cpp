// ВРАЖДЕБНЫЕ УЧАСТНИКИ И ЗАМОЛЧАВШИЙ АВТОРИТЕТ.
//
// Один процесс, один поток, настоящий loopback GNS — как frontier_join_smoke. Участники здесь
// нарочно ведут себя плохо: подключаются и молчат, шлют сообщения не своей фазы, ломают контракт
// полос, заливают предложениями, отстают за историю. Авторитет тоже бывает плохим: молчит
// посреди рукопожатия, бросает передачу состояния, присылает противоречивые данные.
//
// Что доказывается — ни одно такое поведение не роняет процесс и не блокирует остальных:
//   1. закрытые участники освобождают слоты: после десятка отказов присоединение проходит;
//   2. молчащее соединение закрывается в срок;
//   3. сообщение не по контракту полос закрывает ЭТО соединение, а не процесс;
//   4. сообщения не своей фазы считаются нарушениями, порог закрывает участника;
//   5. поток предложений одного участника не глушит ввод другого;
//   6. два участника делают одно действие в одном тике; отставший за историю отключается с
//      причиной, остальные продолжают; медленный ведомый догоняет без ошибки;
//   7. ведомый прекращает сессию с названной причиной, если авторитет молчит или противоречит себе.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <vector>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <devils_engine/network/intent_wire.h>
#include <devils_engine/network/session_wire.h>
#include <devils_engine/utils/string_id.h>
#include <spdlog/spdlog.h>

#include "core/actor_checkpoint.h"
#include "core/causal_content.h"
#include "net/link.h"
#include "net/live_runtime.h"
#include "net/player_intent_wire.h"
#include "net/protocol.h"
#include "net/secure_random.h"

namespace fo = frontier_online::core;
namespace fnet = frontier_online::net;
namespace network = devils_engine::network;
using namespace std::chrono_literals;

namespace {

int failures = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
      ++failures;                                             \
    }                                                         \
  } while (0)

constexpr uint32_t loopback = 0x7f000001;

template <class Step, class Done>
bool drive(fnet::net_runtime& runtime, Step&& step, Done&& done,
           const std::chrono::milliseconds limit = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    runtime.pump();
    step();
    if (done()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

fnet::live_run_options session_options() {
  fnet::live_run_options value;
  value.actors = 64;
  value.workers = 1;
  value.final_tick = 1000000;
  return value;
}

fnet::authority_config authority_options(const uint32_t peers) {
  fnet::authority_config value;
  value.host = loopback;
  value.port = 0;
  value.max_peers = peers;
  value.verbose = false;
  value.keep_joined_connection = true;
  value.hello_timeout = 300ms;
  value.response_timeout = 300ms;
  value.close_linger = 50ms;
  return value;
}

fnet::follower_config follower_options(const uint16_t port) {
  fnet::follower_config value;
  value.host = loopback;
  value.port = port;
  value.verbose = false;
  return value;
}

bool terminated(const fnet::authority& endpoint, const uint64_t peer,
                const fnet::termination_reason reason) {
  for (const auto& value : endpoint.terminations()) {
    if (value.peer_id == peer && value.reason == reason) return true;
  }
  return false;
}

bool any_termination(const fnet::authority& endpoint, const fnet::termination_reason reason) {
  for (const auto& value : endpoint.terminations()) {
    if (value.reason == reason) return true;
  }
  return false;
}

// Клиент без протокола: только соединение. Подключается, говорит или молчит как велено.
struct raw_client {
  fnet::net_link link;
  network::gns_peer peer;
  bool connected = false;
  bool terminal = false;

  explicit raw_client(fnet::net_runtime& runtime) : link(runtime, 1) {}

  bool connect(const uint16_t port) {
    if (!link.connect_to(loopback, port)) return false;
    peer = link.pending();
    return true;
  }

  template <class Consumer>
  void step(Consumer&& consumer) {
    std::array<fnet::net_link::observation, 4> events;
    const size_t count = link.poll(events);
    for (size_t i = 0; i < count; ++i) {
      if (!(events[i].peer == peer)) continue;
      connected |= events[i].connected;
      terminal |= events[i].terminal;
    }
    link.receive([&](const network::gns_peer from, const uint16_t lane,
                     const std::span<const std::byte> payload) {
      if (from == peer) consumer(lane, payload);
    });
  }

  void step() {
    step([](uint16_t, std::span<const std::byte>) {});
  }
};

struct anonymous_answer {
  bool answer(const network::authority_challenge&, const devils_engine::utils::digest&,
              std::vector<std::byte>& credential) {
    credential.clear();
    return true;
  }
};

devils_engine::act::intent food(const double x, const double y) {
  devils_engine::act::intent action;
  action.kind = devils_engine::act::intent_kind::spawn_prefab;
  action.payload.spawn.prefab = devils_engine::utils::string_hash("food");
  action.payload.spawn.target = devils_engine::act::vec3{x, y, 0.0};
  return action;
}

// 1. Слоты. Двенадцать отвергнутых присоединений и шесть оборванных соединений при пределе в
//    четыре одновременных записи; после них честное присоединение обязано пройти.
void slots_are_reclaimed(fnet::net_runtime& runtime, const fo::causal_content& content,
                         const fo::causal_content& other) {
  const auto opts = session_options();
  fnet::live_authority authority(runtime, content, opts, authority_options(2));

  for (int i = 0; i < 12; ++i) {
    auto config = follower_options(authority.port());
    config.corrupt_content_root = true;
    fnet::live_follower refused(runtime, other, config, opts);
    CHECK(refused.start());
    CHECK(drive(runtime, [&] { authority.service(); refused.service(); },
                [&] { return refused.endpoint().phase() == fnet::follower_phase::failed; }));
    CHECK(refused.endpoint().refusal() == network::session_refusal_reason::content_mismatch);
  }
  for (int i = 0; i < 6; ++i) {
    raw_client dropped(runtime);
    CHECK(dropped.connect(authority.port()));
    CHECK(drive(runtime, [&] { authority.service(); dropped.step(); },
                [&] { return dropped.connected; }));
  } // соединение рвётся вместе с клиентом, до единого слова

  fnet::live_follower honest(runtime, other, follower_options(authority.port()), opts);
  CHECK(honest.start());
  const bool joined = drive(runtime, [&] { authority.service(); honest.service(); },
                            [&] { return honest.ready(); }, 10000ms);
  std::printf("slots: %u refused, honest join %s\n", authority.endpoint().refused_joins(),
              joined ? "ok" : "FAILED");
  CHECK(joined);
  CHECK(authority.endpoint().refused_joins() >= 12);
}

// 2. Молчание. Соединение без hello закрывается по сроку, слот освобождается.
void silent_peer_times_out(fnet::net_runtime& runtime, const fo::causal_content& content) {
  fnet::live_authority authority(runtime, content, session_options(), authority_options(2));
  raw_client silent(runtime);
  CHECK(silent.connect(authority.port()));
  const bool closed = drive(runtime, [&] { authority.service(); silent.step(); },
                            [&] { return silent.terminal; }, 3000ms);
  std::printf("silent peer: %s\n", closed ? "closed by deadline" : "STILL OPEN");
  CHECK(closed);
  CHECK(any_termination(authority.endpoint(), fnet::termination_reason::timeout));
  CHECK(drive(runtime, [&] { authority.service(); },
              [&] { return authority.endpoint().connected_peers() == 0; }, 1000ms));
}

// 3. Контракт полос. Клиент со СВОЕЙ раскладкой полос шлёт ненадёжное сообщение по полосе,
//    объявленной у авторитета надёжной. Адаптер закрывает это соединение; процесс авторитета
//    живёт и принимает следующего.
void lane_contract_breach_closes_only_that_peer(fnet::net_runtime& runtime,
                                                const fo::causal_content& content,
                                                const fo::causal_content& other) {
  const auto opts = session_options();
  fnet::live_authority authority(runtime, content, opts, authority_options(2));
  {
    std::array<network::gns_lane_config, 3> lanes{};
    lanes[0] = {network::gns_delivery::unreliable_sequenced, 0, 1, 4, 64, 256};
    lanes[1] = {network::gns_delivery::reliable_ordered, 0, 1, 4, 64, 256};
    lanes[2] = {network::gns_delivery::unreliable_sequenced, 0, 1, 4, 64, 256};
    network::gns_transport hostile(runtime.dispatcher(), network::gns_transport_config{.peers = 1},
                                   lanes);
    CHECK(hostile.ready());
    std::array<SteamNetworkingConfigValue_t, 1> options{};
    options[0].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
    SteamNetworkingIPAddr address{};
    address.SetIPv4(loopback, authority.port());
    const auto connection = hostile.connect(address, options);
    CHECK(connection.status == network::gns_status::ok);
    bool connected = false;
    bool closed = false;
    bool sent = false;
    const std::array<std::byte, 3> junk{std::byte{1}, std::byte{2}, std::byte{3}};
    drive(runtime,
          [&] {
            authority.service();
            std::array<network::gns_connection_event, 4> events;
            const size_t count = hostile.poll_connections(events);
            for (size_t i = 0; i < count; ++i) {
              const auto state = events[i].info.m_eState;
              connected |= state == k_ESteamNetworkingConnectionState_Connected;
              closed |= state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                        state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
            }
            if (connected && !sent)
              sent = hostile.try_send(connection.peer, 0, junk).status == network::gns_status::ok;
          },
          [&] { return closed; }, 3000ms);
    std::printf("lane breach: sent %d, peer closed %d\n", int(sent), int(closed));
    CHECK(sent);
    CHECK(closed);
  }

  fnet::live_follower honest(runtime, other, follower_options(authority.port()), opts);
  CHECK(honest.start());
  CHECK(drive(runtime, [&] { authority.service(); honest.service(); },
              [&] { return honest.ready(); }, 10000ms));
}

// 4. Нарушения. Клиент честно проходит рукопожатие, а затем шлёт по управляющей полосе мусор.
//    Каждое сообщение — нарушение; порог закрывает участника с причиной.
void phase_violations_close_peer(fnet::net_runtime& runtime, const fo::causal_content& content) {
  fnet::live_authority authority(runtime, content, session_options(), authority_options(2));
  raw_client rogue(runtime);
  CHECK(rogue.connect(authority.port()));
  network::client_handshake handshake(fnet::local_compatibility(content), network::session_nonce{});
  anonymous_answer policy;
  std::vector<std::byte> out;
  std::vector<std::byte> reply;
  out.reserve(network::session_wire_max_message_bytes);
  reply.reserve(network::session_wire_max_message_bytes);
  bool hello_sent = false;
  bool flooded = false;
  const std::array<std::byte, 1> junk{std::byte{0x07}};
  const bool closed = drive(runtime,
    [&] {
      authority.service();
      rogue.step([&](const uint16_t lane, const std::span<const std::byte> payload) {
        if (lane != fnet::lane_control || handshake.established()) return;
        (void)handshake.consume(payload, reply, policy);
        if (!reply.empty()) (void)rogue.link.send(rogue.peer, fnet::lane_control, reply);
      });
      if (rogue.connected && !hello_sent && handshake.start(out) == network::session_wire_status::ok) {
        hello_sent = rogue.link.send(rogue.peer, fnet::lane_control, out) == fnet::send_result::sent;
      }
      if (handshake.established() && !flooded) {
        for (int i = 0; i < 20; ++i) (void)rogue.link.send(rogue.peer, fnet::lane_control, junk);
        flooded = true;
      }
    },
    [&] { return rogue.terminal; }, 5000ms);
  std::printf("phase violations: established %d, closed %d, violations %llu\n",
              int(handshake.established()), int(closed),
              static_cast<unsigned long long>(authority.endpoint().violations()));
  CHECK(handshake.established());
  CHECK(closed);
  CHECK(any_termination(authority.endpoint(), fnet::termination_reason::violations));
}

// 5. Поток предложений. Один участник шлёт пачками, второй — одно действие. Первый закрыт по
//    порогу нарушений, действие второго принято и исполнено у всех.
void intent_flood_does_not_starve_others(fnet::net_runtime& runtime,
                                         const fo::causal_content& content,
                                         const fo::causal_content& other) {
  auto opts = session_options();
  opts.predict_wrong = false;
  auto config = authority_options(2);
  config.intents_per_step = 4;
  config.violation_limit = 8;
  fnet::live_authority authority(runtime, content, opts, config);
  fnet::live_follower flooder(runtime, other, follower_options(authority.port()), opts);
  fnet::live_follower honest(runtime, other, follower_options(authority.port()), opts);
  CHECK(flooder.start());
  CHECK(honest.start());
  CHECK(drive(runtime, [&] { authority.service(); flooder.service(); honest.service(); },
              [&] { return flooder.ready() && honest.ready(); }, 10000ms));

  const auto layouts = fnet::player_intent_layouts();
  const network::id_index_table registry;
  network::intent proposal;
  CHECK(fnet::encode_player_intent(food(10.0, 10.0), 2, 0, proposal));
  std::vector<std::byte> batch;
  batch.reserve(network::intent_batch_max_bytes);
  CHECK(network::try_encode_intent_batch(authority.tick() + 5, std::span(&proposal, 1), layouts,
                                         registry, batch) == network::intent_wire_status::ok);
  const uint64_t flooder_peer = flooder.endpoint().peer_id();
  CHECK(honest.propose_food(14.0, 13.0, authority.tick() + 6));

  const uint64_t until = authority.tick() + 20;
  while (authority.tick() < until) {
    runtime.pump();
    authority.service();
    authority.advance();
    if (!flooder.endpoint().live_terminal())
      for (int i = 0; i < 10; ++i) (void)flooder.endpoint().send_intent(batch);
    flooder.service();
    honest.service();
    std::this_thread::sleep_for(1ms);
  }
  CHECK(drive(runtime, [&] { authority.service(); honest.service(); },
              [&] { return honest.tick() == authority.tick(); }));
  std::printf("intent flood: flooder terminated %d, honest inputs %u, roots %s\n",
              int(terminated(authority.endpoint(), flooder_peer,
                             fnet::termination_reason::violations)),
              honest.stats().own_inputs, honest.root() == authority.root() ? "equal" : "DIFFER");
  CHECK(terminated(authority.endpoint(), flooder_peer, fnet::termination_reason::violations));
  // Причина дошла до самого участника, а не только в журнал авторитета.
  CHECK(flooder.endpoint().termination() == fnet::termination_reason::violations);
  CHECK(honest.stats().own_inputs == 1);
  CHECK(honest.root() == authority.root());
  CHECK(honest.endpoint().fault() == fnet::follower_fault::none);
}

// 6. Живая сессия под нагрузкой порядка: два действия одного вида в одном тике, медленный
//    ведомый с маленькой очередью, отставший за историю третий.
void live_session_survives_order_pressure(fnet::net_runtime& runtime,
                                          const fo::causal_content& content,
                                          const fo::causal_content& other) {
  auto opts = session_options();
  opts.history_ticks = 48;
  fnet::live_authority authority(runtime, content, opts, authority_options(3));

  auto slow_opts = opts;
  slow_opts.predict_wrong = false;
  auto slow_config = follower_options(authority.port());
  slow_config.canonical_capacity = 4;
  fnet::live_follower slow(runtime, other, slow_config, slow_opts);
  fnet::live_follower repairing(runtime, other, follower_options(authority.port()), opts);
  CHECK(slow.start());
  CHECK(repairing.start());
  CHECK(drive(runtime, [&] { authority.service(); slow.service(); repairing.service(); },
              [&] { return slow.ready() && repairing.ready(); }, 10000ms));

  // Оба предлагают spawn_food на ОДИН тик. Раньше второе действие роняло авторитет.
  const uint64_t shared_tick = authority.tick() + 10;
  CHECK(slow.propose_food(20.0, 20.0, shared_tick));
  CHECK(repairing.propose_food(30.0, 30.0, shared_tick));
  // Пауза на тик: управляющая полоса держит не больше 32 сообщений в полёте, и слот
  // освобождается подтверждением. Авторитет без пауз ушёл бы за историю от всех сразу — это
  // свойство нагрузки, а не того, что здесь проверяется.
  while (authority.tick() < shared_tick + 3) {
    runtime.pump();
    authority.service();
    authority.advance();
    slow.service();
    repairing.service();
    std::this_thread::sleep_for(2ms);
  }
  CHECK(authority.stats().applied_inputs == 2);

  // Третий проходит рукопожатие и замолкает, пока мир уходит дальше удерживаемой истории.
  fnet::live_follower late(runtime, other, follower_options(authority.port()), slow_opts);
  CHECK(late.start());
  CHECK(drive(runtime, [&] { authority.service(); slow.service(); repairing.service(); late.service(); },
              [&] { return late.endpoint().phase() == fnet::follower_phase::awaiting_world ||
                           late.endpoint().phase() == fnet::follower_phase::receiving_state; }));
  const uint64_t late_from = authority.tick();
  while (authority.tick() < late_from + 60) {
    runtime.pump();
    authority.service();
    authority.advance();
    // Медленный ведомый не читает 30 тиков подряд: его очередь — 4, остальное ждёт в транспорте.
    const uint64_t offset = authority.tick() - late_from;
    if (offset < 10 || offset >= 40) slow.service();
    repairing.service();
    std::this_thread::sleep_for(2ms);
  }
  const uint64_t late_peer = late.endpoint().peer_id();
  const bool dropped = drive(runtime,
    [&] { authority.service(); slow.service(); repairing.service(); late.service(); },
    [&] { return terminated(authority.endpoint(), late_peer,
                            fnet::termination_reason::history_exhausted) &&
                 (late.endpoint().termination().has_value() || late.endpoint().live_terminal()); });
  CHECK(drive(runtime, [&] { authority.service(); slow.service(); repairing.service(); },
              [&] { return slow.tick() == authority.tick() && repairing.tick() == authority.tick(); },
              10000ms));
  std::printf("order pressure: shared tick inputs %u, late dropped %d, slow fault '%s', roots %s\n",
              authority.stats().applied_inputs, int(dropped), fnet::describe(slow.endpoint().fault()),
              slow.root() == authority.root() && repairing.root() == authority.root() ? "equal"
                                                                                       : "DIFFER");
  CHECK(dropped);
  CHECK(late.endpoint().termination() == fnet::termination_reason::history_exhausted);
  CHECK(slow.endpoint().fault() == fnet::follower_fault::none);
  CHECK(repairing.endpoint().fault() == fnet::follower_fault::none);
  CHECK(slow.stats().own_inputs == 1);
  CHECK(repairing.stats().own_inputs == 1);
  CHECK(repairing.stats().corrected);
  CHECK(slow.root() == authority.root());
  CHECK(repairing.root() == authority.root());
}

// 8. Потери. Пятая часть пакетов теряется в обе стороны; надёжные полосы переотправляет GNS,
//    предложения — сам ведомый, повторяя неподтверждённые. Каждое действие принято РОВНО
//    один раз: повторы распознаются по номеру ввода, а не по содержимому.
void inputs_survive_loss_exactly_once(fnet::net_runtime& runtime, const fo::causal_content& content,
                                      const fo::causal_content& other) {
  auto opts = session_options();
  opts.predict_wrong = false;
  {
    fnet::live_authority authority(runtime, content, opts, authority_options(1));
    fnet::live_follower client(runtime, other, follower_options(authority.port()), opts);
    CHECK(client.start());
    const bool ready = drive(runtime, [&] { authority.service(); client.service(); },
                             [&] { return client.ready(); }, 15000ms);
    CHECK(ready);
    if (!ready) return;
    // Потери включаются после присоединения: здесь проверяется ввод, а не рукопожатие.
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(
      k_ESteamNetworkingConfig_FakePacketLoss_Send, 20.0f);
    const uint64_t first = authority.tick() + 8;
    for (uint64_t i = 0; i < 5; ++i)
      CHECK(client.propose_food(10.0 + double(i), 11.0 + double(i), first + i));
    // Настоящий период тика (60 Гц): соотношение задержек GNS и срока цели — часть проверки.
    auto next = std::chrono::steady_clock::now();
    while (authority.tick() < first + 8) {
      runtime.pump();
      authority.service();
      client.service();
      if (std::chrono::steady_clock::now() >= next) {
        authority.advance();
        next += std::chrono::microseconds(16667);
      }
      std::this_thread::sleep_for(1ms);
    }
    SteamNetworkingUtils()->SetGlobalConfigValueFloat(
      k_ESteamNetworkingConfig_FakePacketLoss_Send, 0.0f);
    CHECK(drive(runtime, [&] { authority.service(); client.service(); },
                [&] { return client.tick() == authority.tick(); }, 15000ms));
    std::printf("loss: applied %u, own %u, rejected %u, duplicates %llu, roots %s\n",
                authority.stats().applied_inputs, client.stats().own_inputs,
                client.stats().rejected_inputs,
                static_cast<unsigned long long>(authority.stats().duplicate_inputs),
                client.root() == authority.root() ? "equal" : "DIFFER");
    CHECK(authority.stats().applied_inputs == 5);
    CHECK(client.stats().own_inputs == 5);
    CHECK(client.stats().rejected_inputs == 0);
    CHECK(client.root() == authority.root());
  }
}

// 7. Плохой авторитет: рукопожатие библиотеки, всё остальное — по сценарию.
enum class bad_authority { silent, stalls, wrong_tick, zero_session };

struct fake_authority {
  struct admit_all {
    bad_authority mode;
    bool issue_challenge(const network::client_hello&, std::vector<std::byte>& challenge) {
      challenge.clear();
      return true;
    }
    network::session_refusal_reason admit(const network::client_response&,
                                          const devils_engine::utils::digest&,
                                          network::session_accepted& accepted) {
      accepted = {mode == bad_authority::zero_session ? 0u : 77u, 1, 0, 1, 5};
      return network::session_refusal_reason::none;
    }
  };

  bad_authority mode;
  fnet::world_declaration world;
  network::session_compatibility compatibility;
  fnet::net_link link;
  uint16_t port = 0;
  network::gns_peer peer;
  std::optional<network::authority_handshake> handshake;
  bool sent_state = false;
  std::vector<std::byte> reply;

  fake_authority(fnet::net_runtime& runtime, const fo::causal_content& content, const bad_authority m)
    : mode(m), compatibility(fnet::local_compatibility(content)), link(runtime, 2) {
    auto terrain = content.make_terrain("generator/world", 16, 20260911);
    world = fnet::declare_world(*terrain, "generator/world");
    port = link.listen_any().port;
    reply.reserve(fnet::max_message_bytes);
  }

  void step() {
    std::array<fnet::net_link::observation, 4> events;
    const size_t count = link.poll(events);
    for (size_t i = 0; i < count; ++i) {
      if (!events[i].needs_accept) continue;
      (void)link.accept(events[i].peer);
      peer = events[i].peer;
      handshake.emplace(compatibility, network::session_nonce{});
    }
    link.receive([&](const network::gns_peer from, const uint16_t lane,
                     const std::span<const std::byte> payload) {
      if (mode == bad_authority::silent || !handshake || !(from == peer) ||
          lane != fnet::lane_control || handshake->established())
        return;
      admit_all policy{mode};
      (void)handshake->consume(payload, reply, policy);
      if (!reply.empty()) (void)link.send(peer, fnet::lane_control, reply);
    });
    if (handshake && handshake->established() && !sent_state) {
      std::vector<std::byte> bytes;
      CHECK(fnet::encode(world, bytes));
      (void)link.send(peer, fnet::lane_control, bytes);
      const fnet::checkpoint_begin header{mode == bad_authority::wrong_tick ? 6u : 5u, 1, 100,
                                          fo::actor_checkpoint_schema_fingerprint()};
      CHECK(fnet::encode(header, bytes));
      (void)link.send(peer, fnet::lane_bulk, bytes);
      sent_state = true; // и больше ни байта: передача брошена
    }
  }
};

void bad_authority_is_named(fnet::net_runtime& runtime, const fo::causal_content& content,
                            const bad_authority mode, const fnet::follower_fault expected,
                            const char* label,
                            const network::session_refusal_reason expected_refusal =
                              network::session_refusal_reason::none) {
  fake_authority server(runtime, content, mode);
  auto config = follower_options(server.port);
  config.handshake_timeout = 300ms;
  config.world_timeout = 300ms;
  config.transfer_stall_timeout = 300ms;
  fnet::live_follower client(runtime, content, config, session_options());
  CHECK(client.start());
  const bool failed = drive(runtime, [&] { server.step(); client.service(); },
                            [&] { return client.endpoint().phase() == fnet::follower_phase::failed; },
                            3000ms);
  std::printf("bad authority (%s): fault '%s', refusal '%s'\n", label,
              fnet::describe(client.endpoint().fault()), fnet::describe(client.endpoint().refusal()));
  CHECK(failed);
  CHECK(client.endpoint().fault() == expected);
  CHECK(client.endpoint().refusal() == expected_refusal);
}

// 0. Случайность рукопожатия: санитарная проверка, а не тест качества генератора.
void handshake_randomness_is_not_degenerate(fnet::net_runtime& runtime,
                                            const fo::causal_content& content) {
  std::set<std::array<std::byte, 32>> nonces;
  for (int i = 0; i < 10000; ++i) {
    std::array<std::byte, 32> value{};
    fnet::secure_random_bytes(value);
    nonces.insert(value);
  }
  CHECK(nonces.size() == 10000);
  fnet::live_authority first(runtime, content, session_options(), authority_options(1));
  fnet::live_authority second(runtime, content, session_options(), authority_options(1));
  CHECK(first.endpoint().session() != 0);
  CHECK(first.endpoint().session() != second.endpoint().session());
  std::printf("randomness: %zu distinct nonces, sessions differ\n", nonces.size());
}

} // namespace

int main() {
  spdlog::set_level(spdlog::level::err);
  fo::causal_content authority_content(FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT);
  fo::causal_content follower_content(FRONTIER_ONLINE_SOURCE_RESOURCE_ROOT);
  fnet::net_runtime runtime(32);

  handshake_randomness_is_not_degenerate(runtime, authority_content);
  slots_are_reclaimed(runtime, authority_content, follower_content);
  silent_peer_times_out(runtime, authority_content);
  lane_contract_breach_closes_only_that_peer(runtime, authority_content, follower_content);
  phase_violations_close_peer(runtime, authority_content);
  intent_flood_does_not_starve_others(runtime, authority_content, follower_content);
  live_session_survives_order_pressure(runtime, authority_content, follower_content);
  inputs_survive_loss_exactly_once(runtime, authority_content, follower_content);
  bad_authority_is_named(runtime, follower_content, bad_authority::silent,
                         fnet::follower_fault::handshake_timeout, "silent");
  bad_authority_is_named(runtime, follower_content, bad_authority::stalls,
                         fnet::follower_fault::transfer_stalled, "stalls");
  bad_authority_is_named(runtime, follower_content, bad_authority::wrong_tick,
                         fnet::follower_fault::insane_authority, "wrong tick");
  // Session 0 не доходит до ведомого: библиотека v4 не кодирует такую выдачу, и авторитет
  // отвечает отказом рукопожатия вместо тишины.
  bad_authority_is_named(runtime, follower_content, bad_authority::zero_session,
                         fnet::follower_fault::none, "zero session",
                         network::session_refusal_reason::malformed_message);

  std::printf("%s\n", failures == 0 ? "hardening smoke: ok" : "hardening smoke: FAILED");
  return failures == 0 ? 0 : 1;
}
