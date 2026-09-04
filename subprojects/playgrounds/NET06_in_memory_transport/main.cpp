#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <devils_engine/network/network.h>
#include <devils_engine/utils/core.h>

namespace {

namespace network = devils_engine::network;
namespace utils = devils_engine::utils;

enum class message_kind : std::uint8_t {
  intent,
  state_frame,
  bulk,
  control
};

struct wire_message {
  message_kind kind = message_kind::control;
  std::uint64_t tick = 0;
  std::uint32_t application_sequence = 0;
  std::int32_t value = 0;
  std::size_t wire_bytes = 0;
};

struct wire_size {
  std::size_t operator()(const wire_message& value) const noexcept {
    return value.wire_bytes;
  }
};

struct fault_rule {
  network::link_endpoint source = network::link_endpoint::first;
  std::uint8_t lane = 0;
  std::uint64_t sequence = 0;
  std::uint32_t attempt = 0;
  network::link_fault_effect effect;
};

struct scripted_faults {
  std::vector<fault_rule> rules;

  network::link_fault_effect operator()(const network::link_transmission& value) const noexcept {
    const auto it = std::find_if(
      rules.begin(), rules.end(),
      [&value](const fault_rule& rule) {
        return rule.source == value.source && rule.lane == value.options.lane &&
               rule.sequence == value.sequence && rule.attempt == value.attempt;
      });
    return it == rules.end() ? network::link_fault_effect{} : it->effect;
  }
};

using scripted_link = network::in_memory_link<wire_message, wire_size, scripted_faults>;
using plain_link = network::in_memory_link<wire_message, wire_size>;

struct verifier {
  std::size_t checks = 0;

