#include "net/authority.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <optional>

#include <devils_engine/network/canonical_tick_wire.h>
#include <devils_engine/network/credential.h>
#include <devils_engine/network/reconnect.h>
#include <devils_engine/utils/core.h>

#include "net/link.h"
#include <devils_engine/network/secure_random.h>

namespace frontier_online {
namespace net {

namespace utils = devils_engine::utils;
using clock = std::chrono::steady_clock;

namespace {

network::session_nonce random_nonce() {
  network::session_nonce value{};
  devils_engine::network::secure_random_bytes(value);
  return value;
}


clock::time_point deadline_after(const std::chrono::milliseconds span) {
  return span.count() == 0 ? clock::time_point::max() : clock::now() + span;
}

struct peer_state {
  network::gns_peer peer;
  uint64_t peer_id = 0;
  // Сессия участника — своя у каждого: на неё выдаётся билет повторного входа и держится слот.
  // 0 — ещё не выдана (рукопожатие не завершено).
  uint64_t session = 0;
  // Рукопожатие с продолжением: вместо объявления мира — план, вместо снимка «сейчас» — то, что
  // выбрал источник (якорь в истории или снимок из кольца).
  std::optional<resume_mode> resume;
  bool plan_sent = false;
  // Исход записи для слота уже подведён (удержан или освобождён): второй раз не подводится.
  bool settled = false;
  bool grant_sent = false;
  clock::time_point grant_at{};
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
  // Ответ рукопожатия (в том числе отказ с причиной), ещё не принятый транспортом. Повторяется
  // каждый круг, как и последнее слово; пока он не ушёл, дальше по управляющей полосе ничего не
  // отправляется — порядок полосы и есть порядок фаз.
  std::vector<std::byte> handshake_reply;
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
  uint64_t session_id = devils_engine::network::secure_random_nonzero_u64(); // 0 = «сессии нет»
  uint64_t next_peer_id = 1;
  std::vector<peer_state> peers;
  // Журналы исходов ограничены: долгая сессия с повторными входами иначе копила бы их без бюджета,
  // а живой цикл перебирает `joins` каждый круг. Счётчики итогов ведутся отдельно и не теряются.
  std::vector<authority_join> joins;
  std::vector<authority_termination> terminations;
  uint32_t agreed_joins = 0;
  uint64_t termination_count = 0;
  uint32_t refused = 0;
  uint64_t violations = 0;
  uint64_t bytes_sent = 0;
  std::vector<live_intent_packet> live_intents;
  uint64_t dropped_live_intents = 0;
  std::vector<std::byte> scratch;
  std::vector<std::byte> reply;

  // ПОВТОРНЫЙ ВХОД. Ключ билетов — случайный на процесс. `issued` — сессии участников, чьи игроки
  // сейчас в мире (живые и удерживаемые): только по ним принимается билет. `hold_until` — конец
  // окна после первого обрыва (0 — участник жив); неудачная попытка вернуться окно не продлевает.
  struct issued_session {
    uint64_t session = 0;
    uint64_t peer_id = 0;
    uint64_t hold_until = 0;
  };
  network::credential_mac mac_key{};
  credential_policy mac{&mac_key};
  network::credential_scratch credential_scratch;
  network::session_hold_table holds;
  std::vector<issued_session> issued;
  clock::time_point origin = clock::now();
  uint32_t resumed = 0;

  state(net_runtime& runtime, const core::causal_content& c, const world_declaration& w,
        const authority_config cfg)
    : content(c), world(w), config(cfg), compatibility(local_compatibility(c)),
      link(runtime, cfg.max_peers), holds(cfg.max_peers) {
    devils_engine::network::secure_random_bytes(mac_key);
    const auto outcome = config.port == 0 ? link.listen_any(config.host == 0 ? 0x7f000001
                                                                            : config.host)
                                          : link.listen_on(config.port, config.host);
    port = outcome.port;
    scratch.reserve(max_message_bytes);
    reply.reserve(max_message_bytes);
  }

