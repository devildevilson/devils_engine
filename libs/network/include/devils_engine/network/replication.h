#ifndef DEVILS_ENGINE_NETWORK_REPLICATION_H
#define DEVILS_ENGINE_NETWORK_REPLICATION_H

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "bounded_history.h"

namespace devils_engine::network {

// This is application metadata, not a wire-format declaration. A codec owns
// integer widths, endianness and validation of its serialized representation.
// A missing base_baseline identifies a full replication baseline; a present
// one identifies the exact state from which a delta was produced.
template <class Tick, std::unsigned_integral Sequence, class BaselineId,
          class InputSequence>
struct state_frame_header {
  std::uint32_t format_version = 0;
  Tick server_tick{};
  Sequence state_sequence{};
  std::optional<BaselineId> base_baseline;
  BaselineId result_baseline{};
  InputSequence acknowledged_input_sequence{};

  bool operator==(const state_frame_header&) const = default;
};

enum class state_frame_acceptance : unsigned char {
  accepted,
  duplicate,
  stale,
  too_far_ahead,
  format_version_mismatch
};

// State frames are latest-state observations, unlike reliable commands. A
// previously unseen older sequence is therefore stale rather than useful.
// classify() never mutates: a receiver commits only after decoding and
// materializing the frame succeeds. An authenticated full-baseline recovery
// may establish a distant sequence explicitly with reset().
template <std::unsigned_integral Sequence, std::size_t MaxForwardAdvance>
class state_frame_window {
public:
  static_assert(MaxForwardAdvance > 0,
                "network::state_frame_window requires a non-empty forward window");
  static constexpr Sequence half_range =
    Sequence(std::numeric_limits<Sequence>::max() / Sequence{2} + Sequence{1});
  static_assert(MaxForwardAdvance < half_range,
                "network::state_frame_window must be smaller than half the sequence space");

  constexpr explicit state_frame_window(const std::uint32_t format_version) noexcept
    : format_version_(format_version) {}

  constexpr std::uint32_t format_version() const noexcept {
    return format_version_;
  }

  constexpr bool initialized() const noexcept {
    return newest_.has_value();
  }

  constexpr std::optional<Sequence> newest() const noexcept {
    return newest_;
  }

  constexpr state_frame_acceptance classify(
    const std::uint32_t format_version,
    const Sequence sequence) const noexcept {
    if (format_version != format_version_) {
      return state_frame_acceptance::format_version_mismatch;
    }
    if (!newest_) return state_frame_acceptance::accepted;

    const Sequence forward = Sequence(sequence - *newest_);
    if (forward == 0) return state_frame_acceptance::duplicate;
    if (forward == half_range) return state_frame_acceptance::too_far_ahead;
    if (forward < half_range) {
      return forward <= Sequence(MaxForwardAdvance)
               ? state_frame_acceptance::accepted
               : state_frame_acceptance::too_far_ahead;
    }
    return state_frame_acceptance::stale;
  }

  constexpr state_frame_acceptance commit(
    const std::uint32_t format_version,
    const Sequence sequence) noexcept {
    const state_frame_acceptance result = classify(format_version, sequence);
    if (result == state_frame_acceptance::accepted) newest_ = sequence;
    return result;
  }

  constexpr void reset() noexcept {
    newest_.reset();
  }

  constexpr state_frame_acceptance reset(
    const std::uint32_t format_version,
    const Sequence accepted) noexcept {
    if (format_version != format_version_) {
      return state_frame_acceptance::format_version_mismatch;
    }
    newest_ = accepted;
    return state_frame_acceptance::accepted;
  }

private:
  std::uint32_t format_version_ = 0;
  std::optional<Sequence> newest_;
};

// Immutable snapshots retained under explicit, monotonically increasing IDs.
// SizeOf names the owner's serialized/logical byte cost; sizeof(Snapshot) is
// usually unrelated to the budget that matters on the network.
enum class baseline_store_status : unsigned char {
  stored,
  duplicate_id,
  out_of_order,
  budget_exceeded
};

struct baseline_store_result {
  baseline_store_status status = baseline_store_status::stored;
  std::size_t evicted_count = 0;
  std::size_t evicted_bytes = 0;

