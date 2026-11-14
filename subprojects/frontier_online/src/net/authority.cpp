#include "net/authority.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <optional>

#include <devils_engine/network/canonical_tick_wire.h>
#include <devils_engine/utils/core.h>

#include "net/link.h"
#include "net/secure_random.h"

namespace frontier_online {
namespace net {

namespace utils = devils_engine::utils;
using clock = std::chrono::steady_clock;

namespace {

network::session_nonce random_nonce() {
  network::session_nonce value{};
  secure_random_bytes(value);
  return value;
}


clock::time_point deadline_after(const std::chrono::milliseconds span) {
  return span.count() == 0 ? clock::time_point::max() : clock::now() + span;
}

struct peer_state {
  network::gns_peer peer;
  uint64_t peer_id = 0;
  bool connected = false;
  std::optional<network::authority_handshake> handshake;
  bool world_sent = false;
  bool begin_sent = false;
  uint32_t sent_bytes = 0;
  checkpoint_begin header;
  std::vector<std::byte> document;
  bool reported = false;
  bool joined = false;
  bool confirmation_sent = false;
  // Участник больше не обслуживается: всё входящее от него отбрасывается без разбора, он ждёт
  // закрытия. Отдельно от `closed`, потому что последнему слову авторитета нужен linger.
  bool failed = false;
  // Соединение закрыто, запись удаляется в конце круга. Слот освобождается ТОЛЬКО так: запись,
  // пережившая своё соединение, занимала бы его навсегда.
  bool closed = false;
  // Срок текущей фазы. max — фаза без срока.
  clock::time_point deadline = clock::time_point::max();
  // Закрытие ОТЛОЖЕНО. `gns_transport::close` закрывает БЕЗ linger — незабранные надёжные байты
  // пропадают, — а последнее, что авторитет говорит участнику, это как раз причина.
  clock::time_point close_at = clock::time_point::max();
  // Последнее слово (session_terminated), ещё не принятое транспортом. Повторяется каждый круг
  // до закрытия: первый отказ полосы не повод промолчать.
  std::optional<termination_reason> notice;
  uint32_t violations = 0;
  uint32_t messages_this_step = 0;
  uint32_t intents_this_step = 0;
};

} // namespace

struct authority::state {
  const core::causal_content& content;
  world_declaration world;
  authority_config config;
  network::session_compatibility compatibility;
  net_link link;
  uint16_t port = 0;
  uint64_t session_id = secure_random_nonzero_u64(); // 0 = «сессии нет»
  uint64_t next_peer_id = 1;
  std::vector<peer_state> peers;
  std::vector<authority_join> joins;
  std::vector<authority_termination> terminations;
  uint32_t refused = 0;
  uint64_t violations = 0;
  uint64_t bytes_sent = 0;
  std::vector<live_intent_packet> live_intents;
  uint64_t dropped_live_intents = 0;
  std::vector<std::byte> scratch;
  std::vector<std::byte> reply;

  state(net_runtime& runtime, const core::causal_content& c, const world_declaration& w,
        const authority_config cfg)
    : content(c), world(w), config(cfg), compatibility(local_compatibility(c)),
      link(runtime, cfg.max_peers) {
    const auto outcome = config.port == 0 ? link.listen_any(config.host == 0 ? 0x7f000001
                                                                            : config.host)
                                          : link.listen_on(config.port, config.host);
    port = outcome.port;
    scratch.reserve(max_message_bytes);
    reply.reserve(max_message_bytes);
  }

  // Политика авторитета: два решения, которые библиотека отказывается угадывать.
  //
  // ЛИЧНОСТИ ПОКА НЕТ, и сказано об этом здесь прямо. Принимается РОВНО пустое удостоверение —
  // «аноним»; непустое отвергается, чтобы никто не принял отсутствие проверки за проверку. Место
  // под настоящую политику (`network/credential.h` с билетами переподключения) уже есть, и когда
  // она появится, меняться будет этот класс, а не протокол.
  struct admit_policy {
    state& owner;
    peer_state& peer;
    checkpoint_source& source;

    bool issue_challenge(const network::client_hello&, std::vector<std::byte>& challenge) {
      challenge.clear(); // вызова нечем осмысленно наполнить, пока нет удостоверений
      return true;
    }

