#include "core/net_client.h"

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/log.h>

#ifdef FRONTIER_ONLINE_HAS_NETWORK
#  include <chrono>
#  include <thread>

#  include <devils_engine/network/reconnect.h>

#  include "core/actor_simulation.h"
#  include "core/causal_content.h"
#  include "net/link.h"
#  include "net/live_runtime.h"
#  include "net/network_settings.h"
#endif

namespace frontier_online::core {

namespace utils = devils_engine::utils;

#ifdef FRONTIER_ONLINE_HAS_NETWORK

namespace fnet = frontier_online::net;

bool network_client_available() noexcept {
  return true;
}

// Повторный вход (решения автора 2026-11-23). После обрыва клиент возвращается ТЕМ ЖЕ участником:
// авторитет держит его слот `hold_ms` (игрок стоит в мире без вводов), клиент приходит с ключом из
// `reconnect_grant` и своим подтверждённым тиком/корнем, авторитет догоняет его тиками из истории
// или снимком из кольца. Мир клиента, неподтверждённые предложения и номера ввода переживают
// соединение. Окно истекло или догнать не из чего (`unknown_session`) — новый вход новым участником
// со свежим checkpoint.
//
// Две машины `network::reconnect_coordinator`. Новый вход: срока нет (ключа ещё нет), предел —
// попытки; соединение, рукопожатие и загрузка — одно событие «вошёл». Продолжение: срок — окно
// удержания от момента обрыва, события — соединился / принят / подтверждён по фазам ведомого.
//
// Причины конца делятся на два рода. Повторить: обрыв, тишина, сроки фаз, отставание за историю,
// авторитет полон, рассинхрон с авторитетом (продолжение с чужим корнем авторитет догонит снимком).
// Конец сессии: несовместимая сборка, другой мир при присоединении, нарушения протокола с любой
// стороны, авторитет завершил прогон. Критические ошибки (отказ собственных ресурсов, нарушенный
// инвариант) сюда не попадают: это `utils::error`, и процесс их не переживает.
namespace {

using clock = std::chrono::steady_clock;

// Политика — из `network.tavl` (умолчания в network_settings.h: подозрение через 1 с без нового
// тика, потеря через 6 с — дольше собственного срока тишины ведомого, он точнее; пауза 0.5..8 с,
// 8 попыток).

// fresh — продолжать нечего (окно истекло, авторитет не держит сессию): только новый вход.
enum class attempt_end : uint8_t { none, retry, fresh, final };

struct attempt_outcome {
  attempt_end kind = attempt_end::none;
  const char* why = "";
};

attempt_outcome retry(const char* why) {
  return {attempt_end::retry, why};
}

attempt_outcome stop(const char* why) {
  return {attempt_end::final, why};
}

attempt_outcome classify_end(const fnet::live_follower& session) {
  switch (session.failure()) {
    case fnet::live_failure::none: break;
    case fnet::live_failure::diverged: return retry(fnet::describe(session.failure()));
    case fnet::live_failure::malformed_bundle: return stop(fnet::describe(session.failure()));
  }
  const auto& endpoint = session.endpoint();
  using refusal = devils_engine::network::session_refusal_reason;
  if (const auto reason = endpoint.refusal(); reason != refusal::none) {
    if (reason == refusal::unknown_session) return {attempt_end::fresh, fnet::describe(reason)};
    return reason == refusal::no_capacity ? retry(fnet::describe(reason)) : stop(fnet::describe(reason));
  }
  if (const auto reason = endpoint.termination()) {
    switch (*reason) {
      case fnet::termination_reason::history_exhausted:
      case fnet::termination_reason::timeout: return retry(fnet::describe(*reason));
      case fnet::termination_reason::violations:
      case fnet::termination_reason::shutdown:
      case fnet::termination_reason::disagreement: return stop(fnet::describe(*reason));
    }
    return stop(fnet::describe(*reason));
  }
  switch (endpoint.fault()) {
    case fnet::follower_fault::none:
    case fnet::follower_fault::terminated: break;
    case fnet::follower_fault::protocol_violation:
    case fnet::follower_fault::insane_authority: return stop(fnet::describe(endpoint.fault()));
    default: return retry(fnet::describe(endpoint.fault()));
  }
  // Провал присоединения без сетевой причины — мир или checkpoint не сошлись: повтор даст то же.
  if (endpoint.phase() == fnet::follower_phase::failed)
    return stop(fnet::describe(endpoint.report().status));
  if (endpoint.live_terminal()) return retry("the connection closed without a reason");
  return {};
}

const char* describe(const devils_engine::network::abandon_reason reason) noexcept {
  using devils_engine::network::abandon_reason;
  switch (reason) {
    case abandon_reason::attempts_exhausted: return "reconnect attempts exhausted";
    case abandon_reason::deadline_passed: return "reconnect deadline passed";
    case abandon_reason::refused: return "refused";
    case abandon_reason::unrecoverable: return "unrecoverable";
    case abandon_reason::none: break;
  }
  return "gave up";
}

} // namespace

struct net_client::state {
  // Одна попытка входа — один ведомый. Сеть и причинное содержимое общие для всех попыток.
  struct attempt {
    fnet::live_follower follower;
    uint64_t last_tick = 0;
    bool joined = false;

