#include "core/net_client.h"

#include <devils_engine/utils/core.h>

#ifdef FRONTIER_ONLINE_HAS_NETWORK
#  include <exception>

#  include "core/causal_content.h"
#  include "net/link.h"
#  include "net/live_runtime.h"
#endif

namespace frontier_online::core {

namespace utils = devils_engine::utils;

join_request_t& join_request() {
  static join_request_t value;
  return value;
}

#ifdef FRONTIER_ONLINE_HAS_NETWORK

namespace fnet = frontier_online::net;

bool network_client_available() noexcept {
  return true;
}

struct net_client::state {
  join_request_t request;
  fnet::net_runtime runtime{1};
  causal_content content;
  fnet::live_follower follower;
  bool announced = false;
  bool ended = false;

  state(const join_request_t& value, fnet::follower_config config, const std::string& resource_root,
        devils_engine::thread::atomic_pool& pool)
    : request(value), content(resource_root),
      follower(runtime, content, config, live_options(), &pool) {}

  static fnet::live_run_options live_options() {
    fnet::live_run_options options;
    options.predict_wrong = false; // показ проверенного мира: прогноза нет, нечего и портить
    return options;
  }

  void end(const char* why) {
    ended = true;
    utils::warn("frontier_online: network session ended at tick {}: {}", follower.tick(), why);
  }

  void check() {
    const auto& endpoint = follower.endpoint();
    if (endpoint.refusal() != devils_engine::network::session_refusal_reason::none)
      return end(fnet::describe(endpoint.refusal()));
    if (const auto reason = endpoint.termination()) return end(fnet::describe(*reason));
    if (endpoint.fault() != fnet::follower_fault::none) return end(fnet::describe(endpoint.fault()));
    if (endpoint.phase() == fnet::follower_phase::failed) return end(fnet::describe(endpoint.report().status));
    if (endpoint.live_terminal()) return end("the session closed");
  }
};

net_client::net_client(std::unique_ptr<state> value) noexcept : state_(std::move(value)) {}
net_client::~net_client() = default;

std::unique_ptr<net_client> net_client::connect(const join_request_t& request,
                                                const std::string& resource_root,
                                                devils_engine::thread::atomic_pool& pool) {
  fnet::follower_config config;
  if (!fnet::parse_endpoint(request.endpoint, config.host, config.port)) {
    utils::warn("frontier_online: --join='{}' is not HOST:PORT (IPv4)", request.endpoint);
    return nullptr;
  }
  config.verbose = false;
  auto value = std::make_unique<state>(request, config, resource_root, pool);
  if (!value->follower.start()) {
    utils::warn("frontier_online: could not open a connection to {}", request.endpoint);
    return nullptr;
  }
  utils::info("frontier_online: joining {} (causal content from '{}')", request.endpoint, resource_root);
  return std::unique_ptr<net_client>(new net_client(std::move(value)));
}

void net_client::service() {
  auto& self = *state_;
  if (self.ended) return;
  // Расхождение корня и отвергнутый bundle ведомый сообщает исключением: это конец сессии с
  // причиной, процесс клиента с окном продолжает жить на последнем проверенном мире.
  try {
    self.runtime.pump();
    self.follower.service();
  } catch (const std::exception& error) {
    return self.end(error.what());
  }
  if (!self.announced && self.follower.ready()) {
    self.announced = true;
    utils::info("frontier_online: joined as peer {} at tick {}", self.follower.endpoint().peer_id(),
                self.follower.stats().checkpoint_tick);
  }
  self.check();
}

bool net_client::propose_food(const double x, const double y) {
  auto& self = *state_;
  if (self.ended || !self.follower.admitted()) return false;
  return self.follower.propose_food(x, y, self.follower.tick() + self.request.input_lead_ticks);
}

const actor_world_slice* net_client::world() const noexcept {
  return state_->follower.ready() ? &state_->follower.world() : nullptr;
}

uint64_t net_client::tick() const noexcept {
  return state_->follower.tick();
}

bool net_client::ended() const noexcept {
  return state_->ended;
}

#else

struct net_client::state {};

bool network_client_available() noexcept {
  return false;
}

net_client::net_client(std::unique_ptr<state> value) noexcept : state_(std::move(value)) {}
net_client::~net_client() = default;

std::unique_ptr<net_client> net_client::connect(const join_request_t&, const std::string&,
                                                devils_engine::thread::atomic_pool&) {
  utils::warn("frontier_online: this build has no network backend; --join is unavailable");
  return nullptr;
}

void net_client::service() {}
bool net_client::propose_food(double, double) {
  return false;
}
const actor_world_slice* net_client::world() const noexcept {
  return nullptr;
}
uint64_t net_client::tick() const noexcept {
  return 0;
}
bool net_client::ended() const noexcept {
  return true;
}

#endif

} // namespace frontier_online::core
