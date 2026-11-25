#include "net/live_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <devils_engine/network/bounded_history.h>
#include <devils_engine/network/canonical_tick_wire.h>
#include <devils_engine/network/checkpoint_ring.h>
#include <devils_engine/network/reconnect.h>
#include <devils_engine/network/replay.h>
#include <devils_engine/network/sequence_window.h>
#include <devils_engine/network/state_digest.h>
#include <devils_engine/network/tick_journal.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/core.h>
#include <gtl/phmap.hpp>
#include <devils_engine/utils/simulation_time.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/timeline.h>
#include <spdlog/spdlog.h>

#include "core/actor_checkpoint.h"
#include "core/actor_simulation.h"
#include "core/causal_content.h"
#include "core/world_rules.h"
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

// Политика ошибок этого файла. Нарушение СОБСТВЕННОГО инварианта — `utils::error` прямо у причины.
// Всё, что приходит от другого узла, проверяется раньше и становится исходом (`live_failure`,
// отказ рукопожатия, вердикт прогона), а не исключением.

struct host {
  core::actor_world_slice actors;
  utils::timelines clocks;
  // Пул владельца процесса (графический клиент) или собственный (headless): второй независимый
  // пул рядом с пулом клиента был бы скрытым расходом потоков.
  std::optional<devils_engine::thread::atomic_pool> own_pool;
  devils_engine::thread::atomic_pool* pool = nullptr;
  uint64_t expected_root = 0;
  uint64_t pending_tick = 0;

  host(const uint32_t rate, const uint32_t workers, devils_engine::thread::atomic_pool* shared)
    : clocks(utils::simulation_rate(rate)) {
    clocks.set_game_scale(core::world_time_scale()); // правило мира; ведомый получит его checkpoint'ом
    if (shared == nullptr) shared = &own_pool.emplace(workers);
    pool = shared;
  }
};

void step(host& value, const uint64_t number) {
  const auto tick = value.clocks.simulation_now() + utils::simulation_duration{1};
  if (tick.value != number)
    utils::error{}("frontier_online live: simulation step {} requested, next tick is {}", number,
                   tick.value);
  value.actors.update(tick, value.clocks.advance_simulation(tick), nullptr, *value.pool);
}

uint64_t root_of(const host& value) {
  core::actor_checkpoint_buffers buffers;
  network::state_digest_report<uint64_t> digest;
  digest.sections.reserve(core::actor_checkpoint_section_count);
  if (!core::actor_checkpoint_digest(value.actors, value.clocks, buffers, digest))
    utils::error{}("frontier_online live: causal state at tick {} does not digest",
                   value.clocks.simulation_now().value);
  return digest.root;
}

struct ring_checkpoint {
  checkpoint_begin header;
  std::vector<std::byte> document;
};

struct ring_checkpoint_size {
  size_t operator()(const ring_checkpoint& value) const noexcept {
    return value.document.size();
  }
};

using canonical_history = network::bounded_history<uint64_t, std::vector<std::byte>>;
using checkpoint_ring_t = network::checkpoint_ring<uint64_t, ring_checkpoint, ring_checkpoint_size>;

class source final : public checkpoint_source {
public:
  source(const host& value, const canonical_history& history, const checkpoint_ring_t& ring,
         live_authority::counters& stats)
    : value_(value), history_(history), ring_(ring), stats_(stats) {}

  bool resume(uint64_t logical_peer, const std::optional<resume_anchor>& anchor, resume_mode& mode,
              checkpoint_begin& header, std::vector<std::byte>& document) override;

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
  const canonical_history& history_;
  const checkpoint_ring_t& ring_;
  live_authority::counters& stats_;
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
  if (!encode(canonical_food_action{action.payload.spawn.target.x, action.payload.spawn.target.y},
              bytes))
    utils::error{}("frontier_online live: spawn_food payload ({}, {}) does not encode",
                   action.payload.spawn.target.x, action.payload.spawn.target.y);
  return bytes;
}