  // Политика авторитета: два решения, которые библиотека отказывается угадывать.
  //
  // ЛИЧНОСТИ ПРИ ПЕРВОМ ВХОДЕ НЕТ: принимается РОВНО пустое удостоверение — «аноним». Непустое —
  // только билет повторного входа (`network/credential.h`), выданный этим авторитетом этой сессии.
  struct admit_policy {
    state& owner;
    peer_state& peer;
    checkpoint_source& source;

    bool issue_challenge(const network::client_hello&, std::vector<std::byte>& challenge) {
      challenge.clear(); // вызов не нужен: присутствие доказывает подпись нового транскрипта
      return true;
    }

    network::session_refusal_reason admit(const network::client_response& response,
                                          const utils::digest& transcript,
                                          network::session_accepted& accepted) {
      if (response.resumed_session) return admit_resume(response, transcript, accepted);
      if (!response.credential.empty()) return network::session_refusal_reason::identity_rejected;
      // Ёмкость считается по участникам, которые СЕЙЧАС заняты сессией (передача, ожидание
      // отчёта, живая игра), и по удерживаемым слотам: их игроки стоят в мире и вернутся.
      if (owner.occupied() >= owner.config.max_peers)
        return network::session_refusal_reason::no_capacity;

      // Чек-пойнт снимается ЗДЕСЬ, до того как клиент услышал «принят»: тик, с которого он
      // участвует, обязан быть тем самым тиком, чьё состояние ему поедет.
      // Отказ ниже — свой предел (мир вырос больше, чем пролезает checkpoint), а не «сервер полон»:
      // участнику уходит отказ, а причина пишется всегда, не только с --verbose.
      peer.document.clear();
      if (!source.capture(peer.header, peer.document)) {
        std::fprintf(stderr, "authority: refused a join: the checkpoint could not be captured\n");
        return network::session_refusal_reason::no_capacity;
      }
      if (peer.document.empty() || peer.document.size() > max_checkpoint_bytes ||
          peer.header.total_bytes != peer.document.size()) {
        std::fprintf(stderr, "authority: refused a join: checkpoint of %zu bytes (limit %zu)\n",
                     peer.document.size(), size_t(max_checkpoint_bytes));
        return network::session_refusal_reason::no_capacity;
      }

      peer.session = owner.fresh_session();
      accepted.session = peer.session;
      accepted.local_peer = peer.peer_id;
      accepted.authority_peer = 0;
      accepted.authority_epoch = 1;
      accepted.start_tick = peer.header.tick;
      return network::session_refusal_reason::none;
    }

    // Билет проверяется ДО любых ответов о сессии: чужой не узнаёт, какие сессии есть.
    network::session_refusal_reason admit_resume(const network::client_response& response,
                                                 const utils::digest& transcript,
                                                 network::session_accepted& accepted) {
      network::reconnect_credential credential;
      if (network::try_decode_credential(response.credential, credential) !=
          network::credential_status::accepted)
        return network::session_refusal_reason::identity_rejected;
      const uint64_t session = *response.resumed_session;
      auto* record = owner.find_issued(session);
      if (record == nullptr) return network::session_refusal_reason::unknown_session;
      const uint64_t now = owner.now_ms();
      if (network::verify_reconnect_credential(credential, {session, record->peer_id, 1}, now,
                                               transcript, owner.mac, owner.credential_scratch) !=
          network::credential_status::accepted)
        return network::session_refusal_reason::identity_rejected;
      // Прежнее соединение ещё числится (клиент заметил обрыв раньше авторитета) — переносится.
      // Иначе слот обязан удерживаться.
      peer_state* old = owner.find_session_record(session, &peer);
      if (old == nullptr &&
          owner.holds.resolve(session, record->peer_id, 1, now) !=
            network::session_hold_table::resolve_status::held)
        return network::session_refusal_reason::unknown_session;

      std::optional<resume_anchor> anchor;
      if (response.confirmed) {
        resume_anchor value{response.confirmed->tick, 0};
        if (anchor_root(response.confirmed->root, value.root)) anchor = value;
      }
      resume_mode mode = resume_mode::journal;
      peer.document.clear();
      if (!source.resume(record->peer_id, anchor, mode, peer.header, peer.document) ||
          peer.document.size() > max_checkpoint_bytes ||
          peer.header.total_bytes != peer.document.size() ||
          (mode == resume_mode::checkpoint) == peer.document.empty()) {
        // Догнать не из чего: игрок уходит, вернуться можно только новым входом.
        if (old != nullptr) {
          old->failed = true;
          owner.close_now(*old);
        }
        owner.release_session(session);
        return network::session_refusal_reason::unknown_session;
      }
      if (old != nullptr) {
        old->failed = true;
        old->settled = true; // сессию продолжает новое соединение
        owner.close_now(*old);
      }
      peer.peer_id = record->peer_id;
      peer.session = session;
      peer.resume = mode;
      peer.world_sent = true; // мир у клиента уже есть
      peer.begin_sent = mode == resume_mode::journal;
      accepted.session = session;
      accepted.local_peer = peer.peer_id;
      accepted.authority_peer = 0;
      accepted.authority_epoch = 1;
      accepted.start_tick = peer.header.tick;
      return network::session_refusal_reason::none;
    }
  };

