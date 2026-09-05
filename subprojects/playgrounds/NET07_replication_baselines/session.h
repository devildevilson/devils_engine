#ifndef DEVILS_ENGINE_NET07_SESSION_H
#define DEVILS_ENGINE_NET07_SESSION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <devils_engine/network/network.h>
#include <devils_engine/utils/core.h>

namespace net07_fixture {

namespace network = devils_engine::network;
namespace utils = devils_engine::utils;

constexpr std::uint32_t frame_format = 1;
constexpr std::uint8_t request_lane = 0;
constexpr std::uint8_t baseline_lane = 1;
constexpr std::uint8_t delta_lane = 2;

struct actor {
  std::int32_t x = 0;
  std::int32_t health = 0;
  bool operator==(const actor&) const = default;
};

using snapshot = network::keyed_snapshot<std::uint32_t, actor, std::uint32_t>;
using delta = network::keyed_delta<std::uint32_t, actor, std::uint32_t>;
using frame_header = network::state_frame_header<
  std::uint64_t, std::uint16_t, std::uint64_t, std::uint64_t>;

enum class message_kind : std::uint8_t {
  full_baseline,
  delta,
  baseline_request
};

struct message {
  message_kind kind = message_kind::baseline_request;
  frame_header header;
  snapshot full;
  delta changes;
  std::uint64_t requested_baseline = 0;
  std::size_t wire_bytes = 0;
  std::uint64_t recovery_token = 0;
};

struct snapshot_size {
  std::size_t operator()(const snapshot& value) const noexcept {
    constexpr std::size_t entry_bytes = 4 + 4 + 4 + 4;
    return 4 + entry_bytes * value.size();
  }
};

using baseline_store = network::baseline_store<std::uint64_t, snapshot, snapshot_size>;

struct verifier {
  std::size_t checks = 0;

  void require(const bool condition, const std::string_view message) {
    if (!condition) utils::error{}("NET07: {}", message);
    ++checks;
  }
};

constexpr network::link_send_options reliable(
  const std::uint8_t lane) noexcept {
  return {lane, network::link_reliability::reliable_ordered};
}

constexpr network::link_send_options unreliable(
  const std::uint8_t lane) noexcept {
  return {lane, network::link_reliability::unreliable};
}

inline std::optional<snapshot> apply_project_delta(
  const snapshot& base,
  const delta& changes) {
  auto result = network::apply_keyed_delta(base, changes);
  return std::move(result.snapshot);
}

inline delta make_delta(verifier& verify, const snapshot& base, const snapshot& current) {
  auto result = network::make_keyed_delta(base, current);
  verify.require(result.succeeded(), "authority produced an invalid delta");
  return std::move(result.delta);
}

inline message make_full(
  const std::uint16_t sequence,
  const std::uint64_t tick,
  const std::uint64_t result_id,
  const snapshot& value) {
  return {
    message_kind::full_baseline,
    {frame_format, tick, sequence, std::nullopt, result_id, tick - 1},
    value,
    {},
    0,
    48 + snapshot_size{}(value),
  };
}

inline message make_delta_frame(
  const std::uint16_t sequence,
  const std::uint64_t tick,
  const std::uint64_t base_id,
  const std::uint64_t result_id,
  delta changes) {
  const std::size_t bytes = 48 + 20 * changes.size();
  return {
    message_kind::delta,
    {frame_format, tick, sequence, base_id, result_id, tick - 1},
    {},
    std::move(changes),
    0,
    bytes,
  };
}

inline message make_request(const std::uint64_t missing) {
  message value;
  value.kind = message_kind::baseline_request;
  value.requested_baseline = missing;
  value.wire_bytes = 24;
  return value;
}

struct follower {
  baseline_store baselines{4, 4096};
  network::state_frame_window<std::uint16_t, 32> frames{frame_format};
  std::optional<std::uint64_t> current_baseline;
  std::size_t applied_full = 0;
  std::size_t applied_delta = 0;
  std::size_t missing_base = 0;
  std::size_t duplicate = 0;
  std::size_t stale = 0;

