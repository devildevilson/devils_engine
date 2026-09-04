#ifndef DEVILS_ENGINE_NETWORK_IN_MEMORY_LINK_H
#define DEVILS_ENGINE_NETWORK_IN_MEMORY_LINK_H

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/utils/core.h>

namespace devils_engine::network {

enum class link_endpoint : std::uint8_t {
  first,
  second
};

constexpr link_endpoint opposite_endpoint(const link_endpoint value) noexcept {
  return value == link_endpoint::first ? link_endpoint::second : link_endpoint::first;
}

enum class link_reliability : std::uint8_t {
  unreliable,
  reliable_ordered
};

struct link_send_options {
  std::uint8_t lane = 0;
  link_reliability reliability = link_reliability::unreliable;

  bool operator==(const link_send_options&) const = default;
};

struct in_memory_link_config {
  std::size_t queue_count_budget = 0;
  std::size_t queue_byte_budget = 0;
  std::size_t bytes_per_step = 0;
  std::uint64_t base_latency_steps = 1;
  std::uint64_t reliable_retry_steps = 1;
};

enum class link_send_status : std::uint8_t {
  accepted,
  disconnected,
  count_budget_exceeded,
  byte_budget_exceeded
};

enum class link_connection_status : std::uint8_t {
  changed,
  already_in_state
};

struct link_transmission {
  std::uint64_t epoch = 0;
  std::uint64_t step = 0;
  std::uint64_t sequence = 0;
  std::uint32_t attempt = 0;
  std::size_t byte_size = 0;
  link_endpoint source = link_endpoint::first;
  link_send_options options;

  bool operator==(const link_transmission&) const = default;
};

struct link_fault_effect {
  bool drop = false;
  std::uint64_t extra_delay_steps = 0;
  std::uint32_t duplicate_count = 0;

  bool operator==(const link_fault_effect&) const = default;
};

struct no_link_faults {
  constexpr link_fault_effect operator()(const link_transmission&) const noexcept {
    return {};
  }
};

enum class link_trace_kind : std::uint8_t {
  connected,
  disconnected,
  accepted,
  rejected,
  transmitted,
  dropped,
  retry_scheduled,
  delivery_scheduled,
  delivered
};

struct link_trace_event {
  std::uint64_t step = 0;
  std::uint64_t epoch = 0;
  std::uint64_t sequence = 0;
  std::uint32_t attempt = 0;
  std::size_t byte_size = 0;
  link_endpoint source = link_endpoint::first;
  link_send_options options;
  link_trace_kind kind = link_trace_kind::accepted;
  link_send_status send_status = link_send_status::accepted;

  bool operator==(const link_trace_event&) const = default;
};

template <class Message>
struct link_received_message {
  std::uint64_t epoch = 0;
  std::uint64_t sequence = 0;
  std::uint64_t delivered_step = 0;
  link_endpoint source = link_endpoint::first;
  link_send_options options;
  Message message;
};

template <class SizeOf, class Message>
concept in_memory_size_policy =
  std::invocable<const SizeOf&, const Message&> &&
  std::same_as<std::invoke_result_t<const SizeOf&, const Message&>, std::size_t>;

template <class FaultPolicy>
concept in_memory_fault_policy =
  std::invocable<FaultPolicy&, const link_transmission&> &&
  std::same_as<std::invoke_result_t<FaultPolicy&, const link_transmission&>, link_fault_effect>;

// A deterministic, single-owner logical transport. Lanes with smaller IDs are
// serviced first. Reliable messages retry after injected loss and are exposed
// exactly once, in lane order; unreliable messages may be lost, duplicated or
// reordered by the injected fault policy. Each direction has its own queue and
// bandwidth budget. Disconnect destroys all queued/in-flight/inbox data, and a
// reconnect starts a new epoch with fresh per-lane sequence numbers.
template <
  std::copy_constructible Message,
  class SizeOf,
  class FaultPolicy = no_link_faults>
  requires in_memory_size_policy<SizeOf, Message> &&
           in_memory_fault_policy<FaultPolicy>
class in_memory_link {
public:
  using message_type = Message;
  using received_type = link_received_message<Message>;

