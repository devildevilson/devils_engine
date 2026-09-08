#ifndef DEVILS_ENGINE_NETLAB01_LINK_H
#define DEVILS_ENGINE_NETLAB01_LINK_H

#include "lab.h"

#include <chrono>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <devils_engine/network/gns_transport.h>

// The transport side of NET-LAB-01: one real GNS endpoint per process, with the
// lane mapping the design document declares. Nothing here knows the causal
// state or the session; a reconnect is a NEW connection and this wrapper says
// so by handing out a fresh peer rather than reviving the old one.

namespace netlab01 {

// Wall instants are milliseconds from process start. They are the caller-declared
// unit for credential validity and the reconnect deadline, and the follower
// anchors them to the authority's issue instant, because two machines' monotonic
// clocks share no origin.
inline uint64_t monotonic_ms() {
  static const auto origin = std::chrono::steady_clock::now();
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - origin)
                    .count());
}

struct gns_runtime {
  SteamNetworkingErrMsg error{};
  bool initialized = false;

  gns_runtime() {
    initialized = GameNetworkingSockets_Init(nullptr, error);
    if (!initialized) utils::error{}("NET-LAB-01: GNS init failed: {}", error);
  }
  ~gns_runtime() {
    if (initialized) GameNetworkingSockets_Kill();
  }
  gns_runtime(const gns_runtime&) = delete;
  gns_runtime& operator=(const gns_runtime&) = delete;
};

inline constexpr net::gns_transport_config lab_transport_config{
  // One slot per follower plus two: a reconnect overlaps the connection it
  // replaces, and the spare is what lets an over-capacity join be REFUSED by
  // the session layer with a reason rather than by the transport with silence.
  .peers = lab_max_followers + 2,
  .receive_leases = 16,
  .max_receive_bytes = 64 * 1024,
  .backend_receive_messages = 256,
  .backend_receive_bytes = 4 * 1024 * 1024,
  .listeners = 1,
};

inline std::array<net::gns_lane_config, 3> lab_lanes() {
  std::array<net::gns_lane_config, 3> lanes{};
  // The byte budget cannot exceed what the declared slots can hold: the slabs
  // ARE the budget, and asking for more bytes than slots is a contradiction the
  // adapter refuses at construction.
  // Control: canonical bundles, session control. Highest priority.
  lanes[lab_lane_control] = {net::gns_delivery::reliable_ordered, 0, 1, 96,
                             lab_max_message_bytes, 96 * lab_max_message_bytes};
  // Bulk: checkpoint chunks. Reliable but explicitly lower priority, so a
  // recovery transfer cannot head-of-line block the current bundles.
  lanes[lab_lane_bulk] = {net::gns_delivery::reliable_ordered, 2, 1, 192,
                          lab_max_message_bytes, 192 * lab_max_message_bytes};
  // Intent proposals: unreliable sequenced, because redundancy across a tick
  // window is the recovery mechanism for this class, not retransmission.
  lanes[lab_lane_intent] = {net::gns_delivery::unreliable_sequenced, 1, 1, 16,
                            lab_max_message_bytes, 16 * lab_max_message_bytes};
  return lanes;
}

enum class lab_send_result : uint8_t { sent, no_peer, backpressure, refused };

class lab_link {
public:
  lab_link()
    : lanes_(lab_lanes()),
      dispatcher_(*SteamNetworkingSockets(), *SteamNetworkingUtils(), 1),
      transport_(dispatcher_, lab_transport_config, lanes_) {
    if (!transport_.ready()) utils::error{}("NET-LAB-01: transport not ready");
    scratch_.reserve(lab_max_message_bytes);
  }

