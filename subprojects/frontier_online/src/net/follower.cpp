#include "net/follower.h"

#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>

#include <devils_engine/network/canonical_tick_wire.h>
#include <devils_engine/utils/core.h>

#include "net/link.h"
#include <devils_engine/network/secure_random.h>

namespace frontier_online {
namespace net {

namespace utils = devils_engine::utils;
using clock = std::chrono::steady_clock;

namespace {

clock::time_point deadline_after(const std::chrono::milliseconds span) {
  return span.count() == 0 ? clock::time_point::max() : clock::now() + span;
}

network::session_nonce random_nonce() {
  network::session_nonce value{};
  devils_engine::network::secure_random_bytes(value);
  return value;
}

} // namespace

struct follower::state {
  const core::causal_content& content;
  follower_config config;
  network::session_compatibility compatibility;
  net_link link;
  network::gns_peer peer;
  std::optional<network::client_handshake> handshake;
  follower_phase phase = follower_phase::connecting;
  network::session_refusal_reason refusal = network::session_refusal_reason::none;

  world_declaration world;
  bool world_checked = false;
  checkpoint_begin header;
  bool header_seen = false;
  // Checkpoint кусками одной упорядоченной полосы (network/document_chunks.h).
  network::document_assembly document;

  join_report report;
  bool reported = false;
  bool live_ready = false;
  bool live_terminal = false;
  follower_fault fault = follower_fault::none;
  std::optional<termination_reason> termination;
  // Срок текущей фазы и срок прогресса передачи (сдвигается каждым пришедшим куском).
  clock::time_point deadline = clock::time_point::max();
  clock::time_point progress_deadline = clock::time_point::max();
  uint64_t catchup_tick = 0;
  std::vector<std::vector<std::byte>> canonical_ticks;

  std::vector<std::byte> scratch;
  std::vector<std::byte> reply;
  std::vector<std::vector<std::byte>> pending_control;
  std::optional<reconnect_grant> grant;
  std::optional<resume_plan> plan;

  // Ответ клиента на вызов. Новый вход — объявленный аноним (РОВНО пустое удостоверение).
  // Продолжение — билет авторитета, подписанный выведенным секретом поверх НОВОГО транскрипта.
  struct answer_policy {
    const std::optional<resume_claim>& resume;
    bool answer(const network::authority_challenge&, const utils::digest& transcript,
                std::vector<std::byte>& credential) {
      credential.clear();
      if (!resume) return true;
      auto value = resume->grant.credential;
      credential_policy mac;
      network::credential_scratch scratch;
      credential.reserve(network::reconnect_credential_bytes);
      return network::seal_reconnect_presentation(resume->grant.secret, transcript, mac, scratch,
                                                  value) == network::credential_status::accepted &&
             network::try_encode_credential(value, credential) == network::credential_status::accepted;
    }
  };

  state(net_runtime& runtime, const core::causal_content& c, const follower_config cfg)
    : content(c), config(cfg), compatibility(local_compatibility(c)), link(runtime, 1) {
    if (config.protocol_version_override != 0) {
      compatibility.protocol_version = config.protocol_version_override;
    }
    if (config.corrupt_content_root) {
      compatibility.content_root[0] = uint8_t(compatibility.content_root[0] ^ 0xffu);
    }
    if (config.corrupt_intent_schema) {
      compatibility.intent_schema_fingerprint ^= 1u;
    }
    if (config.corrupt_component_schema) {
      compatibility.component_schema_fingerprint ^= 1u;
    }
    scratch.reserve(max_message_bytes);
    reply.reserve(max_message_bytes);
  }

  bool loud() const noexcept {
    return config.verbose;
  }

  void fail(const join_status status, const follower_fault reason = follower_fault::none) {
    report.status = status;
    phase = follower_phase::failed;
    if (fault == follower_fault::none) fault = reason;
  }

  // Ошибка ПОСЛЕ присоединения: отчёт уже ушёл, сессия просто прекращается.
  void live_failure(const follower_fault reason) {
    if (fault == follower_fault::none) fault = reason;
  }