  uint64_t now_ms() const noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - origin)
                      .count()) + 1;
  }

  issued_session* find_issued(const uint64_t session) noexcept {
    for (auto& value : issued)
      if (value.session == session) return &value;
    return nullptr;
  }

  const issued_session* issued_for_peer(const uint64_t peer_id) const noexcept {
    for (const auto& value : issued)
      if (value.peer_id == peer_id) return &value;
    return nullptr;
  }

  // Действующая запись этой сессии (кроме `except`): живое соединение или идущее продолжение.
  peer_state* find_session_record(const uint64_t session, const peer_state* except = nullptr) {
    for (auto& value : peers)
      if (&value != except && !value.failed && !value.closed && value.session == session)
        return &value;
    return nullptr;
  }

  bool session_active(const uint64_t session) const noexcept {
    return std::any_of(peers.begin(), peers.end(), [&](const peer_state& value) {
      return !value.failed && !value.closed && value.session == session;
    });
  }

  uint64_t fresh_session() const {
    for (;;) {
      const uint64_t value = devils_engine::network::secure_random_nonzero_u64();
      if (std::none_of(issued.begin(), issued.end(), [&](const auto& v) { return v.session == value; }) &&
          std::none_of(peers.begin(), peers.end(), [&](const auto& v) { return v.session == value; }))
        return value;
    }
  }

  void release_session(const uint64_t session) {
    holds.release(session);
    std::erase_if(issued, [&](const issued_session& value) { return value.session == session; });
  }

  // Исход записи для слота: удержать (обрыв, срок, отставание) или освободить (остальное). Только
  // у участника, чей игрок в мире (`issued`); у не дошедшего до мира слота нет.
  void settle(peer_state& peer, const bool keep) {
    if (peer.settled || peer.session == 0) return;
    peer.settled = true;
    auto* record = find_issued(peer.session);
    if (record == nullptr) return;
    if (!keep || config.hold_window.count() <= 0) return release_session(peer.session);
    const uint64_t now = now_ms();
    if (record->hold_until == 0)
      record->hold_until = now + uint64_t(config.hold_window.count());
    else if (now >= record->hold_until)
      return release_session(peer.session); // окно первого обрыва истекло: продлений нет
    holds.release(peer.session);
    const auto status = holds.hold(peer.session, record->peer_id, 1, now, record->hold_until);
    if (status != network::session_hold_table::hold_status::held) {
      // Мест не больше max_peers (occupied), так что сюда не попасть; если всё же попали, это не
      // повод ронять сессию остальным: игрок уходит, как после истёкшего окна.
      std::fprintf(stderr, "authority: hold of peer %llu refused (status %u, %zu of %zu held); the player leaves\n",
                   static_cast<unsigned long long>(record->peer_id), unsigned(status), holds.size(),
                   holds.capacity());
      return release_session(peer.session);
    }
    if (loud()) std::printf("authority: peer %llu held until %llu ms\n",
                            static_cast<unsigned long long>(record->peer_id),
                            static_cast<unsigned long long>(record->hold_until));
  }

