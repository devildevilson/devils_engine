#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <devils_engine/network/network.h>
#include <devils_engine/utils/core.h>

namespace {

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
};

struct message_size {
  std::size_t operator()(const message& value) const noexcept {
    return value.wire_bytes;
  }
};

struct fault_rule {
  std::uint8_t lane = 0;
  std::uint64_t transport_sequence = 0;
  network::link_fault_effect effect;
};

struct scripted_faults {
  std::vector<fault_rule> rules;

  network::link_fault_effect operator()(
    const network::link_transmission& value) const noexcept {
    if (value.source != network::link_endpoint::first) return {};
    const auto it = std::find_if(
      rules.begin(), rules.end(),
      [&value](const fault_rule& rule) {
        return rule.lane == value.options.lane &&
               rule.transport_sequence == value.sequence;
      });
    return it == rules.end() ? network::link_fault_effect{} : it->effect;
  }
};

using link_type = network::in_memory_link<message, message_size, scripted_faults>;

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

std::optional<snapshot> apply_project_delta(
  const snapshot& base,
  const delta& changes) {
  auto result = network::apply_keyed_delta(base, changes);
  return std::move(result.snapshot);
}

delta make_delta(verifier& verify, const snapshot& base, const snapshot& current) {
  auto result = network::make_keyed_delta(base, current);
  verify.require(result.succeeded(), "authority produced an invalid delta");
  return std::move(result.delta);
}

