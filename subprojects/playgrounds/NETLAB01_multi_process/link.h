#ifndef DEVILS_ENGINE_NETLAB01_LINK_H
#define DEVILS_ENGINE_NETLAB01_LINK_H

#include "lab.h"

#include <chrono>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <devils_engine/network/gns_runtime.h>
#include <devils_engine/network/secure_random.h>
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

// GNS Init/Kill и диспетчер — движковый net::gns_runtime (network/gns_runtime.h): владелец считается,
// первый поднимает, последний гасит.

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

using lab_send_result = net::gns_send_outcome;

class lab_link {
public:
  explicit lab_link(net::gns_dispatcher& dispatcher)
    : lanes_(lab_lanes()),
      dispatcher_(dispatcher),
      transport_(dispatcher_, lab_transport_config, lanes_) {
    if (!transport_.ready()) utils::error{}("NET-LAB-01: transport not ready");
    scratch_.reserve(lab_max_message_bytes);
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
    const auto result = net::gns_listen_on(transport_, host, port);
    if (result.status != net::gns_status::ok)
      utils::error{}("NET-LAB-01: listen on the declared port refused: {}", net::describe(result.status));
    listener_ = result.listener;
    return {result.port, result.ephemeral};
  }

  listen_outcome listen_any(const uint32_t host = 0x7f000001) {
    const auto result = net::gns_listen_any(transport_, host, 41200, 256);
    if (result.status != net::gns_status::ok)
      utils::error{}("NET-LAB-01: no listen port in the declared laboratory range: {}",
                     net::describe(result.status));
    listener_ = result.listener;
    return {result.port, result.ephemeral};
  }

  bool connect_to(const uint16_t port, const uint32_t host = 0x7f000001) {
    auto options = net::gns_unauthenticated_options();
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
    return net::classify_send(transport_.try_send(peer, lane, bytes).status);
  }

  // Consumes at most one native pump's worth of messages. The lease is released
  // as soon as the consumer returns, so no payload outlives this call.
  // How many unreliable frames the backend discarded because a newer one in the
  // same lane had already arrived. On a jittery link this is where a redundant
  // intent copy goes, and without counting it the copy's fate is invisible.
  uint64_t superseded() const noexcept {
    return superseded_;
  }

  template <class Consumer>
  size_t receive(Consumer&& consumer) {
    size_t total = 0;
    for (unsigned round = 0; round < 8; ++round) {
      std::array<net::gns_received_message, 4> input;
      const auto result = transport_.receive(input);
      if (result.status != net::gns_status::ok)
        utils::error{}("NET-LAB-01: receive refused, status {}", unsigned(result.status));
      superseded_ += result.superseded;
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
  net::gns_dispatcher& dispatcher_;
  net::gns_transport transport_;
  net::gns_listener listener_;
  net::gns_peer pending_;
  uint64_t superseded_ = 0;
  std::vector<std::byte> scratch_;
};

} // namespace netlab01

#endif