  void require(const bool condition, const std::string_view message) {
    if (!condition) utils::error{}("NET06: {}", message);
    ++checks;
  }
};

constexpr network::link_send_options reliable(const std::uint8_t lane) noexcept {
  return {lane, network::link_reliability::reliable_ordered};
}

constexpr network::link_send_options unreliable(const std::uint8_t lane) noexcept {
  return {lane, network::link_reliability::unreliable};
}

std::size_t trace_count(
  const std::span<const network::link_trace_event> trace,
  const network::link_trace_kind kind) {
  return std::size_t(std::count_if(
    trace.begin(), trace.end(),
    [kind](const network::link_trace_event& event) {
      return event.kind == kind;
    }));
}

struct mixed_result {
  std::vector<network::link_trace_event> trace;
  std::int32_t authority_state = 0;
  std::int32_t follower_state = 0;
  std::size_t accepted_latest_frames = 0;
  std::size_t duplicate_frames = 0;
  std::size_t obsolete_frames = 0;
};

mixed_result run_mixed_session(verifier& verify) {
  constexpr std::uint8_t intent_lane = 0;
  constexpr std::uint8_t state_lane = 1;
  scripted_faults faults{{
    // The third reliable intent loses its first transmission and retries.
    {network::link_endpoint::first, intent_lane, 2, 0, {.drop = true}},
    // Frame zero is delayed past frames one and two.
    {network::link_endpoint::first, state_lane, 0, 0, {.extra_delay_steps = 5}},
    // Frame two appears twice at the application boundary.
    {network::link_endpoint::first, state_lane, 2, 0, {.duplicate_count = 1}},
    // Frame three is permanently lost; frame four later supersedes it.
    {network::link_endpoint::first, state_lane, 3, 0, {.drop = true}},
  }};
  scripted_link link(
    {
      .queue_count_budget = 128,
      .queue_byte_budget = 8192,
      .bytes_per_step = 96,
      .base_latency_steps = 1,
      .reliable_retry_steps = 2,
    },
    {}, std::move(faults));

  verify.require(link.connect() == network::link_connection_status::changed,
                 "mixed session did not connect");

  std::array<std::int32_t, 11> authoritative_history{};
  std::array<std::int32_t, 11> follower_history{};
  std::vector<wire_message> newest_frames;
  network::sequence_window<std::uint16_t, 32> frame_sequences;
  std::uint64_t next_intent_tick = 1;
  std::int32_t authority_state = 0;
  std::int32_t follower_state = 0;
  std::uint32_t frame_sequence = 0;
  std::size_t duplicate_frames = 0;
  std::size_t obsolete_frames = 0;

  const auto consume = [&](std::vector<scripted_link::received_type> delivered) {
    for (const auto& entry : delivered) {
      const wire_message& message = entry.message;
      if (message.kind == message_kind::intent) {
        verify.require(message.tick == next_intent_tick,
                       "reliable intent lane was delivered out of tick order");
        follower_state += message.value;
        follower_history[message.tick] = follower_state;
        ++next_intent_tick;
        continue;
      }

      verify.require(message.kind == message_kind::state_frame,
                     "mixed session received an unexpected message kind");
      const std::uint16_t sequence = std::uint16_t(message.application_sequence);
      const auto classification = frame_sequences.observe(sequence);
      if (classification == network::sequence_classification::duplicate) {
        ++duplicate_frames;
      } else if (frame_sequences.newest() == sequence) {
        newest_frames.push_back(message);
      } else {
        ++obsolete_frames;
      }
    }
  };

  for (std::uint64_t tick = 1; tick <= 10; ++tick) {
    const std::int32_t delta = std::int32_t((tick * 7) % 5 + 1);
    authority_state += delta;
    authoritative_history[tick] = authority_state;

    verify.require(
      link.try_send(
        network::link_endpoint::first, reliable(intent_lane),
        {message_kind::intent, tick, 0, delta, 24}) == network::link_send_status::accepted,
      "intent submission failed");

    if ((tick % 2) == 0) {
      verify.require(
        link.try_send(
          network::link_endpoint::first, unreliable(state_lane),
          {message_kind::state_frame, tick, frame_sequence++, authority_state, 32}) ==
          network::link_send_status::accepted,
        "state-frame submission failed");
    }

    link.advance();
    consume(link.drain(network::link_endpoint::second));
  }

  for (std::size_t i = 0; i < 20; ++i) {
    link.advance();
    consume(link.drain(network::link_endpoint::second));
  }

  verify.require(next_intent_tick == 11, "not every reliable intent was delivered");
  verify.require(follower_state == authority_state,
                 "independent simulation hosts did not converge through intents");
  verify.require(link.queued_count(network::link_endpoint::first) == 0,
                 "mixed session retained an outbound message");
  verify.require(duplicate_frames == 1, "unreliable duplicate was not observed exactly once");
  verify.require(obsolete_frames == 1, "reordered obsolete frame was not identified");
  verify.require(newest_frames.size() == 3,
                 "unexpected number of advancing unreliable state frames");

  for (const wire_message& frame : newest_frames) {
    verify.require(authoritative_history[frame.tick] == frame.value,
                   "state frame does not describe authority history");
    verify.require(follower_history[frame.tick] == frame.value,
                   "state frame does not match replayed follower history");
  }

  verify.require(trace_count(link.trace(), network::link_trace_kind::dropped) == 2,
                 "fault schedule did not produce both declared losses");
  verify.require(trace_count(link.trace(), network::link_trace_kind::retry_scheduled) == 1,
                 "reliable loss did not produce exactly one retry");

  return {
    std::vector<network::link_trace_event>{link.trace().begin(), link.trace().end()},
    authority_state,
    follower_state,
    newest_frames.size(),
    duplicate_frames,
    obsolete_frames,
  };
}

void verify_lane_priority_and_reliable_order(verifier& verify) {
  scripted_faults faults{{
    // Duplicate injection is hidden for reliable delivery; only the delay is observable.
    {network::link_endpoint::first, 3, 0, 0, {.extra_delay_steps = 5, .duplicate_count = 2}},
  }};
  scripted_link link(
    {
      .queue_count_budget = 16,
      .queue_byte_budget = 2048,
      .bytes_per_step = 64,
      .base_latency_steps = 0,
      .reliable_retry_steps = 1,
    },
    {}, std::move(faults));
  static_cast<void>(link.connect());

  verify.require(
    link.try_send(network::link_endpoint::first, reliable(5),
                  {message_kind::bulk, 0, 0, 100, 256}) == network::link_send_status::accepted,
    "bulk submission failed");
  link.advance();
  verify.require(link.drain(network::link_endpoint::second).empty(),
                 "partially transmitted bulk was delivered");

  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::control, 0, 0, 200, 16}) == network::link_send_status::accepted,
    "priority control submission failed");
  link.advance();
  auto delivered = link.drain(network::link_endpoint::second);
  verify.require(delivered.size() == 1 && delivered.front().message.value == 200,
                 "small high-priority lane did not overtake bulk transmission");

  for (std::size_t i = 0; i < 4; ++i)
    link.advance();
  delivered = link.drain(network::link_endpoint::second);
  verify.require(delivered.size() == 1 && delivered.front().message.kind == message_kind::bulk,
                 "bulk did not complete after its bandwidth budget");

  verify.require(
    link.try_send(network::link_endpoint::first, reliable(3),
                  {message_kind::control, 0, 0, 300, 8}) == network::link_send_status::accepted,
    "first ordered message submission failed");
  verify.require(
    link.try_send(network::link_endpoint::first, reliable(3),
                  {message_kind::control, 0, 0, 301, 8}) == network::link_send_status::accepted,
    "second ordered message submission failed");
  for (std::size_t i = 0; i < 6; ++i)
    link.advance();
  delivered = link.drain(network::link_endpoint::second);
  verify.require(
    delivered.size() == 2 && delivered[0].message.value == 300 &&
      delivered[1].message.value == 301,
    "reliable lane did not preserve delivery order under unequal delay");
}