    attempt(fnet::net_runtime& runtime, const causal_content& content,
            const fnet::follower_config& config, devils_engine::thread::atomic_pool& pool)
      : follower(runtime, content, config, live_options(), &pool) {}

    static fnet::live_run_options live_options() {
      fnet::live_run_options options;
      options.predict_wrong = false; // показ проверенного мира: прогноза нет, нечего и портить
      return options;
    }
  };

  join_request_t request;
  fnet::follower_config config;
  devils_engine::thread::atomic_pool& pool;
  std::unique_ptr<fnet::net_runtime> own_runtime; // пусто, когда рантайм общий
  fnet::net_runtime& runtime;
  causal_content content;
  devils_engine::network::reconnect_policy policy;
  devils_engine::network::reconnect_coordinator machine;
  clock::time_point origin = clock::now();
  std::unique_ptr<attempt> current;
  uint64_t last_tick = 0; // последний проверенный тик любой из попыток — для лога
  uint64_t last_target = 0; // тик цели последнего своего клика и сколько кликов на нём
  uint32_t target_uses = 0;
  uint64_t progress_ms = 0; // когда проверенный тик сдвинулся в последний раз (часы now())
  uint32_t sessions = 0;  // сколько раз вошли в мир новым участником
  uint32_t resumes = 0;   // сколько раз вернулись тем же участником
  // Продолжение: `current` хранит мир, машина — со сроком окна удержания. `paused` — соединение
  // потеряно, ведомый не обслуживается до следующей попытки.
  bool resuming = false;
  bool paused = false;
  bool saw_connected = false;
  bool saw_admitted = false;
  bool ended = false;

  state(const join_request_t& value, const fnet::follower_config& follower, const std::string& resource_root,
        devils_engine::thread::atomic_pool& frame_pool, fnet::net_runtime* shared,
        const fnet::network_settings& settings_value)
    : request(value), config(follower), pool(frame_pool),
      own_runtime(shared == nullptr ? std::make_unique<fnet::net_runtime>(1) : nullptr),
      runtime(shared != nullptr ? *shared : *own_runtime), content(resource_root),
      policy(fnet::reconnect_policy_of(settings_value)),
      machine(policy, UINT64_MAX, 0) {
    machine.observe_transport_lost(0); // первый вход идёт тем же путём, что повторный
  }

