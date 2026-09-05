#ifndef DEVILS_ENGINE_NETWORK_GNS_TRANSPORT_H
#define DEVILS_ENGINE_NETWORK_GNS_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace devils_engine::network {

enum class gns_delivery : std::uint8_t { reliable_ordered,
                                         unreliable_sequenced };

struct gns_lane_config {
  gns_delivery delivery = gns_delivery::reliable_ordered;
  int priority = 0;
  std::uint16_t weight = 1;
  std::size_t send_slots = 8;
  std::size_t max_payload_bytes = 1024;
  std::size_t send_byte_budget = 8192;
  bool no_nagle = true;
};

struct gns_transport_config {
  std::size_t peers = 8;
  std::size_t receive_leases = 32;
  std::size_t max_receive_bytes = 512 * 1024;
  // Backend queued data is separate from messages leased to the caller.
  int backend_receive_messages = 128;
  int backend_receive_bytes = 4 * 1024 * 1024;
};

// Generation is unique across transport instances in this process. Neither
// this ID nor a GNS handle is a persistent player/authority identity.
struct gns_peer {
  std::uint32_t slot = UINT32_MAX;
  std::uint64_t generation = 0;
  bool operator==(const gns_peer&) const = default;
};

enum class gns_status : std::uint8_t {
  ok,
  not_ready,
  invalid_peer,
  invalid_lane,
  peer_capacity_exceeded,
  already_owned,
  configuration_failed,
  payload_too_large,
  count_budget_exceeded,
  byte_budget_exceeded,
  backend_rejected,
  output_not_empty,
  invalid_message
};

struct gns_adopt_result {
  gns_status status = gns_status::ok;
  gns_peer peer;
};

struct gns_send_result {
  gns_status status = gns_status::ok;
  // Positive GNS message number on acceptance; negative EResult on backend
  // refusal. Acceptance is not remote delivery or gameplay acknowledgement.
  std::int64_t message_number_or_error = 0;
};

struct gns_send_release {
  gns_peer peer;
  std::uint16_t lane = 0;
  std::uint64_t tag = 0;
  std::int64_t message_number = 0;
  std::size_t bytes = 0;
};

namespace detail {
struct gns_receive_budget;
}

// A bounded lease on a native received message. No payload copy or per-lease
// allocation. Move/release may happen on another thread, but borrowed spans
// expire at reset/destruction. The GNS runtime must outlive every lease.
class gns_received_message {
public:
  gns_received_message() = default;
  ~gns_received_message();
  gns_received_message(gns_received_message&& other) noexcept;
  gns_received_message& operator=(gns_received_message&& other) noexcept;
  gns_received_message(const gns_received_message&) = delete;
  gns_received_message& operator=(const gns_received_message&) = delete;

  explicit operator bool() const noexcept {
    return message_ != nullptr;
  }
  std::span<const std::byte> payload() const noexcept;
  gns_peer peer() const noexcept {
    return peer_;
  }
  std::uint16_t lane() const noexcept;
  std::int64_t message_number() const noexcept;
  void reset() noexcept;

private:
  friend class gns_transport;
  SteamNetworkingMessage_t* message_ = nullptr;
  std::shared_ptr<detail::gns_receive_budget> budget_;
  gns_peer peer_;
};

struct gns_receive_result {
  gns_status status = gns_status::ok;
  std::size_t count = 0;
  std::size_t superseded = 0;
};

struct gns_connection_event {
  gns_peer peer;
  SteamNetConnectionInfo_t info{};
};

// Single owner, no worker, no global Init/Kill or callback installation. The
// caller lends initialized GNS interfaces and owns their runtime lifetime.
// Methods are owner-thread only, except received-message release and native
// send-buffer release. Config/lanes and buffer allocation are preparation.
class gns_transport {
public:
  gns_transport(ISteamNetworkingSockets& sockets, ISteamNetworkingUtils& utils,
                gns_transport_config config, std::span<const gns_lane_config> lanes);
  ~gns_transport();
  gns_transport(const gns_transport&) = delete;
  gns_transport& operator=(const gns_transport&) = delete;
  gns_transport(gns_transport&&) = delete;
  gns_transport& operator=(gns_transport&&) = delete;

  bool ready() const noexcept;
  // Takes exclusive ownership on entry, INCLUDING refusal (closes the handle).
  // An already owned handle is refused without closing it. Only transfer fresh
  // connections before their first application messages/lane configuration.
  [[nodiscard]] gns_adopt_result adopt(HSteamNetConnection connection);
  [[nodiscard]] gns_status close(gns_peer peer);

  // Copies the caller's bytes once into a prepared per-lane slab. GNS then owns
  // that slot until m_pfnFreeData; release order need not be FIFO. Rejected sends
  // never consume caller data. No borrowed caller pointer crosses this method.
  [[nodiscard]] gns_send_result try_send(gns_peer peer, std::uint16_t lane,
                                         std::span<const std::byte> payload,
                                         std::uint64_t tag = 0);
  // Reclaims accepted slots only when the owner observes release. Not an ACK.
  // Unobserved completions backpressure their own lane, not another lane.
  std::size_t poll_send_releases(std::span<gns_send_release> output);
  // All output elements must be empty. At most work_budget native messages
  // are examined, including stale unreliable frames, so a flood cannot monopolize
  // one pump. Outstanding leases are bounded across successive receive calls.
  [[nodiscard]] gns_receive_result receive(std::span<gns_received_message> output,
                                           std::size_t work_budget = 64);
  // Observed state changes, not a lossless native callback log. A small output
  // does not discard unreported current states. Terminal handles stay owned
  // until close(). Intermediate states can coalesce between polls.
  std::size_t poll_connections(std::span<gns_connection_event> output);
  [[nodiscard]] gns_status statistics(gns_peer peer, SteamNetConnectionRealTimeStatus_t& status,
                                      std::span<SteamNetConnectionRealTimeLaneStatus_t> lanes = {});
  std::size_t retained_send_count(std::uint16_t lane) const noexcept;
  std::size_t retained_send_bytes(std::uint16_t lane) const noexcept;
  std::size_t leased_receive_count() const noexcept;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace devils_engine::network

#endif
