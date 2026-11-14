#include "net/live_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <devils_engine/network/bounded_history.h>
#include <devils_engine/network/canonical_tick_wire.h>
#include <devils_engine/network/replay.h>
#include <devils_engine/network/sequence_window.h>
#include <devils_engine/network/state_digest.h>
#include <devils_engine/network/tick_journal.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/timeline.h>
#include <spdlog/spdlog.h>

#include "core/actor_checkpoint.h"
#include "core/actor_simulation.h"
#include "core/causal_content.h"
#include "net/player_intent_wire.h"

namespace frontier_online::net {
namespace {

namespace network = devils_engine::network;
namespace utils = devils_engine::utils;
using clock = std::chrono::steady_clock;

// Сколько bundles за один сетевой круг уходит одному участнику при догоне.
constexpr size_t bundles_per_service = 8;
// Окно дубликатов номеров ввода на участника. Клиент держит в полёте не больше
// `max_outstanding_inputs`, поэтому честный повтор всегда попадает в окно.
constexpr size_t input_window_bits = 64;
constexpr size_t max_outstanding_inputs = 16;
static_assert(max_outstanding_inputs < input_window_bits);
// Сколько раз клиент повторяет неподтверждённое предложение — раз в период тика сессии. Класс
// ненадёжный: потеря лечится избыточностью, а не повторной отправкой по запросу. Период —
// по часам, а не по своим тикам: при потерях задерживаются и сами bundles, и повтор, ждущий
// своего тика, встал бы ровно тогда, когда он нужен.
constexpr uint32_t input_resend_limit = 8;

void require(const bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}

struct host {
  core::actor_world_slice actors;
  utils::timelines clocks;
  devils_engine::thread::atomic_pool pool;
  uint64_t expected_root = 0;
  uint64_t pending_tick = 0;

  host(const uint32_t rate, const uint32_t workers)
    : clocks(utils::simulation_rate(rate)), pool(workers) {}
};

void step(host& value, const uint64_t number) {
  const auto tick = value.clocks.simulation_now() + utils::simulation_duration{1};
  require(tick.value == number, "live session skipped a simulation tick");
  value.actors.update(tick, value.clocks.advance_simulation(tick), nullptr, value.pool);
}

uint64_t root_of(const host& value) {
  core::actor_checkpoint_buffers buffers;
  network::state_digest_report<uint64_t> digest;
  digest.sections.reserve(core::actor_checkpoint_section_count);
  require(core::actor_checkpoint_digest(value.actors, value.clocks, buffers, digest),
          "live session could not digest its causal state");
  return digest.root;
}

class source final : public checkpoint_source {
public:
  explicit source(const host& value) : value_(value) {}

  bool capture(checkpoint_begin& header, std::vector<std::byte>& document) override {
    core::actor_checkpoint_buffers buffers;
    network::state_digest_report<uint64_t> digest;
    digest.sections.reserve(core::actor_checkpoint_section_count);
    if (!core::actor_checkpoint_digest(value_.actors, value_.clocks, buffers, digest) ||
        buffers.document.size() > UINT32_MAX) return false;
    header = {value_.clocks.simulation_now().value, digest.root,
              uint32_t(buffers.document.size()), core::actor_checkpoint_schema_fingerprint()};
    document = std::move(buffers.document);
    return true;
  }

private:
  const host& value_;
};

class sink final : public checkpoint_sink {
public:
  sink(host& value, const core::brain_config& brains) : value_(value), brains_(brains) {}

  bool load(const checkpoint_begin& header, const std::span<const std::byte> document,
            uint64_t& root) override {
    if (header.schema_fingerprint != core::actor_checkpoint_schema_fingerprint() ||
        !core::load_actor_checkpoint(value_.actors, value_.clocks, document, brains_).loaded())
      return false;
    checkpoint.assign(document.begin(), document.end());
    root = root_of(value_);
    return true;
  }