  explicit in_memory_link(
    in_memory_link_config config,
    SizeOf size_of = {},
    FaultPolicy fault_policy = {})
    : config_(config),
      size_of_(std::move(size_of)),
      fault_policy_(std::move(fault_policy)) {
    if (config_.reliable_retry_steps == 0) {
      utils::error{}("network::in_memory_link reliable retry must take at least one step");
    }
  }

  bool connected() const noexcept {
    return connected_;
  }

  std::uint64_t epoch() const noexcept {
    return epoch_;
  }

  std::uint64_t step() const noexcept {
    return step_;
  }

  std::span<const link_trace_event> trace() const noexcept {
    return trace_;
  }

  std::size_t queued_count(const link_endpoint source) const noexcept {
    return directions_[index(source)].queued_count;
  }

  std::size_t queued_bytes(const link_endpoint source) const noexcept {
    return directions_[index(source)].queued_bytes;
  }

  link_connection_status connect() {
    if (connected_) return link_connection_status::already_in_state;
    if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
      utils::error{}("network::in_memory_link epoch space exhausted");
    }

    clear_session_data();
    ++epoch_;
    connected_ = true;
    append_connection_trace(link_trace_kind::connected);
    return link_connection_status::changed;
  }

  link_connection_status disconnect() {
    if (!connected_) return link_connection_status::already_in_state;

    append_connection_trace(link_trace_kind::disconnected);
    connected_ = false;
    clear_session_data();
    return link_connection_status::changed;
  }

  link_send_status try_send(
    const link_endpoint source,
    const link_send_options options,
    const Message& message) {
    return try_send_impl(source, options, message);
  }

  link_send_status try_send(
    const link_endpoint source,
    const link_send_options options,
    Message&& message) {
    return try_send_impl(source, options, std::move(message));
  }

  // Advances deterministic transport time by one step, consumes at most one
  // configured byte budget in each direction, then publishes all deliveries
  // whose delay expired at this step.
  void advance() {
    if (step_ == std::numeric_limits<std::uint64_t>::max()) {
      utils::error{}("network::in_memory_link step space exhausted");
    }
    ++step_;
    if (!connected_) return;

    transmit_direction(link_endpoint::first);
    transmit_direction(link_endpoint::second);
    publish_ready_deliveries();
  }

  std::vector<received_type> drain(const link_endpoint destination) {
    auto& inbox = inboxes_[index(destination)];
    std::vector<received_type> result;
    result.swap(inbox);
    return result;
  }

private:
  struct queued_message {
    Message message;
    std::uint64_t sequence = 0;
    std::uint64_t not_before_step = 0;
    std::size_t byte_size = 0;
    std::size_t remaining_bytes = 0;
    std::uint32_t attempt = 0;
    link_send_options options;
  };

  struct lane_state {
    std::deque<queued_message> queue;
    std::uint64_t next_sequence = 0;
    std::uint64_t last_reliable_delivery_step = 0;
  };

  struct direction_state {
    std::array<lane_state, 256> lanes;
    std::size_t queued_count = 0;
    std::size_t queued_bytes = 0;
  };

  struct scheduled_delivery {
    received_type value;
    std::uint64_t ready_step = 0;
    std::uint64_t insertion_order = 0;
  };

  static constexpr std::size_t index(const link_endpoint value) noexcept {
    return value == link_endpoint::first ? 0 : 1;
  }