  // Истёкшие окна: игрок уходит (канонический уход пишет live-слой, увидев, что участник не жив и
  // не удерживается). Идущее продолжение держит сессию само.
  void expire_holds() {
    const uint64_t now = now_ms();
    holds.reap(now);
    std::erase_if(issued, [&](const issued_session& value) {
      if (value.hold_until == 0 || now < value.hold_until || session_active(value.session))
        return false;
      if (loud()) std::printf("authority: hold of peer %llu expired\n",
                              static_cast<unsigned long long>(value.peer_id));
      return true;
    });
  }

  // Занятые места: установленные соединения нового входа плюс игроки в мире (`issued`), за которыми
  // нет такого соединения — удерживаемые, продолжающие и те, чьё старое соединение уже закрыто при
  // переносе. Продолжение не своё место, а место своего игрока: считать его как соединение
  // нельзя (место посчитано за игроком), не считать вовсе тоже нельзя — иначе в окно продолжения
  // вошёл бы лишний участник, а при обрыве обоих таблица удержаний переполнилась бы.
  uint32_t occupied() const noexcept {
    const auto live_entry = [](const peer_state& value) {
      return !value.failed && !value.closed && !value.resume && value.handshake.has_value() &&
             value.handshake->established();
    };
    uint32_t count = 0;
    for (const auto& value : peers)
      if (live_entry(value)) ++count;
    for (const auto& value : issued) {
      const bool counted = std::any_of(peers.begin(), peers.end(), [&](const peer_state& peer) {
        return live_entry(peer) && peer.session == value.session;
      });
      if (!counted) ++count;
    }
    return count;
  }