// Одно исполнение канонической записи для всех: авторитет исполняет те же байты, что ведомые.
bool enqueue_payload(host& target, const uint64_t peer, const uint64_t sequence,
                     const std::span<const std::byte> bytes) {
  switch (classify(bytes, sequence)) {
    case canonical_action_kind::peer_joined: return target.actors.enqueue_canonical_join(peer);
    case canonical_action_kind::peer_left: return target.actors.enqueue_canonical_leave(peer);
    case canonical_action_kind::spawn_food: {
      canonical_food_action action;
      return decode(bytes, action) &&
             target.actors.enqueue_canonical_intent(food_action(action.x, action.y), peer);
    }
    case canonical_action_kind::invalid: break;
  }
  return false;
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

// Разбор bundle на границе с сетью: неразбираемый — исход «авторитет нарушил протокол», не исключение.
bool decode_bundle(const std::span<const std::byte> bytes, const uint64_t now,
                   std::array<network::canonical_tick_record_view,
                              network::canonical_tick_max_records>& storage,
                   network::canonical_tick_view& view) {
  return network::try_decode_canonical_tick(bytes, now, storage, view) ==
           network::canonical_tick_wire_status::ok &&
         view.state_root.has_value();
}

// Догон вернувшегося. Якорь клиента годится, если его корень совпал с корнем того же тика у
// авторитета (тик в истории или текущий): тогда хватает тиков T+1..N. Иначе — снимок K из кольца и
// тики K+1..N, если история их ещё держит. Ни то ни другое — догонять не из чего.
bool source::resume(const uint64_t logical_peer, const std::optional<resume_anchor>& anchor,
                    resume_mode& mode, checkpoint_begin& header, std::vector<std::byte>& document) {
  static_cast<void>(logical_peer);
  const uint64_t now = value_.clocks.simulation_now().value;
  if (anchor && anchor->tick != 0 && anchor->tick <= now) {
    std::optional<uint64_t> root;
    if (anchor->tick == now) {
      root = root_of(value_);
    } else if (const auto* bundle = history_.find(anchor->tick)) {
      std::array<network::canonical_tick_record_view, network::canonical_tick_max_records> storage{};
      network::canonical_tick_view view;
      if (!decode_bundle(*bundle, anchor->tick - 1, storage, view) || view.tick != anchor->tick)
        utils::error{}("frontier_online live authority: own history bundle of tick {} does not decode",
                       anchor->tick);
      root = view.state_root;
    }
    if (root && *root == anchor->root) {
      mode = resume_mode::journal;
      header = {anchor->tick, anchor->root, 0, core::actor_checkpoint_schema_fingerprint()};
      document.clear();
      ++stats_.journal_resumes;
      return true;
    }
  }
  const auto* entry = ring_.latest_at_or_before(now);
  network::recovery_range<uint64_t> range;
  if (entry == nullptr ||
      network::assess_recovery<uint64_t>(entry->tick, history_.oldest_tick(), now, range) !=
        network::recovery_feasibility::recoverable)
    return false;
  mode = resume_mode::checkpoint;
  header = entry->bundle.header;
  document = entry->bundle.document;
  ++stats_.checkpoint_resumes;
  return true;
}

// `on_own(sequence)` — запись этого участника: так ведомый узнаёт, какие его предложения
// авторитет принял.
template <class OnOwn>
bool apply_bundle(host& target, const network::canonical_tick_view& view,
                  const uint64_t local_peer, OnOwn&& on_own) {
  if (!view.state_root || view.tick != target.clocks.simulation_now().value + 1) return false;
  target.pending_tick = view.tick;
  target.expected_root = *view.state_root;
  for (const auto& record : view.records) {
    if (!enqueue_payload(target, record.origin_peer, record.input_sequence, record.payload))
      return false;
    if (record.origin_peer == local_peer) on_own(record.input_sequence);
  }
  return true;
}

} // namespace