  constexpr bool stored() const noexcept {
    return status == baseline_store_status::stored;
  }
};

template <std::totally_ordered BaselineId, class Snapshot, class SizeOf>
  requires std::invocable<const SizeOf&, const Snapshot&> &&
           std::convertible_to<std::invoke_result_t<const SizeOf&, const Snapshot&>,
                               std::size_t>
class baseline_store {
public:
  using id_type = BaselineId;
  using snapshot_type = Snapshot;
  using size_policy_type = SizeOf;
  using history_type = bounded_history<BaselineId, Snapshot>;
  using entry = typename history_type::entry;

  baseline_store(const std::size_t count_budget,
                 const std::size_t byte_budget,
                 SizeOf size_of = {})
    : values_(count_budget, byte_budget), size_of_(std::move(size_of)) {}

  [[nodiscard]] baseline_store_result try_store(
    const BaselineId& id,
    const Snapshot& snapshot) {
    return convert(values_.try_store(id, snapshot, byte_size(snapshot)));
  }

  [[nodiscard]] baseline_store_result try_store(
    const BaselineId& id,
    Snapshot&& snapshot) {
    const std::size_t bytes = byte_size(snapshot);
    return convert(values_.try_store(id, std::move(snapshot), bytes));
  }

  const Snapshot* find(const BaselineId& id) const {
    return values_.find(id);
  }

  const entry* find_entry(const BaselineId& id) const {
    return values_.find_entry(id);
  }

  auto entries() const noexcept {
    return values_.entries();
  }

  std::size_t count_budget() const noexcept {
    return values_.count_budget();
  }
  std::size_t byte_budget() const noexcept {
    return values_.byte_budget();
  }
  std::size_t retained_count() const noexcept {
    return values_.retained_count();
  }
  std::size_t retained_bytes() const noexcept {
    return values_.retained_bytes();
  }
  bool empty() const noexcept {
    return values_.empty();
  }
  std::optional<BaselineId> oldest_id() const {
    return values_.oldest_tick();
  }
  std::optional<BaselineId> newest_id() const {
    return values_.newest_tick();
  }

  void clear() noexcept {
    values_.clear();
  }

  std::optional<Snapshot> take_oldest() {
    return values_.take_oldest();
  }

private:
  static baseline_store_result convert(const history_store_result result) {
    switch (result.status) {
      case history_store_status::stored:
        return {baseline_store_status::stored,
                result.evicted_count, result.evicted_bytes};
      case history_store_status::duplicate_tick:
        return {baseline_store_status::duplicate_id, 0, 0};
      case history_store_status::out_of_order:
        return {baseline_store_status::out_of_order, 0, 0};
      case history_store_status::budget_exceeded:
        return {baseline_store_status::budget_exceeded, 0, 0};
    }
    return {baseline_store_status::budget_exceeded, 0, 0};
  }

  std::size_t byte_size(const Snapshot& snapshot) const {
    return static_cast<std::size_t>(std::invoke(size_of_, snapshot));
  }

  history_type values_;
  [[no_unique_address]] SizeOf size_of_;
};

enum class delta_materialize_status : unsigned char {
  materialized,
  missing_baseline,
  delta_rejected,
  duplicate_result,
  out_of_order_result,
  budget_exceeded
};

struct delta_materialize_result {
  delta_materialize_status status = delta_materialize_status::materialized;
  std::size_t evicted_count = 0;
  std::size_t evicted_bytes = 0;