  bool send_grant(peer_state& peer) {
    const uint64_t now = now_ms();
    network::reconnect_ticket ticket;
    ticket.session = peer.session;
    ticket.principal = peer.peer_id;
    ticket.authority_epoch = 1;
    ticket.issued_at = now;
    ticket.expires_at = now + uint64_t(config.ticket_lifetime.count());
    ticket.issuer = credential_issuer;
    reconnect_grant grant;
    grant.session = peer.session;
    grant.logical_peer = peer.peer_id;
    grant.hold_ms = uint32_t(config.hold_window.count());
    if (network::issue_reconnect_ticket(ticket, mac, credential_scratch, grant.credential) !=
          network::credential_status::accepted ||
        network::derive_session_secret(credential_issuer, peer.session, peer.peer_id, mac,
                                       credential_scratch, grant.secret) !=
          network::credential_status::accepted ||
        !encode(grant, scratch))
      utils::error{}("frontier_online authority: reconnect grant of peer {} does not issue "
                     "(ticket {} ms, hold {} ms)", peer.peer_id, config.ticket_lifetime.count(),
                     config.hold_window.count());
    return link.send(peer.peer, lane_control, scratch) == send_result::sent;
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
    ++termination_count;
    if (terminations.size() >= journal_limit()) terminations.erase(terminations.begin());
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
    settle(peer, reason == termination_reason::timeout ||
                   reason == termination_reason::history_exhausted);
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

  void send_handshake_reply(peer_state& peer) {
    if (peer.handshake_reply.empty() || peer.closed) return;
    if (link.send(peer.peer, lane_control, peer.handshake_reply) != send_result::sent) return;
    peer.handshake_reply.clear();
    // Отказ — последнее слово отвергнутого: linger отсчитывается от момента, когда он ушёл.
    if (peer.failed)
      peer.close_at = std::max(peer.close_at == clock::time_point::max() ? clock::time_point::min()
                                                                         : peer.close_at,
                               deadline_after(config.close_linger));
  }

  size_t journal_limit() const noexcept {
    return 4 * size_t(config.max_peers) + 16;
  }

  // Запись присоединения нужна, пока жив её участник; ушедших выбрасываем, когда журнал упёрся
  // в предел (короткий прогон и тесты видят его целиком).
  void trim_joins() {
    if (joins.size() <= journal_limit()) return;
    std::erase_if(joins, [this](const authority_join& join) {
      return std::none_of(peers.begin(), peers.end(), [&](const peer_state& peer) {
        return peer.peer_id == join.peer_id && !peer.closed;
      });
    });
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
  return state_->agreed_joins;
}

uint64_t authority::session_of(const uint64_t logical_peer) const noexcept {
  const auto* record = state_->issued_for_peer(logical_peer);
  return record != nullptr ? record->session : 0;
}

uint32_t authority::resumed_joins() const noexcept {
  return state_->resumed;
}

bool authority::is_held(const uint64_t logical_peer) const noexcept {
  if (is_live_member(logical_peer)) return false;
  const auto* record = state_->issued_for_peer(logical_peer);
  if (record == nullptr) return false;
  // Удержание или идущее продолжение (в том числе согласованное, но ещё не подтверждённое).
  return record->hold_until != 0 || state_->session_active(record->session);
}

uint64_t authority::termination_count() const noexcept {
  return state_->termination_count;
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
    const catchup_marker marker{peer.session, logical_peer, tick};
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
  self.expire_holds();
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
      if (event.invalid) {
        // Нарушение контракта полос — исход участника, как любое нарушение: без удержания.
        ++self.violations;
        ++peer->violations;
        self.end(*peer, termination_reason::violations);
        continue;
      }
      peer->failed = true;
      self.close_now(*peer);
      self.settle(*peer, true); // обрыв: слот держится, игрок стоит в мире
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
      if (payload.empty() || payload.size() > network::intent_batch_max_bytes) {
        ++self.dropped_live_intents;
        self.violation(*peer);
        return;
      }
      // Сверх квоты — отбросить. Нарушение — только поток: больше учетверённой квоты за круг, и не
      // чаще раза за круг. Честный клиент повторяет неподтверждённое раз в свой период, и пачка от
      // джиттера или заминки сервера превышает квоту без умысла; порог нарушений (16 кругов
      // потока) такие пачки не набирают, а флудер набирает.
      if (++peer->intents_this_step > self.config.intents_per_step) {
        ++self.dropped_live_intents;
        if (peer->intents_this_step == 4 * self.config.intents_per_step + 1) self.violation(*peer);
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
        peer->handshake_reply.assign(self.reply.begin(), self.reply.end());
        self.reply.clear();
        self.send_handshake_reply(*peer);
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

    // После отчёта участник по управляющей полосе говорит только «ухожу»; повтор отчёта и всё
    // прочее — нарушения.
    if (peer->reported && peer->joined && is_leave(payload)) {
      if (self.loud()) std::printf("authority: peer %llu left\n",
                                   static_cast<unsigned long long>(peer->peer_id));
      peer->failed = true;
      peer->deadline = clock::time_point::max();
      self.settle(*peer, false); // ушёл сам: слот освобождается, игрок уходит каноническим тиком
      self.close_now(*peer);     // закрытие — ответ клиенту, который ждёт его в drain()
      return;
    }
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
    join.resumed = peer->resume.has_value();
    // Сравнение делает авторитет. Совпадение корня — это и есть «присоединился»: тот же мир,
    // посчитанный в другом процессе, а не просто успешно принятые байты.
    join.agreed = report.status == join_status::loaded && report.root == peer->header.root &&
                  report.tick == peer->header.tick;
    // Продолжение согласовано, только если его игрок всё ещё в мире.
    auto* issued = self.find_issued(peer->session);
    if (join.resumed && join.agreed && (issued == nullptr || issued->peer_id != peer->peer_id))
      join.agreed = false;
    peer->joined = join.agreed;
    peer->document.clear();
    peer->document.shrink_to_fit(); // состояние доставлено; держать его копию незачем
    if (!join.agreed) {
      ++self.refused;
    } else if (join.resumed) {
      ++self.resumed;
      issued->hold_until = 0;
      self.holds.release(peer->session);
      // Запись прежнего соединения того же участника больше не описывает живой маршрут.
      std::erase_if(self.joins, [&](const authority_join& value) { return value.peer_id == peer->peer_id; });
    } else {
      ++self.agreed_joins;
      if (self.config.keep_joined_connection)
        self.issued.push_back({peer->session, peer->peer_id, 0});
    }
    self.joins.push_back(join);
    if (self.loud()) std::printf("authority: peer %llu join %s (%s), tick %llu root %llu vs local %llu\n",
             static_cast<unsigned long long>(peer->peer_id), join.agreed ? "ok" : "FAILED",
             describe(report.status), static_cast<unsigned long long>(report.tick),
             static_cast<unsigned long long>(report.root),
             static_cast<unsigned long long>(peer->header.root));
    // The old join-only consumer observes close as its completion signal.
    // A live session keeps this exact authenticated route after agreement.
    if (!join.agreed) {
      // Несогласие — с последним словом, как любой конец сессии: иначе клиент, считающий свой вход
      // удачным, увидел бы закрытие без причины и стал бы повторять. Слот освобождается (end).
      self.end(*peer, termination_reason::disagreement);
    } else if (!self.config.keep_joined_connection) {
      peer->failed = true;
      self.close_now(*peer);
      self.settle(*peer, false);
    }
  });

  // Передача. Отправляется столько, сколько принимает транспорт: упёрлись в бюджет полосы —
  // остановились и вернёмся на следующем тике. Объёмная полоса объявлена НИЖЕ приоритетом, так
  // что эта передача не может заткнуть собой управляющую.
  for (auto& peer : self.peers) {
    self.send_handshake_reply(peer);
    if (peer.closed || peer.failed || !peer.handshake.has_value() ||
        !peer.handshake->established() || !peer.handshake_reply.empty())
      continue;

    if (peer.resume && !peer.plan_sent) {
      const resume_plan plan{peer.session, peer.peer_id, *peer.resume, peer.header.tick,
                             peer.header.root};
      if (!encode(plan, self.scratch))
        utils::error{}("frontier_online authority: resume plan of peer {} at tick {} does not encode",
                       peer.peer_id, peer.header.tick);
      if (self.link.send(peer.peer, lane_control, self.scratch) != send_result::sent) continue;
      peer.plan_sent = true;
    }

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
      const join_confirmation confirmation{peer.session, peer.peer_id,
                                            peer.header.tick, peer.header.root};
      if (!encode(confirmation, self.scratch))
        utils::error{}("frontier_online authority: join confirmation does not encode");
      if (self.link.send(peer.peer, lane_control, self.scratch) == send_result::sent)
        peer.confirmation_sent = true;
    }

    // Ключ повторного входа: после подтверждения и заново на половине срока билета.
    if (peer.confirmation_sent && peer.session != 0 && self.find_issued(peer.session) != nullptr &&
        (!peer.grant_sent || clock::now() >= peer.grant_at + self.config.ticket_lifetime / 2) &&
        self.send_grant(peer)) {
      peer.grant_sent = true;
      peer.grant_at = clock::now();
    }
  }

  // Сроки и отложенные закрытия. Отвергнутый участник живёт ровно столько, сколько нужно его
  // отказу, чтобы уйти; не успевший в срок закрывается; закрытые записи удаляются — иначе
  // каждый, кто хоть раз подключился, занимал бы слот навсегда.
  const auto now = clock::now();
  for (auto& peer : self.peers) {
    if (!peer.closed && !peer.failed && peer.deadline <= now)
      self.end(peer, termination_reason::timeout);
    self.send_handshake_reply(peer);
    self.send_notice(peer);
    if (!peer.closed && peer.close_at <= now) self.close_now(peer);
  }
  std::erase_if(self.peers, [](const peer_state& peer) { return peer.closed; });
  self.trim_joins();
}

void authority::close_all() {
  for (auto& peer : state_->peers) state_->close_now(peer);
  state_->peers.clear();
}

} // namespace net
} // namespace frontier_online