void verify_backpressure(verifier& verify) {
  plain_link link({
    .queue_count_budget = 2,
    .queue_byte_budget = 100,
    .bytes_per_step = 0,
    .base_latency_steps = 1,
    .reliable_retry_steps = 1,
  });

  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::control, 0, 0, 0, 1}) == network::link_send_status::disconnected,
    "disconnected send did not return an explicit status");
  static_cast<void>(link.connect());
  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::control, 0, 0, 1, 60}) == network::link_send_status::accepted,
    "first bounded send failed");
  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::control, 0, 0, 2, 50}) ==
      network::link_send_status::byte_budget_exceeded,
    "byte budget did not reject without mutation");
  verify.require(link.queued_count(network::link_endpoint::first) == 1 &&
                   link.queued_bytes(network::link_endpoint::first) == 60,
                 "byte-budget refusal mutated the queue");
  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::control, 0, 0, 3, 40}) == network::link_send_status::accepted,
    "exact byte budget was rejected");
  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::control, 0, 0, 4, 0}) ==
      network::link_send_status::count_budget_exceeded,
    "count budget did not reject an otherwise empty message");
  verify.require(link.queued_count(network::link_endpoint::first) == 2 &&
                   link.queued_bytes(network::link_endpoint::first) == 100,
                 "count-budget refusal mutated the queue");
  verify.require(
    link.try_send(network::link_endpoint::second, reliable(0),
                  {message_kind::control, 0, 0, 5, 100}) == network::link_send_status::accepted &&
      link.queued_count(network::link_endpoint::second) == 1 &&
      link.queued_bytes(network::link_endpoint::second) == 100,
    "one direction consumed the other direction's queue budget");
}

