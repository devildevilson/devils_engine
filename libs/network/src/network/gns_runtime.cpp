#include "devils_engine/network/gns_runtime.h"

#include <mutex>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <devils_engine/utils/core.h>

namespace devils_engine::network {

namespace {
std::mutex gns_lifetime_mutex;
size_t gns_runtime_count = 0;
} // namespace

gns_runtime::gns_runtime(const size_t endpoints) {
  {
    const std::lock_guard lock(gns_lifetime_mutex);
    if (gns_runtime_count == 0) {
      SteamNetworkingErrMsg error{};
      if (!GameNetworkingSockets_Init(nullptr, error)) {
        utils::error{}("network: GNS init failed: {}", error);
      }
    }
    ++gns_runtime_count;
  }
  dispatcher_ = std::make_unique<gns_dispatcher>(*SteamNetworkingSockets(), *SteamNetworkingUtils(), endpoints);
}

gns_runtime::~gns_runtime() {
  dispatcher_.reset();
  const std::lock_guard lock(gns_lifetime_mutex);
  if (--gns_runtime_count == 0) GameNetworkingSockets_Kill();
}

std::array<SteamNetworkingConfigValue_t, 1> gns_unauthenticated_options() noexcept {
  std::array<SteamNetworkingConfigValue_t, 1> options{};
  options[0].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
  return options;
}

gns_listen_outcome gns_listen_on(gns_transport& transport, const uint32_t host, const uint16_t port) {
  auto options = gns_unauthenticated_options();
  SteamNetworkingIPAddr address{};
  address.SetIPv4(host, port);
  const auto result = transport.listen(address, options);
  return {result.status, result.listener, port, false};
}

gns_listen_outcome gns_listen_any(gns_transport& transport, const uint32_t host, const uint16_t first_port,
                                  const uint16_t port_count) {
  auto options = gns_unauthenticated_options();
  SteamNetworkingIPAddr address{};
  address.SetIPv4(host, 0);
  auto result = transport.listen(address, options);
  if (result.status == gns_status::ok) {
    SteamNetworkingIPAddr bound{};
    if (transport.listen_address(result.listener, bound) == gns_status::ok && bound.m_port != 0) {
      return {gns_status::ok, result.listener, bound.m_port, true};
    }
    (void)transport.close_listener(result.listener);
  }
  for (uint32_t port = first_port; port < uint32_t(first_port) + port_count && port <= 65535; ++port) {
    address.m_port = uint16_t(port);
    result = transport.listen(address, options);
    if (result.status == gns_status::ok) return {gns_status::ok, result.listener, uint16_t(port), false};
    if (result.status != gns_status::backend_rejected) return {result.status, {}, uint16_t(port), false};
  }
  return {gns_status::backend_rejected, {}, 0, false};
}

gns_send_outcome classify_send(const gns_status status) noexcept {
  switch (status) {
    case gns_status::ok: return gns_send_outcome::sent;
    case gns_status::invalid_peer:
    case gns_status::invalid_state: return gns_send_outcome::no_peer;
    case gns_status::peer_budget_exceeded:
    case gns_status::count_budget_exceeded:
    case gns_status::byte_budget_exceeded: return gns_send_outcome::backpressure;
    default: return gns_send_outcome::refused;
  }
}

std::string_view describe(const gns_status status) noexcept {
  switch (status) {
    case gns_status::ok: return "ok";
    case gns_status::not_ready: return "not ready";
    case gns_status::invalid_peer: return "invalid peer";
    case gns_status::invalid_lane: return "invalid lane";
    case gns_status::peer_capacity_exceeded: return "peer capacity exceeded";
    case gns_status::already_owned: return "already owned";
    case gns_status::configuration_failed: return "configuration failed";
    case gns_status::payload_too_large: return "payload too large";
    case gns_status::count_budget_exceeded: return "count budget exceeded";
    case gns_status::byte_budget_exceeded: return "byte budget exceeded";
    case gns_status::backend_rejected: return "backend rejected";
    case gns_status::output_not_empty: return "output not empty";
    case gns_status::invalid_message: return "invalid message";
    case gns_status::invalid_listener: return "invalid listener";
    case gns_status::listener_capacity_exceeded: return "listener capacity exceeded";
    case gns_status::invalid_options: return "invalid options";
    case gns_status::invalid_state: return "invalid state";
    case gns_status::peer_budget_exceeded: return "peer budget exceeded";
  }
  return "unknown gns status";
}

} // namespace devils_engine::network