  // Проверка мира. Клиент не спорит с авторитетом о зерне — он принимает объявленное и проверяет,
  // что ПОЛУЧАЕТ то же самое. Расхождение здесь означает, что стороны считают разные миры, и
  // узнать это надо до первого тика, а не по картинке.
  void check_world() {
    world_checked = true;
    if (!content.has_generator(world.generator)) {
      if (loud()) std::printf("follower: declared generator '%s' is not present\n", world.generator.c_str());
      fail(join_status::generator_missing);
      return;
    }
    const auto source = content.make_terrain(world.generator, world.chunk_size, world.world_seed);

    report.world_fingerprint = source->fingerprint();
    report.probe_root = terrain_probe_root(*source);
    if (report.world_fingerprint != world.generator_fingerprint) {
      fail(join_status::world_fingerprint_mismatch);
      return;
    }
    if (report.probe_root != world.probe_root) {
      fail(join_status::world_probe_mismatch);
      return;
    }
    phase = follower_phase::receiving_state;
    if (loud()) {
      std::printf("follower: world accepted, seed %llu chunk %u fingerprint %llu probe %llu\n",
                  static_cast<unsigned long long>(world.world_seed), world.chunk_size,
                  static_cast<unsigned long long>(report.world_fingerprint),
                  static_cast<unsigned long long>(report.probe_root));
    }
  }

  // Управляющие сообщения, ещё не принятые транспортом, в порядке отправки. Повторяются каждый
  // круг: отказ полосы — обратное давление, а не повод промолчать (ответ рукопожатия и отчёт
  // авторитет иначе так и не получил бы и закрыл бы по сроку фазы).
  void queue_control(const std::span<const std::byte> bytes) {
    pending_control.emplace_back(bytes.begin(), bytes.end());
    flush_control();
  }

  void flush_control() {
    while (!pending_control.empty()) {
      if (link.send(peer, lane_control, pending_control.front()) != send_result::sent) return;
      pending_control.erase(pending_control.begin());
    }
  }