  std::uint64_t current_tick = 0;
  std::uint64_t next_request = 1;
  std::optional<std::uint64_t> pending_request;
  std::size_t rejected = 0;
  std::size_t coalesced = 0;

  template <class Link>
  void request_recovery(const std::uint64_t missing, Link& link) {
    if (pending_request) {
      ++coalesced;
      return;
    }
    auto request = make_request(missing);
    request.recovery_token = next_request;
    if (link.try_send(network::link_endpoint::second, reliable(request_lane),
                      std::move(request)) == network::link_send_status::accepted) {
      pending_request = next_request++;
    }
  }

  template <class Link>
  void receive(const message& value, Link& link, verifier& verify) {
    // This fixture's transport peer is the trusted authority. In an actual
    // session the authority/epoch check precedes this code. A token correlates
    // recovery, it does not authenticate a packet.
    if (value.kind == message_kind::baseline_request ||
        value.header.format_version != frame_format) {
      ++rejected;
      return;
    }
    const bool recovery = value.kind == message_kind::full_baseline &&
                          pending_request && value.recovery_token == *pending_request;
    const auto acceptance = frames.classify(
      value.header.format_version, value.header.state_sequence);
    if (acceptance == network::state_frame_acceptance::duplicate) {
      ++duplicate;
      return;
    }
    if (acceptance == network::state_frame_acceptance::stale &&
        !(recovery && value.header.server_tick > current_tick)) {
      if (recovery) pending_request.reset();
      ++stale;
      return;
    }
    if (acceptance == network::state_frame_acceptance::too_far_ahead && !recovery) {
      request_recovery(value.header.base_baseline.value_or(value.header.result_baseline), link);
      return;
    }

    if (value.kind == message_kind::full_baseline) {
      if (value.header.base_baseline || value.header.server_tick < current_tick) {
        ++rejected;
        return;
      }
      const auto* existing = baselines.find(value.header.result_baseline);
      if (existing) {
        // Same immutable identity may be repeated under a new frame sequence,
        // but never assigned new content or move the current state backwards.
        if (*existing != value.full || current_baseline != value.header.result_baseline ||
            current_tick != value.header.server_tick) {
          ++rejected;
          return;
        }
      } else {
        if (!baselines.try_store(value.header.result_baseline, value.full).stored()) {
          ++rejected;
          return;
        }
      }
      const auto committed = recovery
                               ? frames.reset(value.header.format_version, value.header.state_sequence)
                               : frames.commit(value.header.format_version, value.header.state_sequence);
      verify.require(committed == network::state_frame_acceptance::accepted,
                     "validated full baseline sequence was not committed");
      current_baseline = value.header.result_baseline;
      current_tick = value.header.server_tick;
      if (recovery) pending_request.reset();
      ++applied_full;
      return;
    }

    if (!value.header.base_baseline || value.header.server_tick < current_tick) {
      ++rejected;
      return;
    }
    const auto materialized = network::try_materialize_delta(
      baselines, *value.header.base_baseline, value.header.result_baseline,
      value.changes, apply_project_delta);
    if (materialized.status == network::delta_materialize_status::missing_baseline) {
      ++missing_base;
      request_recovery(*value.header.base_baseline, link);
      return;
    }
    if (!materialized.materialized()) {
      ++rejected;
      return;
    }
    verify.require(frames.commit(value.header.format_version,
                                 value.header.state_sequence) ==
                     network::state_frame_acceptance::accepted,
                   "delta sequence was not committed");
    current_baseline = value.header.result_baseline;
    current_tick = value.header.server_tick;
    ++applied_delta;
  }

  const snapshot* current() const {
    return current_baseline ? baselines.find(*current_baseline) : nullptr;
  }
};

struct session_result {
  std::size_t checks = 0;
  std::size_t full = 0;
  std::size_t deltas = 0;
  std::size_t missing = 0;
  std::size_t duplicate = 0;
  std::size_t stale = 0;
  std::size_t requests = 0;
  snapshot authority;
  snapshot replica;
};

} // namespace net07_fixture

#endif