  template <class Value>
  link_send_status try_send_impl(
    const link_endpoint source,
    const link_send_options options,
    Value&& message) {
    auto& direction = directions_[index(source)];
    auto& lane = direction.lanes[options.lane];

    if (!connected_) {
      append_send_trace(source, options, lane.next_sequence, 0,
                        link_trace_kind::rejected, link_send_status::disconnected);
      return link_send_status::disconnected;
    }
    const std::size_t byte_size = std::invoke(size_of_, message);
    if (direction.queued_count == config_.queue_count_budget) {
      append_send_trace(source, options, lane.next_sequence, byte_size,
                        link_trace_kind::rejected, link_send_status::count_budget_exceeded);
      return link_send_status::count_budget_exceeded;
    }
    if (byte_size > config_.queue_byte_budget ||
        direction.queued_bytes > config_.queue_byte_budget - byte_size) {
      append_send_trace(source, options, lane.next_sequence, byte_size,
                        link_trace_kind::rejected, link_send_status::byte_budget_exceeded);
      return link_send_status::byte_budget_exceeded;
    }
    if (lane.next_sequence == std::numeric_limits<std::uint64_t>::max()) {
      utils::error{}("network::in_memory_link lane sequence space exhausted");
    }

    const std::uint64_t sequence = lane.next_sequence++;
    lane.queue.push_back({
      std::forward<Value>(message),
      sequence,
      step_,
      byte_size,
      byte_size,
      0,
      options,
    });
    ++direction.queued_count;
    direction.queued_bytes += byte_size;
    append_send_trace(source, options, sequence, byte_size,
                      link_trace_kind::accepted, link_send_status::accepted);
    return link_send_status::accepted;
  }

  void transmit_direction(const link_endpoint source) {
    auto& direction = directions_[index(source)];
    std::size_t available = config_.bytes_per_step;

    while (true) {
      lane_state* selected_lane = nullptr;
      for (auto& lane : direction.lanes) {
        if (!lane.queue.empty() && lane.queue.front().not_before_step <= step_) {
          selected_lane = &lane;
          break;
        }
      }
      if (selected_lane == nullptr) return;

      auto& message = selected_lane->queue.front();
      if (message.remaining_bytes != 0) {
        if (available == 0) return;
        const std::size_t transmitted = std::min(available, message.remaining_bytes);
        available -= transmitted;
        message.remaining_bytes -= transmitted;
        append_message_trace(source, message, link_trace_kind::transmitted,
                             transmitted);
      }

      if (message.remaining_bytes == 0) {
        complete_attempt(source, *selected_lane, direction);
      }
    }
  }

  void complete_attempt(
    const link_endpoint source,
    lane_state& lane,
    direction_state& direction) {
    auto& message = lane.queue.front();
    const link_transmission transmission{
      epoch_,
      step_,
      message.sequence,
      message.attempt,
      message.byte_size,
      source,
      message.options,
    };
    const link_fault_effect effect = std::invoke(fault_policy_, transmission);

    if (effect.drop) {
      append_message_trace(source, message, link_trace_kind::dropped,
                           message.byte_size);
      if (message.options.reliability == link_reliability::reliable_ordered) {
        if (message.attempt == std::numeric_limits<std::uint32_t>::max()) {
          utils::error{}("network::in_memory_link reliable attempt space exhausted");
        }
        ++message.attempt;
        message.remaining_bytes = message.byte_size;
        message.not_before_step = add_steps(config_.reliable_retry_steps);
        append_message_trace(source, message, link_trace_kind::retry_scheduled,
                             message.byte_size);
        return;
      }

      release_front(lane, direction);
      return;
    }

    const std::uint64_t delay = checked_delay(effect.extra_delay_steps);
    std::uint64_t ready_step = add_steps(delay);
    if (message.options.reliability == link_reliability::reliable_ordered) {
      ready_step = std::max(ready_step, lane.last_reliable_delivery_step);
      lane.last_reliable_delivery_step = ready_step;
    }

    const std::uint32_t copy_count =
      message.options.reliability == link_reliability::reliable_ordered
        ? 1
        : effect.duplicate_count + 1;
    if (copy_count == 0) {
      utils::error{}("network::in_memory_link duplicate count overflow");
    }
    for (std::uint32_t i = 0; i < copy_count; ++i) {
      if (next_delivery_order_ == std::numeric_limits<std::uint64_t>::max()) {
        utils::error{}("network::in_memory_link delivery order space exhausted");
      }
      scheduled_.push_back({
        received_type{
          epoch_,
          message.sequence,
          0,
          source,
          message.options,
          message.message,
        },
        ready_step,
        next_delivery_order_++,
      });
      append_message_trace(source, message, link_trace_kind::delivery_scheduled,
                           message.byte_size);
    }
    release_front(lane, direction);
  }

