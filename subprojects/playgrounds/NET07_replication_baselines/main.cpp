#include "session.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
using namespace net07_fixture;

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

  network::link_fault_effect operator()(const network::link_transmission& value) const noexcept {
    if (value.source != network::link_endpoint::first) return {};
    const auto it = std::find_if(rules.begin(), rules.end(), [&value](const fault_rule& rule) {
      return rule.lane == value.options.lane && rule.transport_sequence == value.sequence;
    });
    return it == rules.end() ? network::link_fault_effect{} : it->effect;
  }
};

using link_type = network::in_memory_link<message, message_size, scripted_faults>;

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
  std::uint64_t authority_tick = 100;
  std::uint64_t authority_baseline = 100;
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
    link.consume(network::link_endpoint::second, [&](const auto& delivered) {
      replica.receive(delivered.message, link, verify);
    });
    link.consume(network::link_endpoint::first, [&](const auto& delivered) {
      verify.require(delivered.message.kind == message_kind::baseline_request,
                     "authority received a non-request message");
      ++requests;
      auto response = make_full(next_frame_sequence++, authority_tick,
                                authority_baseline, authority);
      response.recovery_token = delivered.message.recovery_token;
      send_from_authority(std::move(response), reliable(baseline_lane));
    });
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
  authority_tick = authority_baseline = 102;
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
  authority_tick = authority_baseline = 104;
  for (std::size_t i = 0; i < 16; ++i)
    pump_once();
  verify.require(replica.current() != nullptr && *replica.current() == state104,
                 "reordered-delta recovery did not converge");

  send_from_authority(
    make_delta_frame(next_frame_sequence++, 105, 104, 105,
                     make_delta(verify, state104, state105)),
    unreliable(delta_lane));
  authority = state105;
  authority_tick = authority_baseline = 105;
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

std::size_t run_recovery_regressions() {
  verifier verify;
  link_type link({8, 4096, 4096, 1, 1, 128});
  link.connect();
  follower replica;
  const snapshot initial{{1, 1, {0, 100}}};
  const snapshot updated{{1, 2, {1, 90}}};
  replica.receive(make_full(1, 100, 100, initial), link, verify);
  replica.receive(make_full(2, 100, 100, initial), link, verify);
  verify.require(replica.applied_full == 2 && replica.rejected == 0,
                 "same immutable baseline under a new sequence was rejected");
  replica.receive(make_full(3, 100, 100, updated), link, verify);
  verify.require(replica.rejected == 1 && *replica.current() == initial && replica.frames.newest() == 2,
                 "conflicting baseline changed state or sequence");

  const auto missing = make_delta_frame(4, 102, 101, 102, {});
  replica.receive(missing, link, verify);
  replica.receive(missing, link, verify);
  replica.receive(make_delta_frame(5, 103, 102, 103, {}), link, verify);
  verify.require(replica.pending_request && replica.coalesced == 2 &&
                   link.queued_count(network::link_endpoint::second) == 1,
                 "missing-base reports were not coalesced");
  const auto token = *replica.pending_request;

  // The authority has advanced beyond the normal forward window. An
  // unsolicited full frame cannot authorize its own sequence-window reset.
  auto recovery = make_full(50, 150, 150, updated);
  replica.receive(recovery, link, verify);
  verify.require(replica.frames.newest() == 2 && *replica.current() == initial,
                 "uncorrelated distant full reset the horizon");
  recovery.recovery_token = token + 1;
  replica.receive(recovery, link, verify);
  verify.require(replica.frames.newest() == 2, "wrong recovery token was accepted");
  recovery.recovery_token = token;
  replica.receive(recovery, link, verify);
  verify.require(!replica.pending_request && replica.frames.newest() == 50 &&
                   replica.current_tick == 150 && *replica.current() == updated,
                 "requested full did not recover across the sequence gap");
  replica.receive(recovery, link, verify);
  verify.require(replica.duplicate == 1, "recovery response was not idempotent");
  replica.receive(make_full(51, 150, 150, updated), link, verify);
  verify.require(replica.frames.newest() == 51 && *replica.current() == updated,
                 "repeated recovery baseline failed under a fresh sequence");

  // Many legal schedules, including a gap which wraps uint16_t but stays below
  // the modular half-range. Payload identity does not depend on request count.
  for (std::uint16_t gap : {std::uint16_t{33}, std::uint16_t{300}, std::uint16_t{30000}}) {
    link.disconnect();
    link.connect();
    follower peer;
    peer.receive(make_full(65000, 1, 1, initial), link, verify);
    const auto sequence = std::uint16_t(65000u + gap);
    peer.receive(make_delta_frame(sequence, 2, 2, 3, {}), link, verify);
    verify.require(peer.pending_request.has_value(), "large gap did not request recovery");
    auto full = make_full(std::uint16_t(sequence + 1), 3, 3, updated);
    full.recovery_token = *peer.pending_request;
    peer.receive(full, link, verify);
    verify.require(!peer.pending_request && *peer.current() == updated,
                   "recovery failed at a wrapped or distant sequence");
  }
  return verify.checks;
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

  session_result result = run_session();
  result.checks += run_recovery_regressions();
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