  constexpr bool materialized() const noexcept {
    return status == delta_materialize_status::materialized;
  }
};

// The project codec receives an immutable base and must return a complete
// candidate or nullopt. The candidate is published only if the baseline store
// accepts its explicit result ID.
template <class Store, class Delta, class ApplyDelta>
  requires requires(Store& store, const typename Store::id_type& id,
                    const typename Store::snapshot_type& base,
                    const Delta& delta, ApplyDelta& apply) {
    { store.find(id) } -> std::same_as<const typename Store::snapshot_type*>;
    { std::invoke(apply, base, delta) }
    -> std::same_as<std::optional<typename Store::snapshot_type>>;
  }
[[nodiscard]] delta_materialize_result try_materialize_delta(
  Store& store,
  const typename Store::id_type& base_id,
  const typename Store::id_type& result_id,
  const Delta& delta,
  ApplyDelta apply) {
  const typename Store::snapshot_type* base = store.find(base_id);
  if (base == nullptr) {
    return {delta_materialize_status::missing_baseline, 0, 0};
  }

  std::optional<typename Store::snapshot_type> candidate =
    std::invoke(apply, *base, delta);
  if (!candidate) {
    return {delta_materialize_status::delta_rejected, 0, 0};
  }

  const baseline_store_result stored =
    store.try_store(result_id, std::move(*candidate));
  switch (stored.status) {
    case baseline_store_status::stored:
      return {delta_materialize_status::materialized,
              stored.evicted_count, stored.evicted_bytes};
    case baseline_store_status::duplicate_id:
      return {delta_materialize_status::duplicate_result, 0, 0};
    case baseline_store_status::out_of_order:
      return {delta_materialize_status::out_of_order_result, 0, 0};
    case baseline_store_status::budget_exceeded:
      return {delta_materialize_status::budget_exceeded, 0, 0};
  }
  return {delta_materialize_status::delta_rejected, 0, 0};
}

template <class Value, class Version>
struct versioned_value {
  Version version{};
  Value value;

  bool operator==(const versioned_value&) const = default;
};

template <class Key, class Value, class Version>
struct keyed_state_entry {
  Key key;
  Version version{};
  Value value;

  bool operator==(const keyed_state_entry&) const = default;
};

// expected_version == nullopt means "this key must not exist" (create).
// result == nullopt means erase. Both absent is invalid. This representation
// makes create/update/delete preconditions explicit without knowing entities.
template <class Key, class Value, class Version>
struct keyed_delta_entry {
  Key key;
  std::optional<Version> expected_version;
  std::optional<versioned_value<Value, Version>> result;

  bool operator==(const keyed_delta_entry&) const = default;
};

template <class Key, class Value, class Version>
using keyed_snapshot = std::vector<keyed_state_entry<Key, Value, Version>>;

template <class Key, class Value, class Version>
using keyed_delta = std::vector<keyed_delta_entry<Key, Value, Version>>;

enum class keyed_delta_status : unsigned char {
  success,
  base_not_canonical,
  current_not_canonical,
  delta_not_canonical,
  invalid_change,
  version_not_advanced,
  precondition_failed,
  capacity_exceeded,
  aliased_output
};

template <class Key, class Value, class Version>
struct keyed_delta_build_result {
  keyed_delta_status status = keyed_delta_status::success;
  keyed_delta<Key, Value, Version> delta;

  constexpr bool succeeded() const noexcept {
    return status == keyed_delta_status::success;
  }
};

template <class Key, class Value, class Version>
struct keyed_delta_apply_result {
  keyed_delta_status status = keyed_delta_status::success;
  std::optional<keyed_snapshot<Key, Value, Version>> snapshot;