  // Отчёт уходит только в установленной сессии: до неё у авторитета нет грамматики проектных
  // сообщений, а после отказа рукопожатия говорить уже не о чем.
  void send_report() {
    if (reported || !handshake.has_value() || !handshake->established()) return;
    reported = true;
    if (!encode(report, scratch)) {
      utils::error{}("frontier_online follower: join report does not encode");
    }
    queue_control(scratch);
  }
};

follower::follower(net_runtime& runtime, const core::causal_content& content,
                   const follower_config config)
  : state_(std::make_unique<state>(runtime, content, config)) {}

follower::~follower() = default;

follower_phase follower::phase() const noexcept {
  return state_->phase;
}

bool follower::finished() const noexcept {
  return state_->phase == follower_phase::joined || state_->phase == follower_phase::failed;
}

const join_report& follower::report() const noexcept {
  return state_->report;
}

network::session_refusal_reason follower::refusal() const noexcept {
  return state_->refusal;
}

const world_declaration& follower::world() const noexcept {
  return state_->world;
}

uint64_t follower::session() const noexcept {
  return state_->handshake.has_value() ? state_->handshake->accepted().session : 0;
}

uint64_t follower::peer_id() const noexcept {
  return state_->handshake.has_value() ? state_->handshake->accepted().local_peer : 0;
}

uint32_t follower::received_bytes() const noexcept {
  return state_->document.received();
}

uint32_t follower::expected_bytes() const noexcept {
  return state_->header_seen ? state_->header.total_bytes : 0;
}

send_result follower::send_intent(const std::span<const std::byte> bytes) {
  auto& self = *state_;
  if (self.phase != follower_phase::joined || !self.live_ready || self.live_terminal)
    return send_result::no_peer;
  if (bytes.empty() || bytes.size() > network::intent_batch_max_bytes)
    return send_result::refused;
  return self.link.send(self.peer, lane_intent, bytes);
}

std::vector<std::vector<std::byte>> follower::take_canonical_ticks() {
  std::vector<std::vector<std::byte>> result;
  result.swap(state_->canonical_ticks);
  return result;
}

bool follower::live_terminal() const noexcept { return state_->live_terminal; }
bool follower::live_ready() const noexcept { return state_->live_ready; }
bool follower::live_fault() const noexcept {
  return state_->phase == follower_phase::joined && state_->fault != follower_fault::none;
}
follower_fault follower::fault() const noexcept { return state_->fault; }
std::optional<termination_reason> follower::termination() const noexcept {
  return state_->termination;
}
uint64_t follower::catchup_tick() const noexcept { return state_->catchup_tick; }
bool follower::connected() const noexcept { return state_->handshake.has_value(); }
bool follower::established() const noexcept {
  return state_->handshake.has_value() && state_->handshake->established();
}
const std::optional<reconnect_grant>& follower::grant() const noexcept { return state_->grant; }
const std::optional<resume_plan>& follower::plan() const noexcept { return state_->plan; }

bool follower::start() {
  auto& self = *state_;
  if (!self.link.connect_to(self.config.host, self.config.port)) {
    if (self.loud()) std::printf("follower: connect refused by the transport\n");
    self.phase = follower_phase::failed;
    return false;
  }
  self.peer = self.link.pending();
  self.deadline = deadline_after(self.config.connect_timeout);
  return true;
}

bool follower::leave() {
  auto& self = *state_;
  if (self.phase != follower_phase::joined || !self.live_ready || self.live_terminal) return false;
  if (!encode_leave(self.scratch)) utils::error{}("frontier_online follower: leave does not encode");
  self.queue_control(self.scratch);
  return true;
}

bool follower::drain() {
  auto& self = *state_;
  self.flush_control();
  std::array<net_link::observation, 4> events;
  const size_t count = self.link.poll(events);
  for (size_t i = 0; i < count; ++i) {
    if (events[i].peer == self.peer && events[i].terminal) return true;
  }
  // Входящее в этой фазе нас уже не касается, но арендованные сообщения надо забрать, иначе
  // приёмник упрётся в свой предел аренд и перестанет качать.
  self.link.receive([](network::gns_peer, uint16_t, std::span<const std::byte>) {});
  return false;
}

void follower::step(checkpoint_sink& sink) {
  auto& self = *state_;
  self.flush_control();
  if (self.phase == follower_phase::failed) return;
  if (self.phase == follower_phase::joined && (self.live_terminal || self.fault != follower_fault::none))
    return;

  std::array<net_link::observation, 4> events;
  const size_t count = self.link.poll(events);
  // Закрытие обрабатывается ПОСЛЕ приёма: последнее слово авторитета (session_terminated) могло
  // прийти в том же круге, что и закрытие, и прочитать его надо раньше, чем сдаться.
  bool closed = false;
  for (size_t i = 0; i < count; ++i) {
    if (!(events[i].peer == self.peer)) continue;
    if (events[i].terminal) {
      closed = true;
      continue;
    }
    if (events[i].connected && self.phase == follower_phase::connecting) {
      // Рукопожатие начинает КЛИЕНТ, и только после того, как соединение действительно есть.
      if (self.config.resume) {
        const auto& claim = *self.config.resume;
        self.handshake.emplace(self.compatibility, random_nonce(), claim.grant.session,
                               network::recovery_anchor<uint64_t, utils::digest>{
                                 claim.tick, anchor_digest(claim.root)});
      } else {
        self.handshake.emplace(self.compatibility, random_nonce());
      }
      if (self.handshake->start(self.scratch) != network::session_wire_status::ok) {
        utils::error{}("frontier_online follower: client hello does not encode");
      }
      if (self.link.send(self.peer, lane_control, self.scratch) != send_result::sent) {
        if (self.loud()) std::printf("follower: could not send the hello\n");
        self.phase = follower_phase::failed;
        return;
      }
      self.phase = follower_phase::handshaking;
      self.deadline = deadline_after(self.config.handshake_timeout);
    }
  }

  // Обратное давление: присоединившийся забирает из транспорта не больше, чем ему есть куда
  // положить. Остальное ждёт в очереди GNS — это и есть сигнал авторитету притормозить.
  size_t budget = net_link::default_receive_budget;
  if (self.phase == follower_phase::joined) {
    budget = self.canonical_ticks.size() < self.config.canonical_capacity
               ? self.config.canonical_capacity - self.canonical_ticks.size()
               : 0;
    // Узкое место сейчас — мы сами: тишина авторитета не измерима, пока мы не читаем.
    if (budget == 0) self.deadline = deadline_after(self.config.live_silence_timeout);
  }

  if (budget != 0) self.link.receive([&](const network::gns_peer from, const uint16_t lane,
                        const std::span<const std::byte> payload) {
    if (!(from == self.peer) || !self.handshake.has_value()) return;
    if (self.phase == follower_phase::failed) return;

    if (self.phase == follower_phase::joined) {
      if (self.fault != follower_fault::none) return;
      if (lane != lane_control) {
        self.live_failure(follower_fault::protocol_violation);
        return;
      }
      self.deadline = deadline_after(self.config.live_silence_timeout);
      const auto type = peek_message_type(payload);
      if (type == uint8_t(message::session_terminated)) {
        session_terminated notice;
        if (!decode(payload, notice)) {
          self.live_failure(follower_fault::protocol_violation);
          return;
        }
        self.termination = notice.reason;
        self.live_failure(follower_fault::terminated);
        return;
      }
      if (!self.live_ready) {
        join_confirmation confirmation;
        if (type != uint8_t(message::join_confirmed) || !decode(payload, confirmation) ||
            confirmation.session != self.handshake->accepted().session ||
            confirmation.logical_peer != self.handshake->accepted().local_peer ||
            confirmation.tick != self.report.tick || confirmation.root != self.report.root) {
          self.live_failure(follower_fault::protocol_violation);
          return;
        }
        self.live_ready = true;
        return;
      }
      if (type == uint8_t(message::reconnect_grant)) {
        reconnect_grant value;
        if (!decode(payload, value) || value.session != self.handshake->accepted().session ||
            value.logical_peer != self.handshake->accepted().local_peer) {
          self.live_failure(follower_fault::protocol_violation);
          return;
        }
        self.grant = value;
        return;
      }
      if (type == uint8_t(message::live_caught_up)) {
        catchup_marker marker;
        if (self.catchup_tick != 0 || !decode(payload, marker) ||
            marker.session != self.handshake->accepted().session ||
            marker.logical_peer != self.handshake->accepted().local_peer ||
            marker.tick < self.report.tick) {
          self.live_failure(follower_fault::protocol_violation);
          return;
        }
        self.catchup_tick = marker.tick;
        return;
      }
      // Бюджет приёма не даёт сюда попасть с полной очередью; если всё же попали — это
      // нарушение собственного инварианта, а не поведение сервера.
      if (payload.empty() || payload.size() > network::canonical_tick_max_bytes ||
          self.canonical_ticks.size() >= self.config.canonical_capacity) {
        self.live_failure(follower_fault::protocol_violation);
        return;
      }
      self.canonical_ticks.emplace_back(payload.begin(), payload.end());
      return;
    }

    if (!self.handshake->established()) {
      if (lane != lane_control) return;
      state::answer_policy policy{self.config.resume};
      const auto status = self.handshake->consume(payload, self.reply, policy);
      if (!self.reply.empty()) {
        self.queue_control(self.reply);
        self.reply.clear();
      }
      if (self.handshake->phase() == network::handshake_phase::refused) {
        self.refusal = self.handshake->refusal();
        self.report.status = join_status::handshake_refused;
        if (self.loud()) {
          std::printf("follower: refused: %s (wire status %u)\n", describe(self.refusal),
                      unsigned(status));
        }
        self.phase = follower_phase::failed;
      } else if (self.handshake->established()) {
        // Проверка на вшивость: «сессии нет» не может быть принятой сессией.
        if (self.handshake->accepted().session == 0 || self.handshake->accepted().local_peer == 0 ||
            (self.config.resume &&
             self.handshake->accepted().local_peer != self.config.resume->grant.logical_peer)) {
          self.fail(join_status::handshake_refused, follower_fault::insane_authority);
          return;
        }
        self.phase = follower_phase::awaiting_world;
        self.deadline = deadline_after(self.config.world_timeout);
        self.progress_deadline = deadline_after(self.config.transfer_stall_timeout);
        if (self.loud()) {
          std::printf("follower: established, session %llu peer %llu start tick %llu\n",
                      static_cast<unsigned long long>(self.handshake->accepted().session),
                      static_cast<unsigned long long>(self.handshake->accepted().local_peer),
                      static_cast<unsigned long long>(self.handshake->accepted().start_tick));
        }
      }
      return;
    }

    if (lane == lane_control && peek_message_type(payload) == uint8_t(message::session_terminated)) {
      session_terminated notice;
      if (!decode(payload, notice)) {
        self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
        return;
      }
      self.termination = notice.reason;
      if (self.loud()) std::printf("follower: terminated by the authority: %s\n", describe(notice.reason));
      self.phase = follower_phase::failed;
      if (self.fault == follower_fault::none) self.fault = follower_fault::terminated;
      self.reported = true; // авторитет уже решил; отчёт после его последнего слова не нужен
      return;
    }

    if (lane == lane_control && self.config.resume) {
      // Продолжение: мир у клиента уже есть, первое и единственное слово до отчёта — план.
      resume_plan value;
      const auto& accepted = self.handshake->accepted();
      if (self.plan || peek_message_type(payload) != uint8_t(message::resume_plan) ||
          !decode(payload, value) || value.session != accepted.session ||
          value.logical_peer != accepted.local_peer || value.tick != accepted.start_tick) {
        self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
        return;
      }
      self.plan = value;
      self.world_checked = true;
      self.deadline = clock::time_point::max();
      if (value.mode == resume_mode::checkpoint) {
        self.phase = follower_phase::receiving_state;
        return;
      }
      // Якорь найден в истории авторитета: состояние уже на месте, тот же тик и корень.
      if (value.tick != self.config.resume->tick || value.root != self.config.resume->root) {
        self.fail(join_status::checkpoint_refused, follower_fault::insane_authority);
        return;
      }
      self.report.status = join_status::loaded;
      self.report.tick = value.tick;
      self.report.root = value.root;
      self.phase = follower_phase::joined;
      self.progress_deadline = clock::time_point::max();
      self.deadline = deadline_after(self.config.confirmation_timeout);
      self.send_report();
      return;
    }

    if (lane == lane_control) {
      if (peek_message_type(payload) != uint8_t(message::world_declaration) || self.world_checked) {
        self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
        return;
      }
      if (!decode(payload, self.world)) {
        if (self.loud()) std::printf("follower: malformed world declaration\n");
        self.fail(join_status::world_declaration_malformed, follower_fault::protocol_violation);
        return;
      }
      self.check_world();
      if (self.phase != follower_phase::failed) self.deadline = clock::time_point::max();
      return;
    }

    if (lane != lane_bulk) {
      self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
      return;
    }
    self.progress_deadline = deadline_after(self.config.transfer_stall_timeout);

    const auto type = peek_message_type(payload);
    if (type == uint8_t(message::checkpoint_begin)) {
      if (self.header_seen || !decode(payload, self.header)) {
        self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
        return;
      }
      // Авторитет обещал в рукопожатии тик, с которого участник входит. Состояние другого тика —
      // противоречие, а не «чуть позже».
      if (self.header.tick != self.handshake->accepted().start_tick) {
        self.fail(join_status::checkpoint_refused, follower_fault::insane_authority);
        return;
      }
      // Предел проверен декодером заголовка; begin проверяет его ещё раз до выделения памяти.
      if (!self.document.begin(self.header.total_bytes, max_checkpoint_bytes)) {
        self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
        return;
      }
      self.header_seen = true;
      return;
    }
    if (type != uint8_t(message::checkpoint_chunk)) {
      self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
      return;
    }
    if (!self.header_seen) {
      // Полоса упорядоченная, поэтому кусок раньше заголовка — это не гонка, а протокольная
      // ошибка, и молчать о ней нельзя.
      if (self.loud()) std::printf("follower: checkpoint chunk before its header\n");
      self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
      return;
    }
    chunk_view chunk;
    if (!decode_chunk(payload, chunk) || !self.document.append_in_order(chunk)) {
      self.fail(join_status::checkpoint_refused, follower_fault::protocol_violation);
      return;
    }
  }, budget);

  if (closed && self.phase != follower_phase::failed) {
    if (self.phase == follower_phase::joined) {
      self.live_terminal = true;
      return;
    }
    // Обрыв — это исход, а не пауза. Если мы ещё не знаем причины, ею останется та, что была.
    if (self.loud()) std::printf("follower: connection closed by the authority\n");
    self.phase = follower_phase::failed;
    if (self.fault == follower_fault::none) self.fault = follower_fault::connection_closed;
    return;
  }

  if (self.phase == follower_phase::failed) {
    if (!closed) self.send_report();
    return;
  }

  // Документ собран. Загрузка — на границе приёма, не в обработчике сообщения: там ещё живёт
  // аренда на чужие байты.
  if (self.phase == follower_phase::receiving_state && self.header_seen &&
      self.document.complete()) {
    self.report.tick = self.header.tick;
    uint64_t root = 0;
    if (!sink.load(self.header, self.document.bytes(), root)) {
      self.fail(join_status::checkpoint_refused);
    } else {
      self.report.root = root;
      if (root != self.header.root) {
        self.fail(join_status::root_mismatch);
      } else {
        self.report.status = join_status::loaded;
        self.phase = follower_phase::joined;
        self.progress_deadline = clock::time_point::max();
        self.deadline = deadline_after(self.config.confirmation_timeout);
      }
    }
    self.document.release();
    if (self.loud()) {
      std::printf("follower: checkpoint %u bytes at tick %llu, root %llu (authority %llu): %s\n",
                  self.header.total_bytes,
                  static_cast<unsigned long long>(self.header.tick),
                  static_cast<unsigned long long>(self.report.root),
                  static_cast<unsigned long long>(self.header.root), describe(self.report.status));
    }
    self.send_report();
    return;
  }

  // Сроки. Проверяются после приёма: сообщение, пришедшее в этом круге, успевает сдвинуть срок.
  const auto now = clock::now();
  if (self.phase == follower_phase::joined) {
    if (self.deadline <= now)
      self.live_failure(self.live_ready ? follower_fault::live_silence
                                        : follower_fault::confirmation_timeout);
    return;
  }
  follower_fault expired = follower_fault::none;
  if (self.deadline <= now) {
    switch (self.phase) {
      case follower_phase::connecting: expired = follower_fault::connect_timeout; break;
      case follower_phase::handshaking: expired = follower_fault::handshake_timeout; break;
      case follower_phase::awaiting_world: expired = follower_fault::world_timeout; break;
      default: break;
    }
  }
  if (expired == follower_fault::none && self.progress_deadline <= now &&
      (self.phase == follower_phase::awaiting_world || self.phase == follower_phase::receiving_state))
    expired = follower_fault::transfer_stalled;
  if (expired != follower_fault::none) {
    if (self.loud()) std::printf("follower: %s\n", describe(expired));
    const bool before_session = self.phase == follower_phase::connecting ||
                                self.phase == follower_phase::handshaking;
    self.fail(before_session ? join_status::handshake_refused : join_status::checkpoint_refused,
              expired);
    // До рукопожатия отчёт некуда и незачем слать: сессии нет.
    if (!before_session) self.send_report();
  }
}

const char* describe(const follower_fault fault) noexcept {
  switch (fault) {
    case follower_fault::none: return "none";
    case follower_fault::connect_timeout: return "connection was not established in time";
    case follower_fault::handshake_timeout: return "handshake did not finish in time";
    case follower_fault::world_timeout: return "world declaration did not arrive in time";
    case follower_fault::transfer_stalled: return "state transfer stalled";
    case follower_fault::confirmation_timeout: return "join was not confirmed in time";
    case follower_fault::live_silence: return "authority went silent";
    case follower_fault::connection_closed: return "authority closed the connection";
    case follower_fault::protocol_violation: return "authority sent a message out of phase";
    case follower_fault::insane_authority: return "authority sent contradictory session data";
    case follower_fault::terminated: return "authority terminated the session";
  }
  return "unknown";
}

} // namespace net
} // namespace frontier_online