  // Standalone GNS has no configured certificate authority, so an explicitly
  // unauthenticated laboratory session says so once, here, instead of letting
  // packet encryption be mistaken for identity.
  static std::array<SteamNetworkingConfigValue_t, 1> unauthenticated_options() {
    std::array<SteamNetworkingConfigValue_t, 1> options{};
    options[0].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);
    return options;
  }

  struct listen_outcome {
    uint16_t port = 0;
    bool ephemeral = false;
  };

  // Asking the operating system for a port is the right way and the port the
  // authority then publishes is whatever it got. The pinned GNS backend does
  // not accept port zero, so the fallback is a scan -- recorded rather than
  // hidden, because "the transport refuses an ephemeral bind" is a real
  // property of the backend and not a detail of this stand.
  // A declared port, for a run whose peers are on other machines and cannot
  // read a rendezvous file. There is no fallback here on purpose: an operator
  // who named a port and silently got another one would point the followers at
  // the wrong one.
  listen_outcome listen_on(const uint32_t host, const uint16_t port) {
    auto options = unauthenticated_options();
    SteamNetworkingIPAddr address{};
    address.SetIPv4(host, port);
    const auto result = transport_.listen(address, options);
    if (result.status != net::gns_status::ok)
      utils::error{}("NET-LAB-01: listen on the declared port refused, status {}",
                     unsigned(result.status));
    listener_ = result.listener;
    return {port, false};
  }

  listen_outcome listen_any(const uint32_t host = 0x7f000001) {
    auto options = unauthenticated_options();
    SteamNetworkingIPAddr address{};
    address.SetIPv4(host, 0);
    auto result = transport_.listen(address, options);
    if (result.status == net::gns_status::ok) {
      listener_ = result.listener;
      SteamNetworkingIPAddr bound{};
      if (transport_.listen_address(listener_, bound) == net::gns_status::ok && bound.m_port != 0)
        return {bound.m_port, true};
      (void)transport_.close_listener(listener_);
    }
    for (uint16_t port = 41200; port < 41456; ++port) {
      address.m_port = port;
      result = transport_.listen(address, options);
      if (result.status == net::gns_status::ok) {
        listener_ = result.listener;
        return {port, false};
      }
      if (result.status != net::gns_status::backend_rejected)
        utils::error{}("NET-LAB-01: listen refused, status {}", unsigned(result.status));
    }
    utils::error{}("NET-LAB-01: no free port in the declared laboratory range");
    return {};
  }

  bool connect_to(const uint16_t port, const uint32_t host = 0x7f000001) {
    auto options = unauthenticated_options();
    SteamNetworkingIPAddr address{};
    address.SetIPv4(host, port);
    const auto result = transport_.connect(address, options);
    if (result.status != net::gns_status::ok) return false;
    pending_ = result.peer;
    return true;
  }

  struct observation {
    net::gns_peer peer;
    bool connected = false;
    bool terminal = false;
    bool needs_accept = false;
  };

  // Coalesced observations, exactly as the adapter promises: this is a view of
  // current state, not a lossless callback log.
  size_t poll(std::span<observation> out) {
    dispatcher_.pump();
    std::array<net::gns_send_release, 16> released;
    while (transport_.poll_send_releases(released) != 0) {}
    std::array<net::gns_connection_event, 4> events;
    const size_t count = transport_.poll_connections(events);
    size_t written = 0;
    for (size_t i = 0; i < count && written < out.size(); ++i) {
      const auto& event = events[i];
      observation value{event.peer};
      value.needs_accept = event.needs_accept;
      value.connected = event.info.m_eState == k_ESteamNetworkingConnectionState_Connected;
      value.terminal =
        event.info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        event.info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
      out[written++] = value;
    }
    return written;
  }

  net::gns_status accept(const net::gns_peer peer) {
    return transport_.accept(peer);
  }
  net::gns_status close(const net::gns_peer peer) {
    return transport_.close(peer);
  }
  // The peer a connect() produced. It is valid immediately; being connected is
  // a later observation, and a reconnect always produces a different one.
  net::gns_peer pending() const noexcept {
    return pending_;
  }

  lab_send_result send(const net::gns_peer peer, const uint16_t lane,
                       const std::span<const std::byte> bytes) {
    const auto result = transport_.try_send(peer, lane, bytes);
    switch (result.status) {
      case net::gns_status::ok: return lab_send_result::sent;
      case net::gns_status::invalid_peer:
      case net::gns_status::invalid_state: return lab_send_result::no_peer;
      case net::gns_status::count_budget_exceeded:
      case net::gns_status::byte_budget_exceeded: return lab_send_result::backpressure;
      default: return lab_send_result::refused;
    }
  }

  // Consumes at most one native pump's worth of messages. The lease is released
  // as soon as the consumer returns, so no payload outlives this call.
  template <class Consumer>
  size_t receive(Consumer&& consumer) {
    size_t total = 0;
    for (unsigned round = 0; round < 8; ++round) {
      std::array<net::gns_received_message, 4> input;
      const auto result = transport_.receive(input);
      if (result.status != net::gns_status::ok)
        utils::error{}("NET-LAB-01: receive refused, status {}", unsigned(result.status));
      if (result.count == 0) break;
      for (size_t i = 0; i < result.count; ++i) {
        consumer(input[i].peer(), input[i].lane(), input[i].payload());
        ++total;
      }
    }
    return total;
  }

  std::vector<std::byte>& scratch() noexcept {
    return scratch_;
  }

  // Real conditions, measured rather than assumed. On loopback these are
  // near-zero, and saying so is the point: the numbers are here so that a LAN
  // run reports what it actually cost instead of inheriting a loopback claim.
  struct conditions {
    bool available = false;
    int ping_ms = 0;
    float quality_local = 0, quality_remote = 0;
    float out_packets_per_second = 0, in_packets_per_second = 0;
    int pending_reliable_bytes = 0;
  };

  conditions measure(const net::gns_peer peer) {
    SteamNetConnectionRealTimeStatus_t status{};
    conditions value;
    if (transport_.statistics(peer, status) != net::gns_status::ok) return value;
    value.available = true;
    value.ping_ms = status.m_nPing;
    value.quality_local = status.m_flConnectionQualityLocal;
    value.quality_remote = status.m_flConnectionQualityRemote;
    value.out_packets_per_second = status.m_flOutPacketsPerSec;
    value.in_packets_per_second = status.m_flInPacketsPerSec;
    value.pending_reliable_bytes = status.m_cbPendingReliable;
    return value;
  }

private:
  std::array<net::gns_lane_config, 3> lanes_;
  net::gns_dispatcher dispatcher_;
  net::gns_transport transport_;
  net::gns_listener listener_;
  net::gns_peer pending_;
  std::vector<std::byte> scratch_;
};

} // namespace netlab01

#endif