  constexpr bool succeeded() const noexcept {
    return status == keyed_delta_status::success;
  }
};

namespace detail {

template <class Range, class KeyLess>
bool has_strictly_ordered_keys(const Range& values, KeyLess& less) {
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (!std::invoke(less, values[i - 1].key, values[i].key)) return false;
  }
  return true;
}

template <class Key, class KeyLess>
bool same_key(const Key& first, const Key& second, KeyLess& less) {
  return !std::invoke(less, first, second) && !std::invoke(less, second, first);
}

template <class Key, class Value, class Version, class KeyLess, class ValueEqual, class Emit>
keyed_delta_status visit_keyed_delta(
  const keyed_snapshot<Key, Value, Version>& base,
  const keyed_snapshot<Key, Value, Version>& current,
  KeyLess& less, ValueEqual& equal, Emit emit) {
  if (!has_strictly_ordered_keys(base, less)) return keyed_delta_status::base_not_canonical;
  if (!has_strictly_ordered_keys(current, less)) return keyed_delta_status::current_not_canonical;
  std::size_t b = 0, c = 0;
  while (b < base.size() || c < current.size()) {
    if (b == base.size() || (c < current.size() && std::invoke(less, current[c].key, base[b].key))) {
      const auto& value = current[c++];
      emit(value.key, std::optional<Version>{}, &value);
    } else if (c == current.size() || std::invoke(less, base[b].key, current[c].key)) {
      const auto& value = base[b++];
      emit(value.key, std::optional<Version>{value.version},
           static_cast<const keyed_state_entry<Key, Value, Version>*>(nullptr));
    } else {
      const auto& before = base[b++];
      const auto& after = current[c++];
      if (before.version == after.version) {
        if (!std::invoke(equal, before.value, after.value)) return keyed_delta_status::version_not_advanced;
      } else {
        emit(after.key, std::optional<Version>{before.version}, &after);
      }
    }
  }
  return keyed_delta_status::success;
}

// One merge, never vector insert/erase per change. Emit sees references, so the
// validation/counting pass does not copy project payloads.
template <class Key, class Value, class Version, class KeyLess, class ValueEqual, class Emit>
keyed_delta_status visit_keyed_apply(
  const keyed_snapshot<Key, Value, Version>& base,
  const keyed_delta<Key, Value, Version>& delta,
  KeyLess& less, ValueEqual& equal, Emit emit) {
  if (!has_strictly_ordered_keys(base, less)) return keyed_delta_status::base_not_canonical;
  if (!has_strictly_ordered_keys(delta, less)) return keyed_delta_status::delta_not_canonical;
  std::size_t b = 0;
  for (const auto& change : delta) {
    while (b < base.size() && std::invoke(less, base[b].key, change.key)) {
      const auto& value = base[b++];
      emit(value.key, value.version, value.value);
    }
    const bool exists = b < base.size() && same_key(base[b].key, change.key, less);
    if (!change.expected_version) {
      if (!change.result) return keyed_delta_status::invalid_change;
      if (exists) return keyed_delta_status::precondition_failed;
    } else {
      if (!exists || !(base[b].version == *change.expected_version))
        return keyed_delta_status::precondition_failed;
      if (change.result && change.result->version == *change.expected_version) {
        return std::invoke(equal, base[b].value, change.result->value)
                 ? keyed_delta_status::invalid_change
                 : keyed_delta_status::version_not_advanced;
      }
      ++b;
    }
    if (change.result) emit(change.key, change.result->version, change.result->value);
  }
  while (b < base.size()) {
    const auto& value = base[b++];
    emit(value.key, value.version, value.value);
  }
  return keyed_delta_status::success;
}

} // namespace detail

// Policies must be pure and agree with canonical serialization: equal values
// must serialize identically (or be normalized before this codec). In particular
// numerical float equality is not bit equality; compare float fields explicitly.
// These prepared-output overloads never grow vector capacity. Rejections leave
// output unchanged; project value assignment itself may allocate. Inputs/output
// must not alias. Reserve (and prepare nested payloads) outside the hot loop.
template <class Key, class Value, class Version,
          class KeyLess = std::less<Key>, class ValueEqual = std::equal_to<Value>>
