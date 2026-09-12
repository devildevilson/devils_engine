#include "net/authority.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>

#include <devils_engine/utils/core.h>

#include "net/link.h"

namespace frontier_online {
namespace net {

namespace utils = devils_engine::utils;

namespace {

network::session_nonce random_nonce() {
  // Одноразовое число рукопожатия, а не ключ: оно связывает удостоверение с ЭТИМ обменом, и
  // потому берётся у системного источника, а не у детерминированного prng мира. Причинному
  // состоянию оно не принадлежит и на корень не влияет.
  std::random_device source;
  std::mt19937_64 engine(uint64_t(source()) << 32 | source());
  network::session_nonce value{};
  for (size_t i = 0; i < value.size(); i += 8) {
    const uint64_t word = engine();
    for (size_t b = 0; b < 8 && i + b < value.size(); ++b) {
      value[i + b] = std::byte(uint8_t(word >> (8 * b)));
    }
  }
  return value;
}

uint64_t random_session_id() {
  std::random_device source;
  return (uint64_t(source()) << 32) | source();
}

struct peer_state {
  network::gns_peer peer;
  // Закрытие ОТЛОЖЕНО. `gns_transport::close` закрывает БЕЗ linger — незабранные надёжные байты
  // пропадают, — а последнее, что авторитет говорит отвергнутому, это как раз причина отказа.
  // Закрыть сразу значило бы отвергать молча, ровно вопреки замыслу.
  std::chrono::steady_clock::time_point close_at = std::chrono::steady_clock::time_point::max();
  uint64_t peer_id = 0;
  bool connected = false;
  std::optional<network::authority_handshake> handshake;
  bool world_sent = false;
  bool begin_sent = false;
  uint32_t sent_bytes = 0;
  checkpoint_begin header;
  std::vector<std::byte> document;
  bool reported = false;
  bool failed = false;
};

} // namespace

struct authority::state {
  const core::causal_content& content;
  world_declaration world;
  authority_config config;
  network::session_compatibility compatibility;
  net_link link;
  uint16_t port = 0;
  uint64_t session_id = random_session_id();
  uint64_t next_peer_id = 1;
  std::vector<peer_state> peers;
  std::vector<authority_join> joins;
  uint32_t refused = 0;
  uint64_t bytes_sent = 0;
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
      if (owner.joins.size() >= owner.config.max_peers)
        return network::session_refusal_reason::no_capacity;

      // Чек-пойнт снимается ЗДЕСЬ, до того как клиент услышал «принят»: тик, с которого он
      // участвует, обязан быть тем самым тиком, чьё состояние ему поедет.
      peer.document.clear();
      if (!source.capture(peer.header, peer.document))
        return network::session_refusal_reason::no_capacity;

      accepted.session = owner.session_id;
      accepted.local_peer = peer.peer_id;
      accepted.authority_peer = 0;
      accepted.authority_epoch = 1;
      accepted.start_tick = peer.header.tick;
      return network::session_refusal_reason::none;
    }
  };

  peer_state* find(const network::gns_peer peer) {
    for (auto& value : peers) {
      if (value.peer == peer) return &value;
    }
    return nullptr;
  }