  uint64_t now() const noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - origin).count());
  }

  void finish(const char* why) {
    ended = true;
    current.reset();
    utils::warn("frontier_online: network session ended at tick {}: {}", last_tick, why);
  }

  // Новый вход: машина без срока, первый вход идёт тем же путём, что повторный.
  void restart_fresh(const uint64_t instant) {
    current.reset();
    resuming = false;
    paused = false;
    machine = devils_engine::network::reconnect_coordinator(policy, UINT64_MAX, instant);
    machine.observe_transport_lost(instant);
  }

  // Продолжение: срок — окно удержания, отсчитанное от обрыва (по своим часам; авторитет держит
  // слот по своим, поэтому опоздание даёт отказ `unknown_session`, а не чужого игрока).
  void begin_resume(const uint64_t instant) {
    resuming = true;
    paused = true;
    machine = devils_engine::network::reconnect_coordinator(
      policy, instant + current->follower.hold_ms(), instant);
    machine.observe_transport_lost(instant);
  }

  void open_resume(const uint64_t instant) {
    paused = false;
    saw_connected = false;
    saw_admitted = false;
    utils::info("frontier_online: returning to {} as peer {} (attempt {})", request.endpoint,
                current->follower.endpoint().peer_id(), machine.attempts());
    if (!current->follower.resume()) {
      utils::warn("frontier_online: could not open a connection to {}", request.endpoint);
      paused = true;
      machine.observe_connect_failed(instant);
    }
  }

  void start_attempt(const uint64_t instant) {
    current.reset(); // повторный вход — новое соединение, старое не переиспользуется
    current = std::make_unique<attempt>(runtime, content, config, pool);
    if (!current->follower.start()) {
      utils::warn("frontier_online: could not open a connection to {}", request.endpoint);
      current.reset();
      machine.observe_connect_failed(instant);
      return;
    }
    if (machine.attempts() > 1 || sessions != 0)
      utils::info("frontier_online: entering {} again (attempt {})", request.endpoint, machine.attempts());
  }

  void lose(const uint64_t instant, const char* why) {
    const bool was_joined = current && current->joined;
    if (resuming) {
      // Попытка продолжения сорвалась: мир остаётся, следующая попытка — по машине продолжения.
      utils::warn("frontier_online: attempt {} to return to {} failed: {}", machine.attempts(),
                  request.endpoint, why);
      paused = true;
      if (saw_admitted) machine.observe_transport_lost(instant);
      else machine.observe_connect_failed(instant);
      return;
    }
    if (was_joined && current->follower.can_resume()) {
      utils::warn("frontier_online: lost the session at tick {}: {}; returning within {} ms", last_tick,
                  why, current->follower.hold_ms());
      begin_resume(instant);
      return;
    }
    current.reset();
    if (was_joined) {
      utils::warn("frontier_online: lost the session at tick {}: {}; entering again", last_tick, why);
      machine.observe_transport_lost(instant);
    } else {
      utils::warn("frontier_online: attempt {} to enter {} failed: {}", machine.attempts(),
                  request.endpoint, why);
      machine.observe_connect_failed(instant);
    }
  }

  void service_attempt(const uint64_t instant) {
    auto& follower = current->follower;
    follower.service();
    if (resuming) {
      const auto& endpoint = follower.endpoint();
      if (!saw_connected && endpoint.connected()) {
        saw_connected = true;
        machine.observe_connected(instant);
      }
      if (!saw_admitted && endpoint.established()) {
        saw_admitted = true;
        machine.observe_admitted(instant);
      }
      if (!follower.resuming() && follower.failure() == fnet::live_failure::none) {
        resuming = false;
        ++resumes;
        machine.observe_recovered(instant);
        utils::info("frontier_online: returned as peer {} at tick {} ({})", endpoint.peer_id(),
                    follower.tick(),
                    endpoint.plan() && endpoint.plan()->mode == fnet::resume_mode::checkpoint
                      ? "from a snapshot" : "from history");
      }
    }
    if (!current->joined && follower.ready()) {
      current->joined = true;
      ++sessions;
      machine.observe_connected(instant);
      machine.observe_admitted(instant);
      machine.observe_recovered(instant);
      utils::info("frontier_online: joined as peer {} at tick {}", follower.endpoint().peer_id(),
                  follower.stats().checkpoint_tick);
    }
    if (current->joined && follower.tick() != current->last_tick) {
      current->last_tick = follower.tick();
      last_tick = follower.tick();
      progress_ms = instant;
      machine.observe_traffic(instant);
    }
    const auto outcome = classify_end(follower);
    if (outcome.kind == attempt_end::final) {
      machine.observe_refused(instant);
      return finish(outcome.why);
    }
    if (outcome.kind == attempt_end::fresh) {
      utils::warn("frontier_online: the authority no longer holds peer {} ({}); entering anew",
                  follower.endpoint().peer_id(), outcome.why);
      machine.observe_unrecoverable(instant);
      return;
    }
    if (outcome.kind == attempt_end::retry) lose(instant, outcome.why);
  }

  // Добровольный уход: словом `leave`, а не закрытием — молча закрытое соединение авторитет считает
  // обрывом и держит слот окно удержания. Ответ авторитета — закрытие; ждать его коротко, иначе
  // слово уйдёт вместе с процессом (GNS закрывает без linger).
  void leave() {
    if (ended || paused || current == nullptr || !current->joined) return;
    auto& endpoint = current->follower.endpoint();
    if (!endpoint.leave()) return;
    const auto deadline = clock::now() + std::chrono::milliseconds(500);
    while (clock::now() < deadline && !endpoint.drain()) {
      runtime.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ended = true;
  }

  void service() {
    if (ended) return;
    const uint64_t instant = now();
    runtime.pump();
    if (current && !paused) {
      service_attempt(instant);
      if (ended) return;
    }
    using devils_engine::network::reconnect_action;
    switch (const auto decision = machine.poll(instant); decision.action) {
      case reconnect_action::warn:
        utils::warn("frontier_online: no new tick from the authority for {} ms after tick {}",
                    policy.suspect_after, last_tick);
        break;
      case reconnect_action::connect:
        if (current && current->joined && !resuming)
          utils::warn("frontier_online: the authority is silent for {} ms after tick {}; entering again",
                      policy.lost_after, last_tick);
        if (resuming) open_resume(instant);
        else start_attempt(instant);
        break;
      case reconnect_action::rejoin:
        // Продолжать нечего: новый вход новым участником.
        restart_fresh(instant);
        break;
      case reconnect_action::give_up:
        // Окно удержания прошло — авторитет слот уже отпустил: новый вход, а не конец.
        if (resuming && machine.abandoned_because() == devils_engine::network::abandon_reason::deadline_passed) {
          utils::warn("frontier_online: the hold window passed; entering {} anew", request.endpoint);
          restart_fresh(instant);
          break;
        }
        return finish(describe(machine.abandoned_because()));
      default: break;
    }
  }
};