    network::session_refusal_reason admit(const network::client_response& response,
                                          const utils::digest&,
                                          network::session_accepted& accepted) {
      if (!response.credential.empty()) return network::session_refusal_reason::identity_rejected;
      // Ёмкость считается по участникам, которые СЕЙЧАС заняты сессией (передача, ожидание
      // отчёта, живая игра), а не по уже отчитавшимся: иначе параллельные присоединения прошли
      // бы мимо предела.
      if (owner.members() >= owner.config.max_peers)
        return network::session_refusal_reason::no_capacity;

      // Чек-пойнт снимается ЗДЕСЬ, до того как клиент услышал «принят»: тик, с которого он
      // участвует, обязан быть тем самым тиком, чьё состояние ему поедет.
      peer.document.clear();
      if (!source.capture(peer.header, peer.document))
        return network::session_refusal_reason::no_capacity;
      if (peer.document.empty() || peer.document.size() > max_checkpoint_bytes ||
          peer.header.total_bytes != peer.document.size())
        return network::session_refusal_reason::no_capacity;

      accepted.session = owner.session_id;
      accepted.local_peer = peer.peer_id;
      accepted.authority_peer = 0;
      accepted.authority_epoch = 1;
      accepted.start_tick = peer.header.tick;
      return network::session_refusal_reason::none;
    }
  };

  uint32_t members() const noexcept {
    uint32_t count = 0;
    for (const auto& value : peers) {
      if (!value.failed && !value.closed && value.handshake.has_value() &&
          value.handshake->established())
        ++count;
    }
    return count;
  }

  peer_state* find(const network::gns_peer peer) {
    for (auto& value : peers) {
      if (!value.closed && value.peer == peer) return &value;
    }
    return nullptr;
  }

  peer_state* find_member(const uint64_t peer_id) {
    for (auto& value : peers) {
      if (!value.closed && value.peer_id == peer_id) return &value;
    }
    return nullptr;
  }

  bool loud() const noexcept {
    return config.verbose;
  }

  void close_now(peer_state& peer) {
    if (peer.closed) return;
    (void)link.close(peer.peer);
    peer.connected = false;
    peer.closed = true;
  }

  // Участник перестаёт обслуживаться, соединение закрывается после linger.
  void retire(peer_state& peer) {
    peer.failed = true;
    peer.deadline = clock::time_point::max();
    if (peer.close_at == clock::time_point::max()) peer.close_at = deadline_after(config.close_linger);
  }

  // Исход ДО отчёта считается отказом присоединения, после — завершением сессии.
  void end(peer_state& peer, const termination_reason reason) {
    if (peer.failed || peer.closed) return;
    if (!peer.reported) {
      peer.reported = true;
      ++refused;
    }
    terminations.push_back({peer.peer_id, reason});
    // Последнее слово — причина. Только установленному участнику: до рукопожатия у него нет
    // грамматики проектных сообщений.
    if (peer.handshake.has_value() && peer.handshake->established()) peer.notice = reason;
    if (loud())
      std::printf("authority: peer %llu terminated: %s (%u violations)\n",
                  static_cast<unsigned long long>(peer.peer_id), describe(reason),
                  peer.violations);
    retire(peer);
    send_notice(peer);
  }

  // Причина уходит после всего, что уже стоит в управляющей полосе. Не принята транспортом —
  // повтор на следующем круге; закрытие наступит в срок linger в любом случае.
  void send_notice(peer_state& peer) {
    if (!peer.notice || peer.closed || !encode(session_terminated{*peer.notice}, scratch)) return;
    if (link.send(peer.peer, lane_control, scratch) != send_result::sent) return;
    peer.notice.reset();
    // Linger отсчитывается от момента, когда причина действительно ушла.
    peer.close_at = std::max(peer.close_at == clock::time_point::max() ? clock::time_point::min()
                                                                       : peer.close_at,
                             deadline_after(config.close_linger));
  }