message make_full(
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

message make_delta_frame(
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

message make_request(const std::uint64_t missing) {
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

  void receive(const message& value, link_type& link, verifier& verify) {
    verify.require(value.kind != message_kind::baseline_request,
                   "follower received its own baseline request");
    const auto acceptance = frames.classify(
      value.header.format_version, value.header.state_sequence);
    if (acceptance == network::state_frame_acceptance::duplicate) {
      ++duplicate;
      return;
    }
    if (acceptance == network::state_frame_acceptance::stale) {
      ++stale;
      return;
    }
    verify.require(acceptance == network::state_frame_acceptance::accepted,
                   "frame sequence was not acceptable");

    if (value.kind == message_kind::full_baseline) {
      verify.require(!value.header.base_baseline.has_value(),
                     "full baseline unexpectedly names a base");
      const auto stored = baselines.try_store(
        value.header.result_baseline, value.full);
      verify.require(stored.stored(), "full baseline was not stored");
      verify.require(frames.commit(value.header.format_version,
                                   value.header.state_sequence) ==
                       network::state_frame_acceptance::accepted,
                     "full baseline sequence was not committed");
      current_baseline = value.header.result_baseline;
      ++applied_full;
      return;
    }

    verify.require(value.header.base_baseline.has_value(),
                   "delta does not name its exact base");
    const auto materialized = network::try_materialize_delta(
      baselines,
      *value.header.base_baseline,
      value.header.result_baseline,
      value.changes,
      apply_project_delta);
    if (materialized.status == network::delta_materialize_status::missing_baseline) {
      ++missing_base;
      verify.require(
        link.try_send(
          network::link_endpoint::second,
          reliable(request_lane),
          make_request(*value.header.base_baseline)) ==
          network::link_send_status::accepted,
        "baseline request was not accepted by transport");
      return;
    }

    verify.require(materialized.materialized(), "delta was not materialized");
    verify.require(frames.commit(value.header.format_version,
                                 value.header.state_sequence) ==
                     network::state_frame_acceptance::accepted,
                   "delta sequence was not committed");
    current_baseline = value.header.result_baseline;
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

session_result run_session() {
  verifier verify;
  scripted_faults faults{{
    // Unreliable state-lane transport sequence 0 (100 -> 101) is lost.
    {delta_lane, 0, {.drop = true}},
    // Sequence 2 (102 -> 103) arrives after sequence 3 and its recovery.
    {delta_lane, 2, {.extra_delay_steps = 10}},
    // The final delta is visible twice at the application boundary.
    {delta_lane, 4, {.duplicate_count = 1}},
  }};
  link_type link(
    {
      .queue_count_budget = 32,
      .queue_byte_budget = 8192,
      .bytes_per_step = 4096,
      .base_latency_steps = 1,
      .reliable_retry_steps = 2,
    },
    {},
    std::move(faults));
  verify.require(link.connect() == network::link_connection_status::changed,
                 "transport did not connect");

  const snapshot state100 = {
    {1, 1, {0, 100}},
    {2, 1, {10, 80}},
  };
  const snapshot state101 = {
    {1, 2, {1, 95}},
    {2, 1, {10, 80}},
    {3, 1, {-5, 50}},
  };
  const snapshot state102 = {
    {1, 2, {1, 95}},
    {3, 2, {-4, 45}},
  };
  const snapshot state103 = {
    {1, 3, {2, 90}},
    {3, 2, {-4, 45}},
    {4, 1, {20, 100}},
  };
  const snapshot state104 = {
    {1, 3, {2, 90}},
    {4, 2, {19, 70}},
  };
  const snapshot state105 = {
    {1, 4, {3, 88}},
    {4, 2, {19, 70}},
  };

  follower replica;
  snapshot authority = state100;
  std::uint16_t next_frame_sequence = 1;
  std::size_t requests = 0;

  const auto send_from_authority = [&](message value,
                                       const network::link_send_options options) {
    verify.require(
      link.try_send(network::link_endpoint::first, options, std::move(value)) ==
        network::link_send_status::accepted,
      "authority frame was not accepted by transport");
  };

  const auto pump_once = [&]() {
    link.advance();
    for (const auto& delivered : link.drain(network::link_endpoint::second)) {
      replica.receive(delivered.message, link, verify);
    }
    for (const auto& delivered : link.drain(network::link_endpoint::first)) {
      verify.require(delivered.message.kind == message_kind::baseline_request,
                     "authority received a non-request message");
      ++requests;
      send_from_authority(
        make_full(next_frame_sequence++, 100 + requests * 2,
                  requests == 1 ? 102 : 104, authority),
        reliable(baseline_lane));
    }
  };

  send_from_authority(
    make_full(next_frame_sequence++, 100, 100, authority),
    reliable(baseline_lane));
  pump_once();
  pump_once();
  verify.require(replica.current() != nullptr && *replica.current() == state100,
                 "initial replication baseline did not arrive");

  send_from_authority(
    make_delta_frame(next_frame_sequence++, 101, 100, 101,
                     make_delta(verify, state100, state101)),
    unreliable(delta_lane));
  send_from_authority(
    make_delta_frame(next_frame_sequence++, 102, 101, 102,
                     make_delta(verify, state101, state102)),
    unreliable(delta_lane));
  authority = state102;
  for (std::size_t i = 0; i < 6; ++i)
    pump_once();
  verify.require(replica.current() != nullptr && *replica.current() == state102,
                 "first full-baseline recovery did not converge");

  send_from_authority(
    make_delta_frame(next_frame_sequence++, 103, 102, 103,
                     make_delta(verify, state102, state103)),
    unreliable(delta_lane));
  send_from_authority(
    make_delta_frame(next_frame_sequence++, 104, 103, 104,
                     make_delta(verify, state103, state104)),
    unreliable(delta_lane));
  authority = state104;
  for (std::size_t i = 0; i < 16; ++i)
    pump_once();
  verify.require(replica.current() != nullptr && *replica.current() == state104,
                 "reordered-delta recovery did not converge");

  send_from_authority(
    make_delta_frame(next_frame_sequence++, 105, 104, 105,
                     make_delta(verify, state104, state105)),
    unreliable(delta_lane));
  authority = state105;
  for (std::size_t i = 0; i < 4; ++i)
    pump_once();

  verify.require(replica.current() != nullptr, "replica has no current baseline");
  verify.require(*replica.current() == authority,
                 "fake entity sets did not converge");
  verify.require(replica.current_baseline == 105u,
                 "replica selected an unexpected final baseline ID");
  verify.require(replica.applied_full == 3,
                 "expected initial baseline and two recoveries");
  verify.require(replica.applied_delta == 1,
                 "only the final non-lost delta should apply");
  verify.require(replica.missing_base == 2,
                 "missing exact baseline was not reported twice");
  verify.require(replica.duplicate == 1,
                 "duplicated frame was not rejected exactly once");
  verify.require(replica.stale == 1,
                 "delayed obsolete frame was not rejected exactly once");
  verify.require(requests == 2,
                 "recovery did not emit exactly two baseline requests");
  verify.require(replica.baselines.retained_count() <= 4,
                 "baseline count budget was exceeded");
  verify.require(replica.baselines.retained_bytes() <= 4096,
                 "baseline byte budget was exceeded");

  return {
    verify.checks,
    replica.applied_full,
    replica.applied_delta,
    replica.missing_base,
    replica.duplicate,
    replica.stale,
    requests,
    authority,
    *replica.current(),
  };
}

} // namespace

int main(const int argc, const char* const* argv) {
  bool verify_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--verify")
      verify_only = true;
    else
      utils::error{}("NET07: unknown argument '{}'", arg);
  }

  const session_result result = run_session();
  std::cout << "NET07 replication baselines: " << result.checks << '/' << result.checks
            << " checks; full=" << result.full
            << ", delta=" << result.deltas
            << ", missing=" << result.missing
            << ", duplicate=" << result.duplicate
            << ", stale=" << result.stale
            << ", requests=" << result.requests << '\n';
  if (!verify_only) {
    std::cout << "final baseline 105 contains " << result.replica.size()
              << " project-owned entities\n";
  }
  return EXIT_SUCCESS;
}