void verify_disconnect_and_reconnect(verifier& verify) {
  plain_link link({
    .queue_count_budget = 8,
    .queue_byte_budget = 1024,
    .bytes_per_step = 64,
    .base_latency_steps = 5,
    .reliable_retry_steps = 1,
  });
  static_cast<void>(link.connect());
  verify.require(link.epoch() == 1, "first connection did not create epoch one");
  verify.require(link.connect() == network::link_connection_status::already_in_state,
                 "duplicate connect did not return an explicit status");
  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::control, 0, 0, 400, 8}) == network::link_send_status::accepted,
    "old-epoch message submission failed");
  verify.require(
    link.try_send(network::link_endpoint::first, reliable(0),
                  {message_kind::bulk, 0, 0, 402, 128}) == network::link_send_status::accepted,
    "old-epoch queued message submission failed");
  link.advance();
  verify.require(link.queued_count(network::link_endpoint::first) == 1,
                 "old epoch did not retain the deliberately partial message");
  verify.require(link.disconnect() == network::link_connection_status::changed,
                 "disconnect did not change state");
  verify.require(link.queued_count(network::link_endpoint::first) == 0,
                 "disconnect retained queued data");
  verify.require(link.disconnect() == network::link_connection_status::already_in_state,
                 "duplicate disconnect did not return an explicit status");
  verify.require(link.connect() == network::link_connection_status::changed,
                 "reconnect did not change state");
  verify.require(link.epoch() == 2, "reconnect did not create a fresh epoch");
  verify.require(link.connect() == network::link_connection_status::already_in_state,
                 "duplicate reconnect did not return an explicit status");

  verify.require(
    link.try_send(network::link_endpoint::second, reliable(0),
                  {message_kind::control, 0, 0, 401, 8}) == network::link_send_status::accepted,
    "new-epoch reverse-direction submission failed");
  for (std::size_t i = 0; i < 6; ++i)
    link.advance();
  const auto delivered = link.drain(network::link_endpoint::first);
  verify.require(
    delivered.size() == 1 && delivered.front().epoch == 2 &&
      delivered.front().message.value == 401,
    "reconnect leaked stale delivery or lost fresh reverse-direction data");
}

std::string_view trace_kind_name(const network::link_trace_kind kind) {
  switch (kind) {
    case network::link_trace_kind::connected: return "connect";
    case network::link_trace_kind::disconnected: return "disconnect";
    case network::link_trace_kind::accepted: return "accept";
    case network::link_trace_kind::rejected: return "reject";
    case network::link_trace_kind::transmitted: return "transmit";
    case network::link_trace_kind::dropped: return "drop";
    case network::link_trace_kind::retry_scheduled: return "retry";
    case network::link_trace_kind::delivery_scheduled: return "schedule";
    case network::link_trace_kind::delivered: return "deliver";
  }
  return "unknown";
}

void print_trace(const std::span<const network::link_trace_event> trace) {
  std::cout << "transport trace:\n";
  for (const auto& event : trace) {
    std::cout << "  step=" << event.step << " epoch=" << event.epoch
              << " source=" << (event.source == network::link_endpoint::first ? 'A' : 'B')
              << " lane=" << unsigned(event.options.lane)
              << " seq=" << event.sequence << " attempt=" << event.attempt
              << " bytes=" << event.byte_size << ' ' << trace_kind_name(event.kind) << '\n';
  }
}

} // namespace

int main(const int argc, const char** argv) {
  bool print_packet_trace = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "--verify") {
      continue;
    }
    if (argument == "--trace") {
      print_packet_trace = true;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      std::cout << "NET06 deterministic in-memory transport\n"
                   "  --verify  run the complete contract campaign\n"
                   "  --trace   print the mixed-session transport trace\n";
      return EXIT_SUCCESS;
    }
    utils::error{}("NET06: unknown argument '{}'", argument);
  }

  verifier verify;
  const mixed_result first = run_mixed_session(verify);
  const mixed_result repeated = run_mixed_session(verify);
  verify.require(first.trace == repeated.trace,
                 "the same fault schedule produced a different trace");
  verify.require(first.authority_state == repeated.authority_state &&
                   first.follower_state == repeated.follower_state &&
                   first.accepted_latest_frames == repeated.accepted_latest_frames &&
                   first.duplicate_frames == repeated.duplicate_frames &&
                   first.obsolete_frames == repeated.obsolete_frames,
                 "the same fault schedule produced a different session result");

  verify_lane_priority_and_reliable_order(verify);
  verify_backpressure(verify);
  verify_disconnect_and_reconnect(verify);

  std::cout << "NET06 deterministic in-memory transport\n"
            << "  checks: " << verify.checks << '/' << verify.checks << '\n'
            << "  mixed trace events: " << first.trace.size() << '\n'
            << "  final authority/follower state: " << first.authority_state << '/'
            << first.follower_state << '\n'
            << "  advancing/duplicate/obsolete state frames: "
            << first.accepted_latest_frames << '/' << first.duplicate_frames << '/'
            << first.obsolete_frames << '\n'
            << "  replayed schedule: bit-identical trace and result\n";

  if (print_packet_trace) print_trace(first.trace);
  return EXIT_SUCCESS;
}