  void release_front(lane_state& lane, direction_state& direction) {
    direction.queued_bytes -= lane.queue.front().byte_size;
    --direction.queued_count;
    lane.queue.pop_front();
  }

  void publish_ready_deliveries() {
    std::stable_sort(
      scheduled_.begin(), scheduled_.end(),
      [](const scheduled_delivery& left, const scheduled_delivery& right) {
        if (left.ready_step != right.ready_step) return left.ready_step < right.ready_step;
        return left.insertion_order < right.insertion_order;
      });

    std::size_t ready_count = 0;
    while (ready_count < scheduled_.size() &&
           scheduled_[ready_count].ready_step <= step_) {
      auto& delivery = scheduled_[ready_count];
      delivery.value.delivered_step = step_;
      append_delivery_trace(delivery.value);
      inboxes_[index(opposite_endpoint(delivery.value.source))].push_back(
        std::move(delivery.value));
      ++ready_count;
    }
    scheduled_.erase(scheduled_.begin(), scheduled_.begin() + std::ptrdiff_t(ready_count));
  }

  std::uint64_t checked_delay(const std::uint64_t extra) const {
    if (extra > std::numeric_limits<std::uint64_t>::max() - config_.base_latency_steps) {
      utils::error{}("network::in_memory_link delivery delay overflow");
    }
    return config_.base_latency_steps + extra;
  }

  std::uint64_t add_steps(const std::uint64_t amount) const {
    if (amount > std::numeric_limits<std::uint64_t>::max() - step_) {
      utils::error{}("network::in_memory_link scheduled step overflow");
    }
    return step_ + amount;
  }

  void clear_session_data() {
    for (auto& direction : directions_) {
      direction.queued_count = 0;
      direction.queued_bytes = 0;
      for (auto& lane : direction.lanes) {
        lane.queue.clear();
        lane.next_sequence = 0;
        lane.last_reliable_delivery_step = 0;
      }
    }
    scheduled_.clear();
    for (auto& inbox : inboxes_)
      inbox.clear();
    next_delivery_order_ = 0;
  }

  void append_connection_trace(const link_trace_kind kind) {
    trace_.push_back({
      step_,
      epoch_,
      0,
      0,
      0,
      link_endpoint::first,
      {},
      kind,
      link_send_status::accepted,
    });
  }

  void append_send_trace(
    const link_endpoint source,
    const link_send_options options,
    const std::uint64_t sequence,
    const std::size_t byte_size,
    const link_trace_kind kind,
    const link_send_status status) {
    trace_.push_back({
      step_,
      epoch_,
      sequence,
      0,
      byte_size,
      source,
      options,
      kind,
      status,
    });
  }

  void append_message_trace(
    const link_endpoint source,
    const queued_message& message,
    const link_trace_kind kind,
    const std::size_t byte_size) {
    trace_.push_back({
      step_,
      epoch_,
      message.sequence,
      message.attempt,
      byte_size,
      source,
      message.options,
      kind,
      link_send_status::accepted,
    });
  }

  void append_delivery_trace(const received_type& value) {
    trace_.push_back({
      step_,
      value.epoch,
      value.sequence,
      0,
      std::invoke(size_of_, value.message),
      value.source,
      value.options,
      link_trace_kind::delivered,
      link_send_status::accepted,
    });
  }

  in_memory_link_config config_;
  [[no_unique_address]] SizeOf size_of_;
  [[no_unique_address]] FaultPolicy fault_policy_;
  std::array<direction_state, 2> directions_;
  std::array<std::vector<received_type>, 2> inboxes_;
  std::vector<scheduled_delivery> scheduled_;
  std::vector<link_trace_event> trace_;
  std::uint64_t step_ = 0;
  std::uint64_t epoch_ = 0;
  std::uint64_t next_delivery_order_ = 0;
  bool connected_ = false;
};

} // namespace devils_engine::network

#endif