  void violation(peer_state& peer) {
    if (peer.failed || peer.closed) return;
    ++peer.violations;
    ++violations;
    if (config.violation_limit != 0 && peer.violations >= config.violation_limit)
      end(peer, termination_reason::violations);
  }
};

authority::authority(net_runtime& runtime, const core::causal_content& content,
                     const world_declaration& world, const authority_config config)
  : state_(std::make_unique<state>(runtime, content, world, config)) {}

authority::~authority() = default;

uint16_t authority::port() const noexcept {
  return state_->port;
}

uint64_t authority::session() const noexcept {
  return state_->session_id;
}

uint32_t authority::connected_peers() const noexcept {
  uint32_t count = 0;
  for (const auto& peer : state_->peers) {
    if (peer.connected && !peer.closed) ++count;
  }
  return count;
}

uint32_t authority::completed_joins() const noexcept {
  uint32_t count = 0;
  for (const auto& join : state_->joins) {
    if (join.agreed) ++count;
  }
  return count;
}

uint32_t authority::refused_joins() const noexcept {
  return state_->refused;
}

const std::vector<authority_join>& authority::joins() const noexcept {
  return state_->joins;
}

uint64_t authority::checkpoint_bytes_sent() const noexcept {
  return state_->bytes_sent;
}

std::vector<live_intent_packet> authority::take_live_intents() {
  std::vector<live_intent_packet> result;
  result.swap(state_->live_intents);
  return result;
}

bool authority::is_live_member(const uint64_t logical_peer) const noexcept {
  for (const auto& peer : state_->peers) {
    if (peer.peer_id == logical_peer && !peer.closed && !peer.failed && peer.joined &&
        peer.confirmation_sent && peer.connected)
      return true;
  }
  return false;
}

send_result authority::send_canonical(const uint64_t logical_peer,
                                      const std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > network::canonical_tick_max_bytes)
    return send_result::refused;
  for (const auto& peer : state_->peers) {
    if (peer.peer_id == logical_peer && !peer.closed && !peer.failed && peer.joined &&
        peer.confirmation_sent && peer.connected)
      return state_->link.send(peer.peer, lane_control, bytes);
  }
  return send_result::no_peer;
}

send_result authority::send_catchup(const uint64_t logical_peer, const uint64_t tick) {
  if (tick == 0) return send_result::refused;
  for (const auto& peer : state_->peers) {
    if (peer.peer_id != logical_peer || peer.closed || peer.failed || !peer.joined ||
        !peer.confirmation_sent || !peer.connected) continue;
    const catchup_marker marker{state_->session_id, logical_peer, tick};
    if (!encode(marker, state_->scratch)) return send_result::refused;
    return state_->link.send(peer.peer, lane_control, state_->scratch);
  }
  return send_result::no_peer;
}

uint64_t authority::dropped_live_intents() const noexcept {
  return state_->dropped_live_intents;
}

void authority::note_violation(const uint64_t logical_peer) {
  if (auto* peer = state_->find_member(logical_peer)) state_->violation(*peer);
}

void authority::terminate(const uint64_t logical_peer, const termination_reason reason) {
  if (auto* peer = state_->find_member(logical_peer)) state_->end(*peer, reason);
}

const std::vector<authority_termination>& authority::terminations() const noexcept {
  return state_->terminations;
}

uint64_t authority::violations() const noexcept {
  return state_->violations;
}

void authority::step(checkpoint_source& source) {
  auto& self = *state_;
  for (auto& peer : self.peers) {
    peer.messages_this_step = 0;
    peer.intents_this_step = 0;
  }

  std::array<net_link::observation, 8> events;
  const size_t count = self.link.poll(events);
  for (size_t i = 0; i < count; ++i) {
    const auto& event = events[i];
    if (event.needs_accept) {
      // Слоты считаются по ЖИВЫМ записям: закрытые удаляются в конце каждого круга.
      if (self.peers.size() >= self.config.max_peers + 2) {
        (void)self.link.close(event.peer);
        continue;
      }
      if (self.link.accept(event.peer) != network::gns_status::ok) continue;
      peer_state value;
      value.peer = event.peer;
      value.peer_id = self.next_peer_id++;
      value.handshake.emplace(self.compatibility, random_nonce());
      value.deadline = deadline_after(self.config.hello_timeout);
      self.peers.push_back(std::move(value));
      if (self.loud()) std::printf("authority: peer %llu accepted\n",
               static_cast<unsigned long long>(self.peers.back().peer_id));
      continue;
    }
    auto* peer = self.find(event.peer);
    if (peer == nullptr) continue;
    if (event.connected) peer->connected = true;
    if (event.terminal) {
      if (!peer->reported) {
        // Оборванное присоединение — тоже исход, и он записывается. Молчание в журнале о
        // пропавшем участнике неотличимо от «никто и не приходил».
        if (self.loud()) std::printf("authority: peer %llu dropped before reporting\n",
                 static_cast<unsigned long long>(peer->peer_id));
        peer->reported = true;
        ++self.refused;
      }
      peer->failed = true;
      self.close_now(*peer);
    }
  }

  // Приём. Разделение «рукопожатие или проектное сообщение» — по ФАЗЕ, а не по первому байту:
  // полоса упорядоченная, рукопожатие терминально, и назад оно не возвращается.
  self.link.receive([&](const network::gns_peer from, const uint16_t lane,
                        const std::span<const std::byte> payload) {
    auto* peer = self.find(from);
    if (peer == nullptr || peer->failed || !peer->handshake.has_value()) return;
    if (++peer->messages_this_step > self.config.messages_per_step) {
      self.violation(*peer);
      return;
    }

    if (lane == lane_intent) {
      // Предложение до подтверждённого присоединения — нарушение, а не «рано пришло».
      if (!peer->joined) {
        self.violation(*peer);
        return;
      }
      // Квота на участника: общий бюджет позволил бы одному заглушить ввод всем остальным.
      if (payload.empty() || payload.size() > network::intent_batch_max_bytes ||
          ++peer->intents_this_step > self.config.intents_per_step) {
        ++self.dropped_live_intents;
        self.violation(*peer);
        return;
      }
      // A bounded, owned copy crosses the transport -> simulation phase boundary.
      self.live_intents.push_back({peer->peer_id, {payload.begin(), payload.end()}});
      return;
    }
    if (lane != lane_control) { // клиент не имеет права слать по объёмной полосе
      self.violation(*peer);
      return;
    }

    if (!peer->handshake->established()) {
      state::admit_policy policy{self, *peer, source};
      const auto previous = peer->handshake->phase();
      const auto status = peer->handshake->consume(payload, self.reply, policy);
      if (!self.reply.empty()) {
        (void)self.link.send(from, lane_control, self.reply);
        self.reply.clear();
      }
      if (peer->handshake->phase() == network::handshake_phase::refused) {
        if (self.loud()) std::printf("authority: peer %llu refused: %s (wire status %u)\n",
                 static_cast<unsigned long long>(peer->peer_id),
                 describe(peer->handshake->refusal()), unsigned(status));
        peer->reported = true;
        ++self.refused;
        self.retire(*peer);
      } else if (peer->handshake->established()) {
        // Срок отчёта растёт с размером состояния, которое ещё предстоит передать.
        auto span = self.config.report_timeout;
        if (span.count() != 0 && self.config.report_min_bytes_per_second != 0)
          span += std::chrono::milliseconds(uint64_t(peer->header.total_bytes) * 1000 /
                                            self.config.report_min_bytes_per_second);
        peer->deadline = deadline_after(span);
        if (self.loud()) std::printf("authority: peer %llu established, start tick %llu\n",
                 static_cast<unsigned long long>(peer->peer_id),
                 static_cast<unsigned long long>(peer->handshake->accepted().start_tick));
      } else if (previous != peer->handshake->phase()) {
        peer->deadline = deadline_after(self.config.response_timeout);
      }
      return;
    }

    // После отчёта участник по управляющей полосе не говорит ничего: повтор отчёта и всё
    // прочее — нарушения.
    if (peer->reported || peek_message_type(payload) != uint8_t(message::join_report)) {
      self.violation(*peer);
      return;
    }
    join_report report;
    if (!decode(payload, report)) {
      self.violation(*peer);
      return;
    }
    peer->reported = true;
    peer->deadline = clock::time_point::max();

    authority_join join;
    join.peer_id = peer->peer_id;
    join.report = report;
    // Сравнение делает авторитет. Совпадение корня — это и есть «присоединился»: тот же мир,
    // посчитанный в другом процессе, а не просто успешно принятые байты.
    join.agreed = report.status == join_status::loaded && report.root == peer->header.root &&
                  report.tick == peer->header.tick;
    peer->joined = join.agreed;
    peer->document.clear();
    peer->document.shrink_to_fit(); // состояние доставлено; держать его копию незачем
    if (!join.agreed) ++self.refused;
    self.joins.push_back(join);
    if (self.loud()) std::printf("authority: peer %llu join %s (%s), tick %llu root %llu vs local %llu\n",
             static_cast<unsigned long long>(peer->peer_id), join.agreed ? "ok" : "FAILED",
             describe(report.status), static_cast<unsigned long long>(report.tick),
             static_cast<unsigned long long>(report.root),
             static_cast<unsigned long long>(peer->header.root));
    // The old join-only consumer observes close as its completion signal.
    // A live session keeps this exact authenticated route after agreement.
    if (!join.agreed || !self.config.keep_joined_connection) {
      peer->failed = true;
      self.close_now(*peer);
    }
  });

  // Передача. Отправляется столько, сколько принимает транспорт: упёрлись в бюджет полосы —
  // остановились и вернёмся на следующем тике. Объёмная полоса объявлена НИЖЕ приоритетом, так
  // что эта передача не может заткнуть собой управляющую.
  for (auto& peer : self.peers) {
    if (peer.closed || peer.failed || !peer.handshake.has_value() ||
        !peer.handshake->established())
      continue;

    if (!peer.world_sent) {
      if (!encode(self.world, self.scratch)) {
        utils::error{}("frontier_online authority: world declaration does not encode");
      }
      if (self.link.send(peer.peer, lane_control, self.scratch) != send_result::sent) continue;
      peer.world_sent = true;
    }

    if (!peer.begin_sent) {
      if (!encode(peer.header, self.scratch)) {
        utils::error{}("frontier_online authority: checkpoint header does not encode");
      }
      // Заголовок идёт по ТОЙ ЖЕ полосе, что и куски: между полосами порядка нет.
      if (self.link.send(peer.peer, lane_bulk, self.scratch) != send_result::sent) continue;
      peer.begin_sent = true;
    }

    while (peer.sent_bytes < peer.document.size()) {
      const size_t remaining = peer.document.size() - peer.sent_bytes;
      const size_t take = remaining < chunk_payload_bytes ? remaining : chunk_payload_bytes;
      const std::span<const std::byte> slice(peer.document.data() + peer.sent_bytes, take);
      if (!encode_chunk(peer.sent_bytes, slice, self.scratch)) {
        utils::error{}("frontier_online authority: checkpoint chunk does not encode");
      }
      if (self.link.send(peer.peer, lane_bulk, self.scratch) != send_result::sent) break;
      peer.sent_bytes += uint32_t(take);
      self.bytes_sent += take;
    }

    if (peer.joined && self.config.keep_joined_connection && !peer.confirmation_sent) {
      const join_confirmation confirmation{self.session_id, peer.peer_id,
                                            peer.header.tick, peer.header.root};
      if (!encode(confirmation, self.scratch))
        utils::error{}("frontier_online authority: join confirmation does not encode");
      if (self.link.send(peer.peer, lane_control, self.scratch) == send_result::sent)
        peer.confirmation_sent = true;
    }
  }

  // Сроки и отложенные закрытия. Отвергнутый участник живёт ровно столько, сколько нужно его
  // отказу, чтобы уйти; не успевший в срок закрывается; закрытые записи удаляются — иначе
  // каждый, кто хоть раз подключился, занимал бы слот навсегда.
  const auto now = clock::now();
  for (auto& peer : self.peers) {
    if (!peer.closed && !peer.failed && peer.deadline <= now)
      self.end(peer, termination_reason::timeout);
    self.send_notice(peer);
    if (!peer.closed && peer.close_at <= now) self.close_now(peer);
  }
  std::erase_if(self.peers, [](const peer_state& peer) { return peer.closed; });
}

void authority::close_all() {
  for (auto& peer : state_->peers) state_->close_now(peer);
  state_->peers.clear();
}

} // namespace net
} // namespace frontier_online