net_client::net_client(std::unique_ptr<state> value) noexcept : state_(std::move(value)) {}
net_client::~net_client() {
  if (state_ != nullptr) state_->leave();
}

std::unique_ptr<net_client> net_client::connect(const join_request_t& request,
                                                const std::string& resource_root,
                                                devils_engine::thread::atomic_pool& pool,
                                                fnet::net_runtime* runtime) {
  fnet::follower_config config;
  const auto endpoint = devils_engine::network::parse_endpoint(request.endpoint);
  if (!endpoint) {
    utils::warn("frontier_online: --join='{}' is not HOST:PORT (IPv4)", request.endpoint);
    return nullptr;
  }
  config.host = endpoint->host;
  config.port = endpoint->port;
  config.verbose = false;
  const auto settings = fnet::load_network_settings();
  config.apply(settings);
  utils::info("frontier_online: joining {} (causal content from '{}')", request.endpoint, resource_root);
  return std::unique_ptr<net_client>(
    new net_client(std::make_unique<state>(request, config, resource_root, pool, runtime, settings)));
}

void net_client::service() {
  state_->service();
}

bool net_client::propose_food(const double x, const double y) {
  auto& self = *state_;
  if (self.ended || !self.current || !self.current->joined || !self.current->follower.admitted())
    return false;
  auto& follower = self.current->follower;
  // Авторитет принимает не больше records_per_peer_per_tick записей участника на тик. Пока
  // проверенный тик стоит (задержка), все клики получили бы одну цель: лишние сдвигаются на
  // следующие тики, а не отбрасываются авторитетом.
  uint64_t target = follower.tick() + self.request.input_lead_ticks;
  if (target <= self.last_target) {
    target = self.target_uses < fnet::live_run_options{}.records_per_peer_per_tick ? self.last_target
                                                                                    : self.last_target + 1;
  }
  if (!follower.propose_food(x, y, target)) return false;
  self.target_uses = target == self.last_target ? self.target_uses + 1 : 1;
  self.last_target = target;
  return true;
}