uint32_t live_tick_record_capacity() noexcept {
  return uint32_t(network::canonical_tick_record_capacity(true, canonical_food_action_bytes));
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Авторитет.
// ─────────────────────────────────────────────────────────────────────────────────────────────

struct live_authority::state {
  live_run_options opts;
  authority endpoint;
  host simulation;
  source checkpoint_source;
  canonical_history history;
  checkpoint_ring_t ring;
  // Ключ — logical peer (или тик для `pending`). Порядок обхода нигде не влияет на канонические
  // байты: где он важен (список ушедших), его задаёт явная сортировка.
  gtl::flat_hash_map<uint64_t, uint64_t> next_to_send;
  gtl::flat_hash_set<uint64_t> catchup_sent;
  gtl::flat_hash_map<uint64_t, std::vector<accepted_input>> pending;
  input_journal journal;
  // Горизонт номеров ввода на участника: окно дубликатов (по младшим 16 битам) и самый
  // новый принятый полный номер, относительно которого расширяются следующие.
  struct input_horizon {
    network::sequence_window<uint16_t, input_window_bits> window;
    uint64_t newest = 0;
  };
  gtl::flat_hash_map<uint64_t, input_horizon> horizons;
  // Тик канонической записи присоединения каждого принятого участника. Его предложения
  // принимаются только на тики ПОСЛЕ него: раньше в мире нет его игрока.
  gtl::flat_hash_map<uint64_t, uint64_t> admitted;
  network::intent_layout_table layouts = player_intent_layouts();
  network::id_index_table empty_registry;
  counters stats;

  state(net_runtime& runtime, const core::causal_content& content, const live_run_options& o,
        const authority_config& config, const world_declaration& world)
    : opts(o), endpoint(runtime, content, world, config), simulation(o.rate, o.workers, nullptr),
      checkpoint_source(simulation, history, ring, stats), history(o.history_ticks, o.history_bytes),
      ring(o.checkpoint_ring_count, o.checkpoint_ring_bytes) {
    if (opts.checkpoint_every_ticks == 0)
      opts.checkpoint_every_ticks = uint32_t(std::max<size_t>(1, o.history_ticks / 4));
    if (opts.checkpoint_every_ticks >= o.history_ticks)
      utils::error{}("frontier_online live authority: checkpoint every {} ticks must be below the "
                     "history of {} ticks: an older snapshot cannot be caught up from",
                     opts.checkpoint_every_ticks, o.history_ticks);
    trace_live_phase(opts.verbose, "actors");
    simulation.actors.init(opts.actors, {0.5f, 0.5f}, {opts.extent, opts.extent}, 1,
                           content.config(), opts.prefab_cycle);
    trace_live_phase(opts.verbose, "first_step");
    step(simulation, 1);
    trace_live_phase(opts.verbose, "authority_ready");
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
      // Горизонт ввода заводится только игроку в мире: у не допущенного предложения всё равно
      // отбрасываются, а запись пережила бы его уход до записи присоединения.
      if (!admitted.contains(packet.logical_peer)) {
        stats.dropped_inputs += decoded.size();
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
    const auto joined = admitted.find(peer);
    if (!target || *target <= now() || *target > now() + opts.intent_window_ticks ||
        *target > opts.final_tick || joined == admitted.end() || *target <= joined->second) {
      ++stats.dropped_inputs;
      return;
    }
    auto& queued = pending[*target];
    const auto from_peer = std::count_if(queued.begin(), queued.end(), [&](const auto& input) {
      return input.peer == peer;
    });
    if (uint32_t(from_peer) >= opts.records_per_peer_per_tick) {
      // Отбросить без нарушения: номер не считается использованным, клиент повторит и сам сдвинет
      // лишние клики на следующие тики (net_client::propose_food).
      ++stats.dropped_inputs;
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

  // Состав игроков меняется каноническими записями этого тика. Предложения в тик уже приняты в
  // пределах его ёмкости; запись, которой не хватило места (записей или байт), ждёт следующего
  // тика — её время ни к чему не привязано.
  //
  // Уход: принятый участник, чей игрок уже исполнен (запись присоединения в прошлом тике), больше
  // не обслуживается и его слот не удерживается — отключён за нарушения, не согласился или окно
  // повторного входа истекло. Его ещё не записанные предложения снимаются: в тике ухода и после
  // него действовать некому. Удерживаемый участник уходом не считается: его игрок стоит в мире, а
  // вернувшись, он продолжает тем же logical peer.
  //
  // Присоединение: участник, подтвердивший корень checkpoint, получает игрока. Его предложения до
  // записи всё равно не принимаются.
  void record_membership(const uint64_t tick) {
    std::vector<uint64_t> departed;
    std::vector<uint64_t> idle;
    for (const auto& [peer, joined_at] : admitted) {
      if (joined_at >= tick || endpoint.is_live_member(peer)) continue;
      if (endpoint.is_held(peer)) idle.push_back(peer);
      else departed.push_back(peer);
    }
    // Удерживаемый игрок стоит без вводов: его принятые предложения снимаются (вернувшись, он их не
    // повторит — номера уже использованы, и клиент сочтёт их отвергнутыми по тику цели).
    for (const uint64_t peer : idle) {
      for (auto& [target, queued] : pending) {
        const auto before = queued.size();
        std::erase_if(queued, [peer](const accepted_input& input) { return input.peer == peer; });
        stats.dropped_inputs += before - queued.size();
      }
    }
    // Записи ухода идут в журнал в порядке peer: хеш-таблица порядка не обещает.
    std::sort(departed.begin(), departed.end());
    for (const uint64_t peer : departed) {
      for (auto& [target, queued] : pending) {
        const auto before = queued.size();
        std::erase_if(queued, [peer](const accepted_input& input) { return input.peer == peer; });
        stats.dropped_inputs += before - queued.size();
      }
    }
    gtl::erase_if(pending, [](const auto& entry) { return entry.second.empty(); });

    const auto queued = pending.find(tick);
    const size_t inputs = queued == pending.end() ? 0 : queued->second.size();
    network::canonical_tick_budget budget(true);
    for (size_t i = 0; i < inputs; ++i)
      if (!budget.take(canonical_food_action_bytes))
        utils::error{}("frontier_online live authority: {} accepted inputs of tick {} exceed the "
                       "canonical tick (admission quota broken)", inputs, tick);

    for (const uint64_t peer : departed) {
      if (!budget.take(canonical_leave_action_bytes)) return;
      // Место взято у `budget`: отказ журнала здесь — нарушение собственного учёта.
      if (journal.try_record({tick, peer, canonical_leave_sequence, {}, encode_leave()}) !=
          network::tick_record_result::recorded)
        utils::error{}("frontier_online live authority: leave of peer {} at tick {} not recorded",
                       peer, tick);
      admitted.erase(peer);
      horizons.erase(peer);
      next_to_send.erase(peer);
      catchup_sent.erase(peer);
    }
    for (const auto& join : endpoint.joins()) {
      if (!join.agreed || !endpoint.is_live_member(join.peer_id) ||
          admitted.contains(join.peer_id)) continue;
      if (!budget.take(canonical_join_action_bytes)) return;
      if (journal.try_record({tick, join.peer_id, canonical_join_sequence, {}, encode_join()}) !=
          network::tick_record_result::recorded)
        utils::error{}("frontier_online live authority: join of peer {} at tick {} not recorded",
                       join.peer_id, tick);
      admitted.emplace(join.peer_id, tick);
    }
  }

  void deliver() {
    // У неживого участника маршрута нет: вернувшийся продолжает с тика своего нового отчёта.
    gtl::erase_if(next_to_send, [&](const auto& entry) { return !endpoint.is_live_member(entry.first); });
    gtl::erase_if(catchup_sent, [&](const uint64_t peer) { return !endpoint.is_live_member(peer); });
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
        if (bundle == nullptr)
          utils::error{}("frontier_online live authority: history [{}, {}] lost tick {}",
                         oldest.value_or(0), newest.value_or(0), entry->second);
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
  trace_live_phase(opts.verbose, "terrain");
  auto terrain = content.make_terrain(opts.generator, opts.chunk_size, opts.world_seed);
  trace_live_phase(opts.verbose, "world_declaration");
  auto world = declare_world(*terrain, opts.generator);
  trace_live_phase(opts.verbose, "world_declared");
  return world;
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
  self.record_membership(tick);
  if (auto it = self.pending.find(tick); it != self.pending.end()) {
    // Квоты при приёме гарантируют, что записи помещаются: отказ здесь — нарушение инварианта.
    for (auto& input : it->second)
      if (const uint64_t peer = input.peer;
          self.journal.try_record(std::move(input)) != network::tick_record_result::recorded)
        utils::error{}("frontier_online live authority: accepted input of peer {} exceeds the "
                       "journal of tick {}", peer, tick);
    self.pending.erase(it);
  }
  if (self.journal.seal() != network::tick_seal_result::sealed)
    utils::error{}("frontier_online live authority: journal of tick {} does not seal", tick);
  auto accepted = self.journal.consume(tag);
  for (const auto& input : accepted.records()) {
    if (!enqueue_payload(self.simulation, input.peer, input.sequence, input.payload))
      utils::error{}("frontier_online live authority: own accepted record peer {} sequence {} "
                     "does not apply at tick {}", input.peer, input.sequence, tick);
    if (input.sequence == canonical_join_sequence) ++self.stats.admitted_peers;
    else if (input.sequence == canonical_leave_sequence) ++self.stats.departed_peers;
    else ++self.stats.applied_inputs;
  }
  step(self.simulation, tick);

  std::vector<network::canonical_tick_record_view> records;
  records.reserve(accepted.records().size());
  for (const auto& input : accepted.records())
    records.push_back({input.peer, input.sequence, input.payload});
  std::vector<std::byte> bytes;
  bytes.reserve(network::canonical_tick_max_bytes);
  if (network::try_encode_canonical_tick(tick, root_of(self.simulation), records, bytes) !=
      network::canonical_tick_wire_status::ok)
    utils::error{}("frontier_online live authority: canonical tick {} ({} records) does not encode",
                   tick, records.size());
  const size_t size = bytes.size();
  if (!self.history.try_store(tick, std::move(bytes), size).stored())
    utils::error{}("frontier_online live authority: history refuses tick {} ({} bytes)", tick,
                   size);
  if (tick % self.opts.checkpoint_every_ticks == 0) {
    ring_checkpoint snapshot;
    if (!self.checkpoint_source.capture(snapshot.header, snapshot.document))
      utils::error{}("frontier_online live authority: snapshot of tick {} does not capture", tick);
    const size_t snapshot_bytes = snapshot.document.size();
    if (!self.ring.try_store(tick, std::move(snapshot)).stored())
      utils::error{}("frontier_online live authority: checkpoint ring refuses tick {} ({} bytes, "
                     "budget {} bytes)", tick, snapshot_bytes, self.ring.byte_budget());
  }
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
  net_runtime& runtime;
  follower_config config;
  // Соединение сменное: повторный вход с продолжением заменяет его, а мир, неподтверждённые
  // предложения и номера ввода остаются — тот же участник продолжает с того же места.
  std::unique_ptr<follower> endpoint;
  world_declaration world; // объявление мира первого входа (продолжение мира не присылает)
  std::optional<reconnect_grant> grant;
  bool resuming = false;
  uint32_t resumes = 0;
  host simulation;
  sink checkpoint_sink;
  network::intent_layout_table layouts = player_intent_layouts();
  network::id_index_table empty_registry;
  bool prepared = false;
  bool awaiting_repair = false;
  bool admitted = false; // своя запись присоединения исполнена: в мире есть свой игрок
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
  live_failure failure = live_failure::none; // первый исход; после него ведомый стоит

  state(net_runtime& runtime, const core::causal_content& c, const follower_config& config,
        const live_run_options& o, devils_engine::thread::atomic_pool* pool)
    : opts(o), content(c), runtime(runtime), config(config),
      endpoint(std::make_unique<follower>(runtime, c, config)), simulation(o.rate, o.workers, pool),
      checkpoint_sink(simulation, c.config()) {
    outgoing_intents.reserve(max_outstanding_inputs);
    outgoing.reserve(network::intent_batch_max_bytes);
  }

  uint64_t now() const noexcept {
    return simulation.clocks.simulation_now().value;
  }

  void prepare() {
    prepared = true;
    world = endpoint->world();
    stats.checkpoint_tick = endpoint->report().tick;
    checkpoint_root = endpoint->report().root;
    if (!opts.predict_wrong) return;
    // Один нарочно неверный локальный прогноз делает исправление наблюдаемым путём.
    if (!simulation.actors.enqueue_player_intent(food_action(22.0, 22.0)))
      utils::error{}("frontier_online live follower: deliberate prediction at tick {} not enqueued",
                     stats.checkpoint_tick + 1);
    step(simulation, stats.checkpoint_tick + 1);
    predicted_root = root_of(simulation);
    awaiting_repair = true;
  }

  void repair(std::vector<std::byte>&& bytes, const network::canonical_tick_view& view) {
    // Первый bundle после checkpoint обязан быть следующим тиком; иное — авторитет нарушил порядок.
    if (view.tick != stats.checkpoint_tick + 1) {
      failure = live_failure::malformed_bundle;
      return;
    }
    // Нарочно неверный прогноз совпал с каноническим корнем: чинить нечего.
    if (predicted_root == *view.state_root) {
      awaiting_repair = false;
      return;
    }
    received.push_back({view.tick, std::move(bytes)});
    const uint64_t local_peer = endpoint->peer_id();
    const auto restore = [&](host& value, const std::vector<std::byte>& checkpoint) {
      value.expected_root = checkpoint_root;
      return core::load_actor_checkpoint(value.actors, value.clocks, checkpoint, content.config())
        .loaded();
    };
    const auto apply = [&](host& value, const std::vector<std::byte>& bundle,
                           const network::replay_context context) {
      std::array<network::canonical_tick_record_view, network::canonical_tick_max_records> records{};
      network::canonical_tick_view decoded;
      return context.presentation_suppressed() &&
             decode_bundle(bundle, value.clocks.simulation_now().value, records, decoded) &&
             apply_bundle(value, decoded, local_peer, [this](const uint64_t s) { confirm(s); });
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
    if (!result.completed()) {
      // Переигрывание канонических bundles с checkpoint не сошлось: мир разошёлся с авторитетом.
      std::fprintf(stderr, "live replay refused at tick %llu, status %u\n",
                   static_cast<unsigned long long>(result.tick.value_or(view.tick)),
                   unsigned(result.status));
      failure = live_failure::diverged;
      return;
    }
    stats.corrected = true;
    awaiting_repair = false;
    received.clear();
  }

  void confirm(const uint64_t sequence) {
    if (sequence == canonical_join_sequence) {
      admitted = true;
      return;
    }
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
    // Полный порядок (цель, sequence): при равных целях состав и порядок пакета иначе решала бы
    // реализация std::sort, и Linux- и Windows-клиент слали бы из одного состояния разные байты.
    std::sort(outstanding.begin(), outstanding.end(), [](const auto& a, const auto& b) {
      if (a.target != b.target) return a.target > b.target;
      return a.sequence < b.sequence;
    });
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
          endpoint->send_intent(outgoing) == send_result::sent) {
        for (size_t i = first; i < last; ++i)
          if (outstanding[i].sends < input_resend_limit) ++outstanding[i].sends;
      }
      first = last;
    }
    last_send = clock::now();
  }

  // Запись, которую локальный мир не принимает, и чужой корень после тика — один исход:
  // рассинхрон. Лечится не здесь, а пересинхронизацией (свежий checkpoint).
  void follow(const network::canonical_tick_view& view) {
    if (!apply_bundle(simulation, view, endpoint->peer_id(),
                      [this](const uint64_t sequence) { confirm(sequence); })) {
      std::fprintf(stderr, "live bundle for tick %llu does not apply to the local world\n",
                   static_cast<unsigned long long>(view.tick));
      failure = live_failure::diverged;
      return;
    }
    step(simulation, simulation.pending_tick);
    const uint64_t actual_root = root_of(simulation);
    if (actual_root != simulation.expected_root) {
      std::fprintf(stderr, "live root mismatch at tick %llu: expected %llu, got %llu\n",
                   static_cast<unsigned long long>(simulation.pending_tick),
                   static_cast<unsigned long long>(simulation.expected_root),
                   static_cast<unsigned long long>(actual_root));
      failure = live_failure::diverged;
    }
  }
};

live_follower::live_follower(net_runtime& runtime, const core::causal_content& content,
                             const follower_config& config, const live_run_options& options,
                             devils_engine::thread::atomic_pool* pool)
  : state_(std::make_unique<state>(runtime, content, config, options, pool)) {}

live_follower::~live_follower() = default;

bool live_follower::start() {
  return state_->endpoint->start();
}

void live_follower::service() {
  auto& self = *state_;
  if (self.failure != live_failure::none) return;
  self.endpoint->step(self.checkpoint_sink);
  if (self.endpoint->grant()) self.grant = self.endpoint->grant();
  if (self.resuming) {
    if (!self.endpoint->live_ready()) return;
    self.resuming = false;
    ++self.resumes;
    // Со снимка кольца мир заменён целиком: свой игрок есть, если он есть в снимке.
    self.admitted = self.simulation.actors.player_of(self.endpoint->peer_id()) !=
                    devils_engine::aesthetics::invalid_entityid;
  }
  if (!self.prepared) {
    if (!self.endpoint->live_ready() || self.endpoint->report().status != join_status::loaded ||
        self.checkpoint_sink.checkpoint.empty())
      return;
    self.prepare();
  }
  for (auto& bytes : self.endpoint->take_canonical_ticks()) {
    self.stats.canonical_bytes += bytes.size();
    std::array<network::canonical_tick_record_view, network::canonical_tick_max_records> storage{};
    network::canonical_tick_view view;
    if (!decode_bundle(bytes, self.now(), storage, view)) {
      self.failure = live_failure::malformed_bundle;
      return;
    }
    if (self.awaiting_repair) self.repair(std::move(bytes), view);
    else self.follow(view);
    if (self.failure != live_failure::none) return;
    ++self.stats.bundles_verified;
  }
  self.send_outstanding(false);
}

bool live_follower::propose_food(const double x, const double y, const uint64_t target) {
  auto& self = *state_;
  if (!self.prepared || !self.admitted || self.outstanding.size() >= max_outstanding_inputs ||
      target <= self.now())
    return false;
  network::intent proposal;
  if (!encode_player_intent(food_action(x, y), uint16_t(self.next_sequence), 0, proposal))
    return false;
  self.outstanding.push_back({self.next_sequence, target, proposal, 0});
  ++self.next_sequence;
  self.send_outstanding(true);
  return true;
}

bool live_follower::resume() {
  auto& self = *state_;
  if (!self.prepared || !self.grant || self.failure == live_failure::malformed_bundle) return false;
  auto config = self.config;
  config.resume = resume_claim{*self.grant, self.now(), root_of(self.simulation)};
  self.endpoint.reset(); // прежнее соединение закрывается до нового
  self.endpoint = std::make_unique<follower>(self.runtime, self.content, config);
  self.failure = live_failure::none;
  self.awaiting_repair = false;
  self.received.clear();
  self.resuming = true;
  return self.endpoint->start();
}

bool live_follower::resuming() const noexcept {
  return state_->resuming;
}

bool live_follower::can_resume() const noexcept {
  return state_->prepared && state_->grant.has_value() &&
         state_->failure != live_failure::malformed_bundle;
}

uint32_t live_follower::hold_ms() const noexcept {
  return state_->grant ? state_->grant->hold_ms : 0;
}

uint32_t live_follower::resumes() const noexcept {
  return state_->resumes;
}

const world_declaration& live_follower::declared_world() const noexcept {
  return state_->world;
}

bool live_follower::ready() const noexcept {
  return state_->prepared;
}

bool live_follower::admitted() const noexcept {
  return state_->admitted;
}

bool live_follower::caught_up() const noexcept {
  return state_->prepared && state_->endpoint->catchup_tick() != 0 &&
         state_->now() >= state_->endpoint->catchup_tick();
}

uint64_t live_follower::tick() const noexcept {
  return state_->now();
}

const core::actor_world_slice& live_follower::world() const noexcept {
  return state_->simulation.actors;
}

uint64_t live_follower::root() const {
  return root_of(state_->simulation);
}

const live_follower::counters& live_follower::stats() const noexcept {
  return state_->stats;
}

live_failure live_follower::failure() const noexcept {
  return state_->failure;
}

const char* describe(const live_failure value) noexcept {
  switch (value) {
    case live_failure::none: return "none";
    case live_failure::malformed_bundle: return "the authority sent a malformed canonical bundle";
    case live_failure::diverged: return "the local world diverged from the authority";
  }
  return "unknown live failure";
}

follower& live_follower::endpoint() noexcept {
  return *state_->endpoint;
}

const follower& live_follower::endpoint() const noexcept {
  return *state_->endpoint;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Процессные драйверы.
// ─────────────────────────────────────────────────────────────────────────────────────────────

// Прогон кончился не так, как объявлен: сообщение и код возврата. Это вердикт процесса, а не
// критическая ошибка; критические (`utils::error`) уходят выше, к границе процесса.
int run_failed(const char* who, const char* reason) {
  std::fprintf(stderr, "frontier live %s: %s\n", who, reason);
  return 1;
}

int run_live_authority(const live_run_options& opts, const core::causal_content& content) {
  const auto failed = [](const char* reason) { return run_failed("authority", reason); };
  {
    spdlog::set_level(spdlog::level::err);
    if (opts.port == 0 || opts.final_tick < 64 ||
        !(opts.scripted_inputs ? opts.expected_peers == 2 : opts.expected_peers >= 1))
      return failed(opts.scripted_inputs
                      ? "live authority needs a port, two followers and at least 64 ticks"
                      : "live authority needs a port, a follower and at least 64 ticks");
    trace_live_phase(opts.verbose, "net_runtime");
    net_runtime runtime(4);
    trace_live_phase(opts.verbose, "endpoint");
    authority_config config;
    config.apply(opts.network);
    config.host = opts.bind_loopback ? 0x7f000001u : 0u;
    config.port = opts.port;
    config.max_peers = opts.expected_peers;
    config.verbose = opts.verbose;
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
    if (endpoint.completed_joins() == 0) return failed("timed out waiting for the first join");

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
    std::vector<uint64_t> told_shutdown;
    while (clock::now() < drain_deadline) {
      runtime.pump();
      session.service();
      // Сессия для людей: клиент не знает последнего тика прогона и без слова авторитета принял
      // бы закрытие за обрыв и стал бы входить заново. Процессные ведомые уходят сами. Слово
      // получает и тот, кто вошёл или вернулся уже после конца прогона, а не только живые в момент
      // объявления: иначе он ждал бы тиков до общего срока и увидел бы голое закрытие.
      if (!opts.scripted_inputs && session.delivered_through(opts.final_tick)) {
        for (const auto& join : endpoint.joins()) {
          if (!join.agreed || !endpoint.is_live_member(join.peer_id) ||
              std::find(told_shutdown.begin(), told_shutdown.end(), join.peer_id) != told_shutdown.end())
            continue;
          told_shutdown.push_back(join.peer_id);
          endpoint.terminate(join.peer_id, termination_reason::shutdown);
        }
      }
      if ((!opts.scripted_inputs || endpoint.completed_joins() == opts.expected_peers) &&
          session.delivered_through(opts.final_tick) && endpoint.connected_peers() == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto& stats = session.stats();
    std::printf("live.authority.tick = %llu\n", static_cast<unsigned long long>(session.tick()));
    std::printf("live.authority.root = %llu\n", static_cast<unsigned long long>(session.root()));
    std::printf("live.authority.joins = %u\n", endpoint.completed_joins());
    std::printf("live.authority.players = %u\n", stats.admitted_peers);
    std::printf("live.authority.departed = %u\n", stats.departed_peers);
    std::printf("live.authority.inputs = %u\n", stats.applied_inputs);
    std::printf("live.authority.dropped_inputs = %llu\n",
                static_cast<unsigned long long>(stats.dropped_inputs));
    std::printf("live.authority.terminated = %llu\n",
                static_cast<unsigned long long>(endpoint.termination_count()));
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
    // Процессный сценарий: ровно ожидаемые участники, никто не ушёл. Сессия для людей: уход и
    // повторный вход — штатные события, `--expect-joins` там только ёмкость; провал — если никто
    // так и не вошёл в мир.
    if (!opts.scripted_inputs)
      return stats.admitted_peers != 0 ? 0 : failed("nobody entered the world");
    if (endpoint.completed_joins() != opts.expected_peers || endpoint.termination_count() != 0 ||
        stats.admitted_peers != opts.expected_peers || stats.departed_peers != 0)
      return failed("did not complete its followers");
    if (stats.applied_inputs != opts.expected_peers || stats.dropped_inputs != 0)
      return failed("did not receive exactly one input from each follower");
    return 0;
  }
}

void trace_live_phase(const bool on, const char* name) {
  if (!on) return;
  static const auto origin = clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - origin).count();
  std::fprintf(stderr, "live.phase %s %lld ms\n", name, static_cast<long long>(ms));
}

int run_live_follower(const live_run_options& opts, const core::causal_content& content) {
  const auto failed = [](const char* reason) { return run_failed("follower", reason); };
  {
    spdlog::set_level(spdlog::level::err);
    const auto target = network::parse_endpoint(opts.endpoint);
    if (!target) return failed("needs HOST:PORT");
    const uint32_t address = target->host;
    const uint16_t port = target->port;
    net_runtime runtime(2);
    follower_config config;
    config.apply(opts.network);
    config.host = address;
    config.port = port;
    config.verbose = opts.verbose;
    live_follower session(runtime, content, config, opts);
    if (!session.start()) return failed("could not start the connection");
    const auto& endpoint = session.endpoint();
    const auto deadline = clock::now() + std::chrono::seconds(opts.timeout_seconds);
    const auto alive = [&]() {
      if (endpoint.fault() != follower_fault::none)
        std::fprintf(stderr, "live follower fault: %s\n", describe(endpoint.fault()));
      if (session.failure() != live_failure::none)
        std::fprintf(stderr, "live follower failure: %s\n", describe(session.failure()));
      return endpoint.fault() == follower_fault::none && !endpoint.live_terminal() &&
             endpoint.phase() != follower_phase::failed && session.failure() == live_failure::none;
    };
    while (!session.ready() && clock::now() < deadline) {
      runtime.pump();
      session.service();
      if (!alive()) return failed("failed before join confirmation");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!session.ready()) return failed("did not receive an acknowledged checkpoint");
    const uint64_t checkpoint_tick = session.stats().checkpoint_tick;
    std::printf("live.follower.joined_tick = %llu\n",
                static_cast<unsigned long long>(checkpoint_tick));
    std::fflush(stdout);
    if (checkpoint_tick >= opts.final_tick)
      return failed("joined too late to observe a canonical tick");

    bool sent_input = false;
    while (session.tick() < opts.final_tick && clock::now() < deadline) {
      runtime.pump();
      session.service();
      if (!alive()) return failed("lost the session before the final tick");
      if (!sent_input && session.caught_up() && session.admitted() &&
          session.tick() + 24 <= opts.final_tick) {
        if (!session.propose_food(12.0 + double(endpoint.peer_id()), 13.0, session.tick() + 24))
          return failed("could not send its intent");
        sent_input = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Уйти словом, а не закрытием: молча закрытое соединение — обрыв, и авторитет держал бы слот
    // окно удержания. Ответ авторитета — закрытие; ждать его, иначе слово уйдёт вместе с процессом.
    if (session.endpoint().leave()) {
      const auto leave_deadline = clock::now() + std::chrono::seconds(2);
      while (clock::now() < leave_deadline && !session.endpoint().drain()) {
        runtime.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
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
    if (session.tick() != opts.final_tick || !stats.corrected || !sent_input ||
        stats.own_inputs != 1)
      return failed("did not finish the tick/root/input run");
    return 0;
  }
}

} // namespace frontier_online::net