  bool loud() const noexcept {
    return config.verbose;
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
    if (peer.connected) ++count;
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

void authority::step(checkpoint_source& source) {
  auto& self = *state_;

  std::array<net_link::observation, 8> events;
  const size_t count = self.link.poll(events);
  for (size_t i = 0; i < count; ++i) {
    const auto& event = events[i];
    if (event.needs_accept) {
      if (self.peers.size() >= self.config.max_peers + 2) {
        (void)self.link.close(event.peer);
        continue;
      }
      if (self.link.accept(event.peer) != network::gns_status::ok) continue;
      peer_state value;
      value.peer = event.peer;
      value.peer_id = self.next_peer_id++;
      value.handshake.emplace(self.compatibility, random_nonce());
      self.peers.push_back(std::move(value));
      if (self.loud()) std::printf("authority: peer %llu accepted\n",
               static_cast<unsigned long long>(self.peers.back().peer_id));
      continue;
    }
    auto* peer = self.find(event.peer);
    if (peer == nullptr) continue;
    if (event.connected) peer->connected = true;
    if (event.terminal) {
      peer->connected = false;
      if (!peer->reported) {
        // Оборванное присоединение — тоже исход, и он записывается. Молчание в журнале о
        // пропавшем участнике неотличимо от «никто и не приходил».
        if (self.loud()) std::printf("authority: peer %llu dropped before reporting\n",
                 static_cast<unsigned long long>(peer->peer_id));
        peer->failed = true;
        peer->reported = true;
        ++self.refused;
      }
    }
  }

  // Приём. Разделение «рукопожатие или проектное сообщение» — по ФАЗЕ, а не по первому байту:
  // полоса упорядоченная, рукопожатие терминально, и назад оно не возвращается.
  self.link.receive([&](const network::gns_peer from, const uint16_t lane,
                        const std::span<const std::byte> payload) {
    auto* peer = self.find(from);
    if (peer == nullptr || !peer->handshake.has_value()) return;
    if (lane != lane_control) return; // клиент не имеет права слать сюда по другим полосам

    if (!peer->handshake->established() &&
        peer->handshake->phase() != network::handshake_phase::refused) {
      state::admit_policy policy{self, *peer, source};
      const auto status = peer->handshake->consume(payload, self.reply, policy);
      if (!self.reply.empty()) {
        (void)self.link.send(from, lane_control, self.reply);
        self.reply.clear();
      }
      if (peer->handshake->phase() == network::handshake_phase::refused) {
        if (self.loud()) std::printf("authority: peer %llu refused: %s (wire status %u)\n",
                 static_cast<unsigned long long>(peer->peer_id),
                 describe(peer->handshake->refusal()), unsigned(status));
        peer->failed = true;
        peer->reported = true;
        ++self.refused;
        peer->close_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      } else if (peer->handshake->established()) {
        if (self.loud()) std::printf("authority: peer %llu established, start tick %llu\n",
                 static_cast<unsigned long long>(peer->peer_id),
                 static_cast<unsigned long long>(peer->handshake->accepted().start_tick));
      }
      return;
    }

    if (!peer->handshake->established()) return;

    if (peek_message_type(payload) != uint8_t(message::join_report)) {
      if (self.loud()) std::printf("authority: peer %llu sent an unexpected control message\n",
               static_cast<unsigned long long>(peer->peer_id));
      return;
    }
    join_report report;
    if (!decode(payload, report)) {
      if (self.loud()) std::printf("authority: peer %llu sent a malformed join report\n",
               static_cast<unsigned long long>(peer->peer_id));
      return;
    }
    if (peer->reported) return;
    peer->reported = true;

    authority_join join;
    join.peer_id = peer->peer_id;
    join.report = report;
    // Сравнение делает авторитет. Совпадение корня — это и есть «присоединился»: тот же мир,
    // посчитанный в другом процессе, а не просто успешно принятые байты.
    join.agreed = report.status == join_status::loaded && report.root == peer->header.root &&
                  report.tick == peer->header.tick;
    if (!join.agreed) ++self.refused;
    self.joins.push_back(join);
    // Отчёт получен — обслуживать больше нечего, и соединение закрывается ЗДЕСЬ. Это же и есть
    // сигнал присоединившемуся, что его слышали: он ждёт терминального события, а не таймера.
    (void)self.link.close(from);
    peer->connected = false;
    if (self.loud()) std::printf("authority: peer %llu join %s (%s), tick %llu root %llu vs local %llu\n",
             static_cast<unsigned long long>(peer->peer_id), join.agreed ? "ok" : "FAILED",
             describe(report.status), static_cast<unsigned long long>(report.tick),
             static_cast<unsigned long long>(report.root),
             static_cast<unsigned long long>(peer->header.root));
  });

  // Передача. Отправляется столько, сколько принимает транспорт: упёрлись в бюджет полосы —
  // остановились и вернёмся на следующем тике. Объёмная полоса объявлена НИЖЕ приоритетом, так
  // что эта передача не может заткнуть собой управляющую.
  for (auto& peer : self.peers) {
    if (!peer.handshake.has_value() || !peer.handshake->established() || peer.failed) continue;

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
  }

  // Отложенные закрытия. Отвергнутый участник живёт ровно столько, сколько нужно его отказу,
  // чтобы уйти, и ни тиком дольше: иначе молчащий отвергнутый занимал бы слот навсегда.
  const auto now = std::chrono::steady_clock::now();
  for (auto& peer : self.peers) {
    if (peer.close_at <= now) {
      (void)self.link.close(peer.peer);
      peer.connected = false;
      peer.close_at = std::chrono::steady_clock::time_point::max();
      peer.peer = network::gns_peer{};
    }
  }
}

void authority::close_all() {
  for (auto& peer : state_->peers) {
    (void)state_->link.close(peer.peer);
  }
  state_->peers.clear();
}

} // namespace net
} // namespace frontier_online