[[nodiscard]] keyed_delta_status make_keyed_delta_into(
  const keyed_snapshot<Key, Value, Version>& base,
  const keyed_snapshot<Key, Value, Version>& current,
  keyed_delta<Key, Value, Version>& output,
  KeyLess less = {}, ValueEqual equal = {}) {
  std::size_t count = 0;
  const auto status = detail::visit_keyed_delta(base, current, less, equal,
                                                [&](const auto&, const auto&, const auto*) {
                                                  ++count;
                                                });
  if (status != keyed_delta_status::success) return status;
  if (count > output.capacity()) return keyed_delta_status::capacity_exceeded;
  std::size_t index = 0;
  detail::visit_keyed_delta(base, current, less, equal,
                            [&](const Key& key, std::optional<Version> expected, const auto* value) {
                              if (index == output.size()) output.push_back({key, expected, std::nullopt});
                              auto& target = output[index++];
                              target.key = key;
                              target.expected_version = expected;
                              if (value) {
                                if (target.result) {
                                  target.result->version = value->version;
                                  target.result->value = value->value;
                                } else {
                                  target.result.emplace(versioned_value<Value, Version>{value->version, value->value});
                                }
                              } else
                                target.result.reset();
                            });
  output.erase(output.begin() + std::ptrdiff_t(count), output.end());
  return keyed_delta_status::success;
}

template <class Key, class Value, class Version,
          class KeyLess = std::less<Key>, class ValueEqual = std::equal_to<Value>>
[[nodiscard]] keyed_delta_status apply_keyed_delta_into(
  const keyed_snapshot<Key, Value, Version>& base,
  const keyed_delta<Key, Value, Version>& delta,
  keyed_snapshot<Key, Value, Version>& output,
  KeyLess less = {}, ValueEqual equal = {}) {
  if (&base == &output) return keyed_delta_status::aliased_output;
  std::size_t count = 0;
  const auto status = detail::visit_keyed_apply(base, delta, less, equal,
                                                [&](const auto&, const auto&, const auto&) {
                                                  ++count;
                                                });
  if (status != keyed_delta_status::success) return status;
  if (count > output.capacity()) return keyed_delta_status::capacity_exceeded;
  std::size_t index = 0;
  detail::visit_keyed_apply(base, delta, less, equal,
                            [&](const Key& key, const Version& version, const Value& value) {
                              if (index == output.size())
                                output.push_back({key, version, value});
                              else {
                                auto& target = output[index];
                                target.key = key;
                                target.version = version;
                                target.value = value;
                              }
                              ++index;
                            });
  output.erase(output.begin() + std::ptrdiff_t(count), output.end());
  return keyed_delta_status::success;
}

// Allocating convenience APIs. Hot-path consumers use *_into above.
template <class Key, class Value, class Version,
          class KeyLess = std::less<Key>, class ValueEqual = std::equal_to<Value>>
[[nodiscard]] keyed_delta_build_result<Key, Value, Version> make_keyed_delta(
  const keyed_snapshot<Key, Value, Version>& base,
  const keyed_snapshot<Key, Value, Version>& current,
  KeyLess less = {}, ValueEqual equal = {}) {
  keyed_delta_build_result<Key, Value, Version> result;
  std::size_t count = 0;
  result.status = detail::visit_keyed_delta(base, current, less, equal,
                                            [&](const auto&, const auto&, const auto*) {
                                              ++count;
                                            });
  if (!result.succeeded()) return result;
  result.delta.reserve(count);
  result.status = make_keyed_delta_into(base, current, result.delta, less, equal);
  return result;
}

template <class Key, class Value, class Version,
          class KeyLess = std::less<Key>, class ValueEqual = std::equal_to<Value>>
[[nodiscard]] keyed_delta_apply_result<Key, Value, Version> apply_keyed_delta(
  const keyed_snapshot<Key, Value, Version>& base,
  const keyed_delta<Key, Value, Version>& delta,
  KeyLess less = {}, ValueEqual equal = {}) {
  std::size_t count = 0;
  const auto status = detail::visit_keyed_apply(base, delta, less, equal,
                                                [&](const auto&, const auto&, const auto&) {
                                                  ++count;
                                                });
  if (status != keyed_delta_status::success) return {status, std::nullopt};
  keyed_snapshot<Key, Value, Version> result;
  result.reserve(count);
  const auto applied = apply_keyed_delta_into(base, delta, result, less, equal);
  return {applied, std::move(result)};
}

} // namespace devils_engine::network

#endif
