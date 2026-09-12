#include "net/follower.h"

#include <array>
#include <cstdio>
#include <optional>
#include <random>
#include <stdexcept>

#include <devils_engine/utils/core.h>

#include "net/link.h"

namespace frontier_online {
namespace net {

namespace utils = devils_engine::utils;

namespace {

network::session_nonce random_nonce() {
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
  std::vector<std::byte> document;
  uint32_t received = 0;

  join_report report;
  bool reported = false;

  std::vector<std::byte> scratch;
  std::vector<std::byte> reply;

  // Ответ клиента на вызов. Удостоверения у стенда ещё нет, и пустая строка байт здесь — это не
  // забытое место, а объявленный аноним: авторитет принимает РОВНО пустое.
  struct answer_policy {
    bool answer(const network::authority_challenge&, const utils::digest&,
                std::vector<std::byte>& credential) {
      credential.clear();
      return true;
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
    scratch.reserve(max_message_bytes);
    reply.reserve(max_message_bytes);
  }

  bool loud() const noexcept {
    return config.verbose;
  }

  void fail(const join_status status) {
    report.status = status;
    phase = follower_phase::failed;
  }

  // Проверка мира. Клиент не спорит с авторитетом о зерне — он принимает объявленное и проверяет,
  // что ПОЛУЧАЕТ то же самое. Расхождение здесь означает, что стороны считают разные миры, и
  // узнать это надо до первого тика, а не по картинке.
  void check_world() {
    world_checked = true;
    std::unique_ptr<core::terrain_source> source;
    try {
      source = content.make_terrain(world.generator, world.chunk_size, world.world_seed);
    } catch (const std::exception& error) {
      if (loud()) std::printf("follower: cannot build the declared generator: %s\n", error.what());
      fail(join_status::generator_missing);
      return;
    }

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

  void send_report() {
    if (reported) return;
    reported = true;
    if (!encode(report, scratch)) {
      utils::error{}("frontier_online follower: join report does not encode");
    }
    (void)link.send(peer, lane_control, scratch);
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
  return state_->received;
}

uint32_t follower::expected_bytes() const noexcept {
  return state_->header_seen ? state_->header.total_bytes : 0;
}

bool follower::start() {
  auto& self = *state_;
  if (!self.link.connect_to(self.config.host, self.config.port)) {
    if (self.loud()) std::printf("follower: connect refused by the transport\n");
    self.phase = follower_phase::failed;
    return false;
  }
  self.peer = self.link.pending();
  return true;
}

bool follower::drain() {
  auto& self = *state_;
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
  if (self.phase == follower_phase::failed || self.phase == follower_phase::joined) return;

  std::array<net_link::observation, 4> events;
  const size_t count = self.link.poll(events);
  for (size_t i = 0; i < count; ++i) {
    if (!(events[i].peer == self.peer)) continue;
    if (events[i].terminal) {
      // Обрыв — это исход, а не пауза. Если мы ещё не знаем причины, ею останется та, что была.
      if (self.phase != follower_phase::joined) {
        if (self.loud()) std::printf("follower: connection closed by the authority\n");
        self.phase = follower_phase::failed;
      }
      return;
    }
    if (events[i].connected && self.phase == follower_phase::connecting) {
      // Рукопожатие начинает КЛИЕНТ, и только после того, как соединение действительно есть.
      self.handshake.emplace(self.compatibility, random_nonce());
      if (self.handshake->start(self.scratch) != network::session_wire_status::ok) {
        utils::error{}("frontier_online follower: client hello does not encode");
      }
      if (self.link.send(self.peer, lane_control, self.scratch) != send_result::sent) {
        if (self.loud()) std::printf("follower: could not send the hello\n");
        self.phase = follower_phase::failed;
        return;
      }
      self.phase = follower_phase::handshaking;
    }
  }

  self.link.receive([&](const network::gns_peer from, const uint16_t lane,
                        const std::span<const std::byte> payload) {
    if (!(from == self.peer) || !self.handshake.has_value()) return;

    if (!self.handshake->established()) {
      if (lane != lane_control) return;
      state::answer_policy policy;
      const auto status = self.handshake->consume(payload, self.reply, policy);
      if (!self.reply.empty()) {
        (void)self.link.send(self.peer, lane_control, self.reply);
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
        self.phase = follower_phase::awaiting_world;
        if (self.loud()) {
          std::printf("follower: established, session %llu peer %llu start tick %llu\n",
                      static_cast<unsigned long long>(self.handshake->accepted().session),
                      static_cast<unsigned long long>(self.handshake->accepted().local_peer),
                      static_cast<unsigned long long>(self.handshake->accepted().start_tick));
        }
      }
      return;
    }

    if (self.phase == follower_phase::failed || self.phase == follower_phase::joined) return;

    if (lane == lane_control) {
      if (peek_message_type(payload) != uint8_t(message::world_declaration)) return;
      if (self.world_checked) return;
      if (!decode(payload, self.world)) {
        if (self.loud()) std::printf("follower: malformed world declaration\n");
        self.fail(join_status::generator_missing);
        return;
      }
      self.check_world();
      return;
    }

    if (lane != lane_bulk) return;

    const auto type = peek_message_type(payload);
    if (type == uint8_t(message::checkpoint_begin)) {
      if (self.header_seen) return;
      if (!decode(payload, self.header)) {
        self.fail(join_status::checkpoint_refused);
        return;
      }
      self.header_seen = true;
      self.document.assign(self.header.total_bytes, std::byte{0});
      self.received = 0;
      return;
    }
    if (type != uint8_t(message::checkpoint_chunk)) return;
    if (!self.header_seen) {
      // Полоса упорядоченная, поэтому кусок раньше заголовка — это не гонка, а протокольная
      // ошибка, и молчать о ней нельзя.
      if (self.loud()) std::printf("follower: checkpoint chunk before its header\n");
      self.fail(join_status::checkpoint_refused);
      return;
    }
    chunk_view chunk;
    if (!decode_chunk(payload, chunk)) {
      self.fail(join_status::checkpoint_refused);
      return;
    }
    if (size_t(chunk.offset) + chunk.payload.size() > self.document.size()) {
      self.fail(join_status::checkpoint_refused);
      return;
    }
    std::copy(chunk.payload.begin(), chunk.payload.end(), self.document.begin() + chunk.offset);
    self.received += uint32_t(chunk.payload.size());
  });

  if (self.phase == follower_phase::failed) {
    self.send_report();
    return;
  }

  // Документ собран. Загрузка — на границе приёма, не в обработчике сообщения: там ещё живёт
  // аренда на чужие байты.
  if (self.phase == follower_phase::receiving_state && self.header_seen &&
      self.received == self.header.total_bytes) {
    self.report.tick = self.header.tick;
    uint64_t root = 0;
    if (!sink.load(self.header, self.document, root)) {
      self.fail(join_status::checkpoint_refused);
    } else {
      self.report.root = root;
      if (root != self.header.root) {
        self.fail(join_status::root_mismatch);
      } else {
        self.report.status = join_status::loaded;
        self.phase = follower_phase::joined;
      }
    }
    if (self.loud()) {
      std::printf("follower: checkpoint %u bytes at tick %llu, root %llu (authority %llu): %s\n",
                  self.header.total_bytes,
                  static_cast<unsigned long long>(self.header.tick),
                  static_cast<unsigned long long>(self.report.root),
                  static_cast<unsigned long long>(self.header.root), describe(self.report.status));
    }
    self.send_report();
  }
}

} // namespace net
} // namespace frontier_online