  std::vector<std::byte> checkpoint;

private:
  host& value_;
  const core::brain_config& brains_;
};

devils_engine::act::intent food_action(const double x, const double y) {
  devils_engine::act::intent action;
  action.kind = devils_engine::act::intent_kind::spawn_prefab;
  action.payload.spawn.prefab = utils::string_hash("food");
  action.payload.spawn.target = devils_engine::act::vec3{x, y, 0.0};
  action.source_action = utils::string_hash("network_spawn_food");
  return action;
}

std::vector<std::byte> action_payload(const devils_engine::act::intent& action) {
  std::vector<std::byte> bytes;
  require(encode(canonical_food_action{action.payload.spawn.target.x, action.payload.spawn.target.y},
                 bytes),
          "live action payload did not encode");
  return bytes;
}

bool enqueue_payload(host& target, const std::span<const std::byte> bytes) {
  canonical_food_action action;
  return decode(bytes, action) &&
         target.actors.enqueue_canonical_intent(food_action(action.x, action.y));
}

struct accepted_input {
  uint64_t tick = 0;
  uint64_t peer = 0;
  uint64_t sequence = 0;
  devils_engine::act::intent action;
  std::vector<std::byte> payload;
};

struct input_tick {
  uint64_t operator()(const accepted_input& value) const noexcept {
    return value.tick;
  }
};

struct input_less {
  bool operator()(const accepted_input& a, const accepted_input& b) const noexcept {
    if (a.peer != b.peer) return a.peer < b.peer;
    return a.sequence < b.sequence;
  }
};

struct input_same {
  bool operator()(const accepted_input& a, const accepted_input& b) const noexcept {
    return a.peer == b.peer && a.sequence == b.sequence;
  }
};

using input_journal = network::tick_journal<accepted_input, uint64_t, input_tick,
                                            input_less, input_same>;

struct wire_tick {
  uint64_t tick = 0;
  std::vector<std::byte> bundle;
};

// `on_own(sequence)` — запись этого участника: так ведомый узнаёт, какие его предложения
// авторитет принял.
template <class OnOwn>
bool apply_bundle(host& target, const std::span<const std::byte> bytes,
                  const uint64_t local_peer, OnOwn&& on_own) {
  std::array<network::canonical_tick_record_view, network::canonical_tick_max_records> storage{};
  network::canonical_tick_view view;
  if (network::try_decode_canonical_tick(bytes, target.clocks.simulation_now().value, storage,
                                         view) !=
        network::canonical_tick_wire_status::ok ||
      !view.state_root ||
      view.tick != target.clocks.simulation_now().value + 1) return false;
  target.pending_tick = view.tick;
  target.expected_root = *view.state_root;
  for (const auto& record : view.records) {
    if (!enqueue_payload(target, record.payload)) return false;
    if (record.origin_peer == local_peer) on_own(record.input_sequence);
  }
  return true;
}

} // namespace

uint32_t live_tick_record_capacity() noexcept {
  const size_t by_bytes =
    (network::canonical_tick_max_bytes - network::canonical_tick_header_bytes -
     network::canonical_tick_root_bytes) /
    (network::canonical_tick_record_header_bytes + canonical_food_action_bytes);
  return uint32_t(std::min<size_t>(network::canonical_tick_max_records, by_bytes));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Авторитет.
// ─────────────────────────────────────────────────────────────────────────────────────────────

struct live_authority::state {
  live_run_options opts;
  authority endpoint;
  host simulation;
  source checkpoint_source;
  network::bounded_history<uint64_t, std::vector<std::byte>> history;
  std::map<uint64_t, uint64_t> next_to_send;
  std::set<uint64_t> catchup_sent;
  std::map<uint64_t, std::vector<accepted_input>> pending;
  input_journal journal;
  // Горизонт номеров ввода на участника: окно дубликатов (по младшим 16 битам) и самый
  // новый принятый полный номер, относительно которого расширяются следующие.
  struct input_horizon {
    network::sequence_window<uint16_t, input_window_bits> window;
    uint64_t newest = 0;
  };
  std::map<uint64_t, input_horizon> horizons;
  network::intent_layout_table layouts = player_intent_layouts();
  network::id_index_table empty_registry;
  counters stats;

  state(net_runtime& runtime, const core::causal_content& content, const live_run_options& o,
        const authority_config& config, const world_declaration& world)
    : opts(o), endpoint(runtime, content, world, config), simulation(o.rate, o.workers),
      checkpoint_source(simulation), history(o.history_ticks, o.history_bytes) {
    simulation.actors.init(opts.actors, {0.5f, 0.5f}, {64.0f, 64.0f}, 1, content.config(),
                           opts.prefab_cycle);
    step(simulation, 1);
  }

  uint64_t now() const noexcept {
    return simulation.clocks.simulation_now().value;
  }

  // Предложение отбрасывается четырёх родов. Неразбираемое или с номером далеко впереди —
  // нарушение участника. Повтор уже принятого — штатная избыточность, молча. Чужой или занятый
  // тик — обычная гонка задержек, без нарушения. Сверх квоты участника на тик — нарушение:
  // общий предел тика иначе позволил бы одному занять все места.
  void receive_inputs() {
    const uint32_t tick_capacity = live_tick_record_capacity();
    for (const auto& packet : endpoint.take_live_intents()) {
      network::intent_batch_view batch;
      std::vector<network::intent> decoded;
      decoded.reserve(network::intent_batch_max_bytes / spawn_food_layout.wire_bytes());
      if (network::try_decode_intent_batch(packet.bytes, layouts, empty_registry, batch,
                                           decoded) != network::intent_wire_status::ok ||
          decoded.empty()) {
        ++stats.dropped_inputs;
        endpoint.note_violation(packet.logical_peer);
        continue;
      }
      auto& horizon = horizons[packet.logical_peer];
      for (const auto& proposal : decoded) admit(packet.logical_peer, horizon, batch, proposal,
                                                 tick_capacity);
    }
  }

  void admit(const uint64_t peer, input_horizon& horizon, const network::intent_batch_view& batch,
             const network::intent& proposal, const uint32_t tick_capacity) {
    switch (horizon.window.classify(proposal.sequence_low)) {
      case network::sequence_classification::new_value: break;
      case network::sequence_classification::duplicate:
        ++stats.duplicate_inputs;
        return;
      case network::sequence_classification::stale:
        ++stats.dropped_inputs;
        return;
      case network::sequence_classification::too_far_ahead:
        ++stats.dropped_inputs;
        endpoint.note_violation(peer);
        return;
    }
    devils_engine::act::intent action;
    if (!decode_player_intent(proposal, action)) {
      ++stats.dropped_inputs;
      endpoint.note_violation(peer);
      return;
    }
    const auto target = network::intent_absolute_tick(now(), batch.base_tick_low, proposal.tick_delta);
    if (!target || *target <= now() || *target > now() + opts.intent_window_ticks ||
        *target > opts.final_tick) {
      ++stats.dropped_inputs;
      return;
    }
    auto& queued = pending[*target];
    const auto from_peer = std::count_if(queued.begin(), queued.end(), [&](const auto& input) {
      return input.peer == peer;
    });
    if (uint32_t(from_peer) >= opts.records_per_peer_per_tick) {
      ++stats.dropped_inputs;
      endpoint.note_violation(peer);
      return;
    }
    if (queued.size() >= tick_capacity) {
      ++stats.dropped_inputs;
      return;
    }
    // Принято: только теперь номер считается использованным.
    (void)horizon.window.observe(proposal.sequence_low);
    const uint64_t sequence = network::widen_input_sequence_low16(horizon.newest, proposal.sequence_low);
    horizon.newest = std::max(horizon.newest, sequence);
    queued.push_back({*target, peer, sequence, action, action_payload(action)});
  }

  void deliver() {
    for (const auto& join : endpoint.joins()) {
      if (!join.agreed || !endpoint.is_live_member(join.peer_id)) continue;
      auto [entry, fresh] = next_to_send.try_emplace(join.peer_id, join.report.tick + 1);
      (void)fresh;
      const auto oldest = history.oldest_tick();
      const auto newest = history.newest_tick();
      // Отставший за начало истории не догонит никогда. Отключить ЕГО с причиной, а не уронить
      // сессию всех остальных: повторный вход со свежим состоянием — штатный путь.
      if (oldest && entry->second < *oldest) {
        endpoint.terminate(join.peer_id, termination_reason::history_exhausted);
        continue;
      }
      size_t sent = 0;
      while (newest && entry->second <= *newest && sent < bundles_per_service) {
        const auto* bundle = history.find(entry->second);
        require(bundle != nullptr, "live history lost a retained tick");
        if (endpoint.send_canonical(join.peer_id, *bundle) != send_result::sent) break;
        stats.canonical_bytes_sent += bundle->size();
        ++entry->second;
        ++sent;
      }
      if (newest && entry->second > *newest && !catchup_sent.contains(join.peer_id) &&
          endpoint.send_catchup(join.peer_id, *newest) == send_result::sent)
        catchup_sent.insert(join.peer_id);
    }
  }
};

namespace {

world_declaration declare_live_world(const core::causal_content& content,
                                     const live_run_options& opts) {
  auto terrain = content.make_terrain(opts.generator, 16, opts.world_seed);
  return declare_world(*terrain, opts.generator);
}

} // namespace

live_authority::live_authority(net_runtime& runtime, const core::causal_content& content,
                               const live_run_options& options, const authority_config& config)
  : state_(std::make_unique<state>(runtime, content, options, config,
                                   declare_live_world(content, options))) {}

live_authority::~live_authority() = default;

void live_authority::service() {
  auto& self = *state_;
  self.endpoint.step(self.checkpoint_source);
  self.receive_inputs();
  self.deliver();
}

void live_authority::advance() {
  auto& self = *state_;
  const uint64_t tick = self.now() + 1;
  const auto tag = self.journal.begin(tick, network::canonical_tick_max_records);
  if (auto it = self.pending.find(tick); it != self.pending.end()) {
    // Квоты при приёме гарантируют, что записи помещаются: отказ здесь — нарушение инварианта.
    for (auto& input : it->second)
      require(self.journal.try_record(std::move(input)) == network::tick_record_result::recorded,
              "live authority input exceeded the tick journal budget");
    self.pending.erase(it);
  }
  require(self.journal.seal() == network::tick_seal_result::sealed,
          "live authority could not seal the tick journal");
  auto accepted = self.journal.consume(tag);
  for (const auto& input : accepted.records())
    require(self.simulation.actors.enqueue_canonical_intent(input.action),
            "live authority could not enqueue accepted input");
  self.stats.applied_inputs += uint32_t(accepted.records().size());
  step(self.simulation, tick);

  std::vector<network::canonical_tick_record_view> records;
  records.reserve(accepted.records().size());
  for (const auto& input : accepted.records())
    records.push_back({input.peer, input.sequence, input.payload});
  std::vector<std::byte> bytes;
  bytes.reserve(network::canonical_tick_max_bytes);
  require(network::try_encode_canonical_tick(tick, root_of(self.simulation), records, bytes) ==
            network::canonical_tick_wire_status::ok,
          "live authority could not encode canonical tick");
  const size_t size = bytes.size();
  require(self.history.try_store(tick, std::move(bytes), size).stored(),
          "live authority could not retain its canonical tick");
  auto recycled = std::move(accepted).release_storage();
  self.journal.retire(tag);
  self.journal.recycle(std::move(recycled));
  self.deliver();
}

uint16_t live_authority::port() const noexcept {
  return state_->endpoint.port();
}

uint64_t live_authority::tick() const noexcept {
  return state_->now();
}

uint64_t live_authority::root() const {
  return root_of(state_->simulation);
}

const live_authority::counters& live_authority::stats() const noexcept {
  return state_->stats;
}

bool live_authority::delivered_through(const uint64_t tick) const {
  for (const auto& join : state_->endpoint.joins()) {
    if (!join.agreed || !state_->endpoint.is_live_member(join.peer_id)) continue;
    const auto it = state_->next_to_send.find(join.peer_id);
    if (it == state_->next_to_send.end() || it->second <= tick) return false;
  }
  return true;
}

authority& live_authority::endpoint() noexcept {
  return state_->endpoint;
}

const authority& live_authority::endpoint() const noexcept {
  return state_->endpoint;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Ведомый.
// ─────────────────────────────────────────────────────────────────────────────────────────────

struct live_follower::state {
  live_run_options opts;
  const core::causal_content& content;
  follower endpoint;
  host simulation;
  sink checkpoint_sink;
  network::intent_layout_table layouts = player_intent_layouts();
  network::id_index_table empty_registry;
  bool prepared = false;
  bool awaiting_repair = false;
  // Неподтверждённые предложения. Подтверждение — своя запись в каноническом bundle; тик цели
  // прошёл без неё — предложение отвергнуто.
  struct outstanding_input {
    uint64_t sequence = 0;
    uint64_t target = 0;
    network::intent proposal;
    uint32_t sends = 0;
  };
  std::vector<outstanding_input> outstanding;
  uint64_t next_sequence = 1;
  clock::time_point last_send = clock::time_point::min();
  std::vector<network::intent> outgoing_intents;
  std::vector<std::byte> outgoing;
  uint64_t checkpoint_root = 0;
  uint64_t predicted_root = 0;
  std::vector<wire_tick> received;
  counters stats;

  state(net_runtime& runtime, const core::causal_content& c, const follower_config& config,
        const live_run_options& o)
    : opts(o), content(c), endpoint(runtime, c, config), simulation(o.rate, o.workers),
      checkpoint_sink(simulation, c.config()) {
    outgoing_intents.reserve(max_outstanding_inputs);
    outgoing.reserve(network::intent_batch_max_bytes);
  }

  uint64_t now() const noexcept {
    return simulation.clocks.simulation_now().value;
  }

  void prepare() {
    prepared = true;
    stats.checkpoint_tick = endpoint.report().tick;
    checkpoint_root = endpoint.report().root;
    if (!opts.predict_wrong) return;
    // Один нарочно неверный локальный прогноз делает исправление наблюдаемым путём.
    require(simulation.actors.enqueue_player_intent(food_action(22.0, 22.0)),
            "live follower could not enqueue its prediction");
    step(simulation, stats.checkpoint_tick + 1);
    predicted_root = root_of(simulation);
    awaiting_repair = true;
  }

  void repair(std::vector<std::byte>&& bytes, const network::canonical_tick_view& view) {
    require(view.tick == stats.checkpoint_tick + 1 && predicted_root != *view.state_root,
            "live follower prediction did not diverge as expected");
    received.push_back({view.tick, std::move(bytes)});
    const uint64_t local_peer = endpoint.peer_id();
    const auto restore = [&](host& value, const std::vector<std::byte>& checkpoint) {
      value.expected_root = checkpoint_root;
      return core::load_actor_checkpoint(value.actors, value.clocks, checkpoint, content.config())
        .loaded();
    };
    const auto apply = [&](host& value, const std::vector<std::byte>& bundle,
                           const network::replay_context context) {
      return context.presentation_suppressed() &&
             apply_bundle(value, bundle, local_peer, [this](const uint64_t s) { confirm(s); });
    };
    const auto advance = [](host& value, const uint64_t tick,
                            const network::replay_context context) {
      if (!context.presentation_suppressed() || value.pending_tick != tick) return false;
      step(value, tick);
      return true;
    };
    const auto verify = [](const host& value, const uint64_t tick) {
      return value.clocks.simulation_now().value == tick && root_of(value) == value.expected_root;
    };
    const auto result = network::replay_to(
      simulation, stats.checkpoint_tick, checkpoint_sink.checkpoint, view.tick, received,
      restore, apply, advance, verify, network::checked_tick_successor<uint64_t>{});
    if (!result.completed())
      std::fprintf(stderr, "live replay refused at tick %llu, status %u\n",
                   static_cast<unsigned long long>(result.tick.value_or(view.tick)),
                   unsigned(result.status));
    require(result.completed(), "live follower could not repair its prediction");
    stats.corrected = true;
    awaiting_repair = false;
    received.clear();
  }

  void confirm(const uint64_t sequence) {
    const auto it = std::find_if(outstanding.begin(), outstanding.end(),
                                 [&](const auto& value) { return value.sequence == sequence; });
    if (it == outstanding.end()) return; // уже подтверждено: повтор одного решения не считается
    outstanding.erase(it);
    ++stats.own_inputs;
  }

  // Все неподтверждённые едут ОДНИМ пакетом: цель каждого — `tick_delta` от базового тика
  // пакета (до 7 назад). Отдельный пакет на предложение упёрся бы в квоту пакетов участника,
  // и честный клиент копил бы нарушения. Предложения с целями шире окна — следующим пакетом.
  // Просроченные (тик цели прошёл без записи) считаются отвергнутыми.
  void send_outstanding(const bool force) {
    for (auto it = outstanding.begin(); it != outstanding.end();) {
      if (now() >= it->target) {
        ++stats.rejected_inputs;
        it = outstanding.erase(it);
      } else {
        ++it;
      }
    }
    const auto period = std::chrono::nanoseconds(std::chrono::seconds(1)) / opts.rate;
    if (outstanding.empty() || (!force && clock::now() - last_send < period)) return;
    std::sort(outstanding.begin(), outstanding.end(),
              [](const auto& a, const auto& b) { return a.target > b.target; });
    size_t first = 0;
    while (first < outstanding.size()) {
      const uint64_t base = outstanding[first].target;
      outgoing_intents.clear();
      size_t last = first;
      for (; last < outstanding.size() &&
             base - outstanding[last].target <= network::intent_tick_delta_limit;
           ++last) {
        if (outstanding[last].sends >= input_resend_limit) continue;
        auto proposal = outstanding[last].proposal;
        proposal.tick_delta = uint8_t(base - outstanding[last].target);
        outgoing_intents.push_back(proposal);
      }
      if (!outgoing_intents.empty() &&
          network::try_encode_intent_batch(base, outgoing_intents, layouts, empty_registry,
                                           outgoing) == network::intent_wire_status::ok &&
          endpoint.send_intent(outgoing) == send_result::sent) {
        for (size_t i = first; i < last; ++i)
          if (outstanding[i].sends < input_resend_limit) ++outstanding[i].sends;
      }
      first = last;
    }
    last_send = clock::now();
  }

  void follow(const std::span<const std::byte> bytes) {
    require(apply_bundle(simulation, bytes, endpoint.peer_id(),
                         [this](const uint64_t sequence) { confirm(sequence); }),
            "live follower rejected the next canonical bundle");
    step(simulation, simulation.pending_tick);
    const uint64_t actual_root = root_of(simulation);
    if (actual_root != simulation.expected_root)
      std::fprintf(stderr, "live root mismatch at tick %llu: expected %llu, got %llu\n",
                   static_cast<unsigned long long>(simulation.pending_tick),
                   static_cast<unsigned long long>(simulation.expected_root),
                   static_cast<unsigned long long>(actual_root));
    require(actual_root == simulation.expected_root, "live follower root differs from authority");
  }
};

live_follower::live_follower(net_runtime& runtime, const core::causal_content& content,
                             const follower_config& config, const live_run_options& options)
  : state_(std::make_unique<state>(runtime, content, config, options)) {}

live_follower::~live_follower() = default;

bool live_follower::start() {
  return state_->endpoint.start();
}

void live_follower::service() {
  auto& self = *state_;
  self.endpoint.step(self.checkpoint_sink);
  if (!self.prepared) {
    if (!self.endpoint.live_ready() || self.endpoint.report().status != join_status::loaded ||
        self.checkpoint_sink.checkpoint.empty())
      return;
    self.prepare();
  }
  for (auto& bytes : self.endpoint.take_canonical_ticks()) {
    self.stats.canonical_bytes += bytes.size();
    std::array<network::canonical_tick_record_view, network::canonical_tick_max_records> storage{};
    network::canonical_tick_view view;
    require(network::try_decode_canonical_tick(bytes, self.now(), storage, view) ==
                network::canonical_tick_wire_status::ok &&
              view.state_root,
            "live follower received a malformed canonical tick");
    if (self.awaiting_repair) self.repair(std::move(bytes), view);
    else self.follow(bytes);
    ++self.stats.bundles_verified;
  }
  self.send_outstanding(false);
}

bool live_follower::propose_food(const double x, const double y, const uint64_t target) {
  auto& self = *state_;
  if (!self.prepared || self.outstanding.size() >= max_outstanding_inputs || target <= self.now())
    return false;
  network::intent proposal;
  if (!encode_player_intent(food_action(x, y), uint16_t(self.next_sequence), 0, proposal))
    return false;
  self.outstanding.push_back({self.next_sequence, target, proposal, 0});
  ++self.next_sequence;
  self.send_outstanding(true);
  return true;
}

bool live_follower::ready() const noexcept {
  return state_->prepared;
}

bool live_follower::caught_up() const noexcept {
  return state_->prepared && state_->endpoint.catchup_tick() != 0 &&
         state_->now() >= state_->endpoint.catchup_tick();
}

uint64_t live_follower::tick() const noexcept {
  return state_->now();
}

uint64_t live_follower::root() const {
  return root_of(state_->simulation);
}

const live_follower::counters& live_follower::stats() const noexcept {
  return state_->stats;
}

follower& live_follower::endpoint() noexcept {
  return state_->endpoint;
}

const follower& live_follower::endpoint() const noexcept {
  return state_->endpoint;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Процессные драйверы.
// ─────────────────────────────────────────────────────────────────────────────────────────────

int run_live_authority(const live_run_options& opts, const core::causal_content& content) {
  try {
    spdlog::set_level(spdlog::level::err);
    require(opts.port != 0 && opts.expected_peers == 2 && opts.final_tick >= 64,
            "live authority needs a port, two followers and at least 64 ticks");
    net_runtime runtime(4);
    authority_config config;
    config.host = opts.bind_loopback ? 0x7f000001u : 0u;
    config.port = opts.port;
    config.max_peers = opts.expected_peers;
    config.verbose = false;
    config.keep_joined_connection = true;
    live_authority session(runtime, content, opts, config);
    auto& endpoint = session.endpoint();

    const auto start_deadline = clock::now() + std::chrono::seconds(opts.timeout_seconds);
    std::printf("live.listen.port = %u\n", session.port());
    std::printf("live.listen.content_root = %s\n",
                core::short_digest(content.manifest().root).c_str());
    std::fflush(stdout);

    // Start the clock with the first participant. The second joins a moving world
    // and must consume K+1..N from the bounded history.
    while (endpoint.completed_joins() == 0 && clock::now() < start_deadline) {
      runtime.pump();
      session.service();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(endpoint.completed_joins() != 0, "live authority timed out waiting for first join");

    const auto period = std::chrono::nanoseconds(std::chrono::seconds(1)) / opts.rate;
    auto next_tick_time = clock::now() + period;
    while (session.tick() < opts.final_tick) {
      runtime.pump();
      session.service();
      session.advance();
      std::this_thread::sleep_until(next_tick_time);
      next_tick_time += period;
    }

    const auto drain_deadline = clock::now() + std::chrono::seconds(opts.timeout_seconds);
    while (clock::now() < drain_deadline) {
      runtime.pump();
      session.service();
      if (endpoint.completed_joins() == opts.expected_peers &&
          session.delivered_through(opts.final_tick) && endpoint.connected_peers() == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto& stats = session.stats();
    std::printf("live.authority.tick = %llu\n", static_cast<unsigned long long>(session.tick()));
    std::printf("live.authority.root = %llu\n", static_cast<unsigned long long>(session.root()));
    std::printf("live.authority.joins = %u\n", endpoint.completed_joins());
    std::printf("live.authority.inputs = %u\n", stats.applied_inputs);
    std::printf("live.authority.dropped_inputs = %llu\n",
                static_cast<unsigned long long>(stats.dropped_inputs));
    std::printf("live.authority.terminated = %zu\n", endpoint.terminations().size());
    std::printf("live.authority.violations = %llu\n",
                static_cast<unsigned long long>(endpoint.violations()));
    std::printf("live.authority.checkpoint_bytes_sent = %llu\n",
                static_cast<unsigned long long>(endpoint.checkpoint_bytes_sent()));
    std::printf("live.authority.canonical_bytes_sent = %llu\n",
                static_cast<unsigned long long>(stats.canonical_bytes_sent));
    for (const auto& value : endpoint.terminations())
      std::printf("live.authority.termination = peer %llu: %s\n",
                  static_cast<unsigned long long>(value.peer_id), describe(value.reason));
    endpoint.close_all();
    require(endpoint.completed_joins() == opts.expected_peers && endpoint.terminations().empty() &&
              stats.applied_inputs == opts.expected_peers && stats.dropped_inputs == 0,
            "live authority did not complete both followers and their inputs");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "frontier live authority: %s\n", error.what());
    return 1;
  }
}

int run_live_follower(const live_run_options& opts, const core::causal_content& content) {
  try {
    spdlog::set_level(spdlog::level::err);
    uint32_t address = 0;
    uint16_t port = 0;
    require(parse_endpoint(opts.endpoint, address, port), "live follower needs HOST:PORT");
    net_runtime runtime(2);
    follower_config config;
    config.host = address;
    config.port = port;
    config.verbose = false;
    live_follower session(runtime, content, config, opts);
    require(session.start(), "live follower could not start connection");
    const auto& endpoint = session.endpoint();
    const auto deadline = clock::now() + std::chrono::seconds(opts.timeout_seconds);
    const auto check_alive = [&](const char* reason) {
      if (endpoint.fault() != follower_fault::none)
        std::fprintf(stderr, "live follower fault: %s\n", describe(endpoint.fault()));
      require(endpoint.fault() == follower_fault::none && !endpoint.live_terminal() &&
                endpoint.phase() != follower_phase::failed,
              reason);
    };
    while (!session.ready() && clock::now() < deadline) {
      runtime.pump();
      session.service();
      check_alive("live follower failed before join confirmation");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(session.ready(), "live follower did not receive an acknowledged checkpoint");
    const uint64_t checkpoint_tick = session.stats().checkpoint_tick;
    std::printf("live.follower.joined_tick = %llu\n",
                static_cast<unsigned long long>(checkpoint_tick));
    std::fflush(stdout);
    require(checkpoint_tick < opts.final_tick,
            "live follower joined too late to observe a canonical tick");

    bool sent_input = false;
    while (session.tick() < opts.final_tick && clock::now() < deadline) {
      runtime.pump();
      session.service();
      check_alive("live follower lost the session before the final tick");
      if (!sent_input && session.caught_up() && session.tick() + 24 <= opts.final_tick) {
        require(session.propose_food(12.0 + double(endpoint.peer_id()), 13.0, session.tick() + 24),
                "live follower could not send its intent");
        sent_input = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto& stats = session.stats();
    std::printf("live.follower.peer = %llu\n",
                static_cast<unsigned long long>(endpoint.peer_id()));
    std::printf("live.follower.checkpoint_tick = %llu\n",
                static_cast<unsigned long long>(checkpoint_tick));
    std::printf("live.follower.tick = %llu\n", static_cast<unsigned long long>(session.tick()));
    std::printf("live.follower.root = %llu\n", static_cast<unsigned long long>(session.root()));
    std::printf("live.follower.corrected = %u\n", unsigned(stats.corrected));
    std::printf("live.follower.own_inputs = %u\n", stats.own_inputs);
    std::printf("live.follower.checkpoint_bytes = %u\n", endpoint.received_bytes());
    std::printf("live.follower.bundles_verified = %llu\n",
                static_cast<unsigned long long>(stats.bundles_verified));
    std::printf("live.follower.canonical_bytes = %llu\n",
                static_cast<unsigned long long>(stats.canonical_bytes));
    require(session.tick() == opts.final_tick && stats.corrected && sent_input &&
              stats.own_inputs == 1,
            "live follower did not finish the tick/root/input run");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "frontier live follower: %s\n", error.what());
    return 1;
  }
}

} // namespace frontier_online::net