bool net_client::layout(session_world& out) const {
  const auto* world = this->world();
  if (world == nullptr) return false;
  const auto& declared = state_->current->follower.declared_world();
  out.generator = declared.generator;
  out.world_seed = declared.world_seed;
  out.chunk_size = declared.chunk_size;
  out.min_x = world->world_min().x;
  out.min_y = world->world_min().y;
  out.max_x = world->world_max().x;
  out.max_y = world->world_max().y;
  return true;
}

const actor_world_slice* net_client::world() const noexcept {
  const auto& current = state_->current;
  return current && current->follower.ready() ? &current->follower.world() : nullptr;
}

uint64_t net_client::tick() const noexcept {
  return state_->last_tick;
}

uint32_t net_client::sessions() const noexcept {
  return state_->sessions;
}

uint32_t net_client::resumes() const noexcept {
  return state_->resumes;
}

uint64_t net_client::peer() const noexcept {
  const auto& current = state_->current;
  return current && current->joined ? current->follower.endpoint().peer_id() : 0;
}

bool net_client::ended() const noexcept {
  return state_->ended;
}

net_status net_client::status() const {
  const auto& self = *state_;
  net_status out;
  out.tick = self.last_tick;
  out.sessions = self.sessions;
  out.resumes = self.resumes;
  out.attempts = self.machine.attempts();
  const uint64_t now = self.now();
  out.silence_ms = now > self.progress_ms ? now - self.progress_ms : 0;
  if (self.ended) {
    out.phase = "ended";
  } else if (self.resuming) {
    out.phase = "resuming";
  } else if (self.current && self.current->joined && self.current->follower.admitted()) {
    out.phase = "live";
  }
  if (self.current) {
    const auto& follower = self.current->follower;
    if (self.current->joined) out.peer = follower.endpoint().peer_id();
    const uint64_t catchup = follower.endpoint().catchup_tick();
    out.catchup_tick = catchup > follower.tick() ? catchup : 0;
    out.own_inputs = follower.stats().own_inputs;
    out.rejected_inputs = follower.stats().rejected_inputs;
    if (follower.ready()) out.players = follower.world().session_player_count();
  }
  return out;
}

#else

struct net_client::state {};

bool network_client_available() noexcept {
  return false;
}

net_client::net_client(std::unique_ptr<state> value) noexcept : state_(std::move(value)) {}
net_client::~net_client() = default;

std::unique_ptr<net_client> net_client::connect(const join_request_t&, const std::string&,
                                                devils_engine::thread::atomic_pool&,
                                                net::net_runtime*) {
  utils::warn("frontier_online: this build has no network backend; --join is unavailable");
  return nullptr;
}

void net_client::service() {}
bool net_client::layout(session_world&) const {
  return false;
}
net_status net_client::status() const {
  net_status out;
  out.phase = "ended";
  return out;
}
bool net_client::propose_food(double, double) {
  return false;
}
const actor_world_slice* net_client::world() const noexcept {
  return nullptr;
}
uint64_t net_client::tick() const noexcept {
  return 0;
}
uint32_t net_client::sessions() const noexcept {
  return 0;
}
uint32_t net_client::resumes() const noexcept {
  return 0;
}
uint64_t net_client::peer() const noexcept {
  return 0;
}
bool net_client::ended() const noexcept {
  return true;
}

#endif

} // namespace frontier_online::core
