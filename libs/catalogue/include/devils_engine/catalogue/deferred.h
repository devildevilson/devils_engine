#ifndef DEVILS_ENGINE_CATALOGUE_DEFERRED_H
#define DEVILS_ENGINE_CATALOGUE_DEFERRED_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/string_id.h"
#include "devils_engine/utils/type_traits.h"

// Typed deferred effects for MT gameplay evaluation.
//
// The native function stays an ordinary `void(Args...)`. `fn_deferred_ptr` mirrors that signature,
// but records owned arguments into the executor bound to its strategy domain. The body runs only
// after `seal()` in deterministic `(key, source, local_sequence, function)` order.
//
// Record-time invariants:
// - `begin_record()` sizes the dense call journal before workers start;
// - one worker owns one source_index for the whole record_scope;
// - record_scope is per-worker ambient metadata only (source id/index + local sequence), not world state;
// - set_executor() is changed only outside a record/commit phase.
//
// Each call owns a fixed 128-byte inline payload. A signature that cannot fit is a compile-time error;
// there is no oversized runtime path or per-call allocation for the type-erased call object.

namespace devils_engine {
namespace catalogue {
namespace mt {

inline constexpr size_t deferred_payload_size = 128;

enum class arbitration : uint8_t {
  collect,
  elect
};

namespace commit {
struct serial {};
struct parallel_groups {};
struct serial_structural {};
} // namespace commit

namespace conflict {
struct none {};
// Elect group is suppressed when its key is also a source in the same strategy pass. This models
// symmetric/cascade interaction protection such as "an entity attempting to eat cannot be eaten".
struct target_not_source {};
} // namespace conflict

struct call_metadata {
  uint64_t key = 0;
  uint64_t source = 0;
  uint64_t local_sequence = 0;
  uint64_t function = 0;
  size_t source_index = 0;
};

namespace detail {

template <typename T>
uint64_t stable_key(const T& value) {
  using U = std::remove_cvref_t<T>;
  if constexpr (std::is_integral_v<U>) {
    return static_cast<uint64_t>(value);
  } else if constexpr (std::is_enum_v<U>) {
    return static_cast<uint64_t>(static_cast<std::underlying_type_t<U>>(value));
  } else if constexpr (requires { value.id; }) {
    return stable_key(value.id);
  } else {
    static_assert(std::is_integral_v<U>,
                  "catalogue::mt key must be integral, enum, or expose an integral/enum .id member");
  }
}

struct record_context {
  uint64_t source = 0;
  size_t source_index = 0;
  uint32_t next_local_ordinal = 0;
};

inline thread_local record_context* current_record_context = nullptr;

inline record_context& require_record_context() {
  if (current_record_context == nullptr) {
    utils::error{}("catalogue::mt deferred function called without an active record_scope");
  }
  return *current_record_context;
}

template <typename T>
struct stored_arg {
  using type = std::remove_cvref_t<T>;
};

template <>
struct stored_arg<std::string_view> {
  using type = std::string;
};

template <>
struct stored_arg<const std::string_view> {
  using type = std::string;
};

template <typename T>
using stored_arg_t = typename stored_arg<T>::type;

template <typename Formal>
stored_arg_t<Formal> capture_arg(const Formal& value) {
  if constexpr (std::is_same_v<std::remove_cv_t<Formal>, std::string_view>) {
    return std::string(value);
  } else {
    return stored_arg_t<Formal>(value);
  }
}

template <typename Formal, typename Stored>
decltype(auto) materialize_arg(Stored& value) {
  if constexpr (std::is_same_v<std::remove_cv_t<Formal>, std::string_view>) {
    return std::string_view(value);
  } else {
    return std::remove_cv_t<Formal>(value);
  }
}

template <auto Fn, typename... Args>
class call_payload {
public:
  static_assert((!std::is_reference_v<Args> && ...),
                "catalogue::mt first slice requires value arguments; pass stable handles/ids by value");
  static_assert((std::is_copy_constructible_v<stored_arg_t<Args>> && ...),
                "catalogue::mt first slice requires copyable owned arguments");
  static_assert((std::is_nothrow_destructible_v<stored_arg_t<Args>> && ...),
                "catalogue::mt deferred owned arguments must be nothrow destructible");

  explicit call_payload(const Args&... args) : args_(capture_arg<Args>(args)...) {}

  void invoke() {
    invoke_impl(std::index_sequence_for<Args...>{});
  }

private:
  template <size_t... I>
  void invoke_impl(std::index_sequence<I...>) {
    std::invoke(Fn, materialize_arg<Args>(std::get<I>(args_))...);
  }

  std::tuple<stored_arg_t<Args>...> args_;
};

class call_slot {
public:
  call_slot() noexcept = default;

  ~call_slot() noexcept {
    reset();
  }

  call_slot(const call_slot&) = delete;
  call_slot(call_slot&&) = delete;
  call_slot& operator=(const call_slot&) = delete;
  call_slot& operator=(call_slot&&) = delete;

  bool occupied() const noexcept {
    return invoke_ != nullptr;
  }

  const call_metadata& metadata() const noexcept {
    return metadata_;
  }

  template <auto Fn, typename... Args>
  void emplace(const call_metadata metadata, const Args&... args) {
    using payload_t = call_payload<Fn, Args...>;
    static_assert(sizeof(payload_t) <= deferred_payload_size,
                  "catalogue::mt deferred call payload exceeds the fixed 128-byte journal ABI");
    static_assert(alignof(payload_t) <= alignof(std::max_align_t),
                  "catalogue::mt deferred call payload requires unsupported over-alignment");

    reset();
    metadata_ = metadata;
    std::construct_at(reinterpret_cast<payload_t*>(payload_.data()), args...);
    invoke_ = [](void* memory) {
      std::launder(reinterpret_cast<payload_t*>(memory))->invoke();
    };
    destroy_ = [](void* memory) noexcept {
      std::destroy_at(std::launder(reinterpret_cast<payload_t*>(memory)));
    };
  }

  void invoke() {
    invoke_(payload_.data());
  }

  void reset() noexcept {
    if (destroy_ != nullptr) destroy_(payload_.data());
    invoke_ = nullptr;
    destroy_ = nullptr;
  }

private:
  using invoke_fn = void (*)(void*);
  using destroy_fn = void (*)(void*) noexcept;

  call_metadata metadata_{};
  invoke_fn invoke_ = nullptr;
  destroy_fn destroy_ = nullptr;
  alignas(std::max_align_t) std::array<std::byte, deferred_payload_size> payload_{};
};

template <typename T>
struct callable_traits : public utils::detail::function_traits_v2<T> {};

} // namespace detail

// Binds stable source provenance to every deferred call made by the current worker. The local ordinal is
// shared across ALL strategy domains used by this scope; executor maps it to the deterministic global id
// `source_index * sequence_capacity + local_ordinal`.
class record_scope {
public:
  record_scope(const uint64_t source, const size_t source_index) noexcept
    : previous_(detail::current_record_context), context_{source, source_index, 0} {
    detail::current_record_context = &context_;
  }

  ~record_scope() noexcept {
    detail::current_record_context = previous_;
  }

  record_scope(const record_scope&) = delete;
  record_scope(record_scope&&) = delete;
  record_scope& operator=(const record_scope&) = delete;
  record_scope& operator=(record_scope&&) = delete;

  uint32_t next_local_ordinal() const noexcept {
    return context_.next_local_ordinal;
  }

private:
  detail::record_context* previous_;
  detail::record_context context_;
};

namespace key {

template <size_t I>
struct entity_arg {
  template <typename... Args>
  static uint64_t get(const Args&... args) {
    static_assert(I < sizeof...(Args), "catalogue::mt key argument index is out of range");
    return detail::stable_key(std::get<I>(std::forward_as_tuple(args...)));
  }
};

} // namespace key

namespace order {

struct source_then_sequence {
  static bool less(const call_metadata& a, const call_metadata& b) noexcept {
    if (a.source != b.source) return a.source < b.source;
    if (a.local_sequence != b.local_sequence) return a.local_sequence < b.local_sequence;
    return a.function < b.function;
  }
};

} // namespace order

template <typename Key, typename Order = order::source_then_sequence,
          typename Commit = commit::parallel_groups>
struct collect {
  using key_policy = Key;
  using order_policy = Order;
  using commit_policy = Commit;
  using conflict_policy = conflict::none;
  static constexpr arbitration arbitration_policy = arbitration::collect;
};

template <typename Key, typename Order = order::source_then_sequence,
          typename Commit = commit::serial, typename Conflict = conflict::none>
struct elect {
  using key_policy = Key;
  using order_policy = Order;
  using commit_policy = Commit;
  using conflict_policy = Conflict;
  static constexpr arbitration arbitration_policy = arbitration::elect;
};

enum class executor_phase : uint8_t {
  idle,
  recording,
  sealed,
  committed
};

template <typename Strategy>
class executor {
public:
  struct group_view {
    uint64_t key = 0;
    size_t begin = 0;
    size_t end = 0;
    bool eligible = true;

    size_t size() const noexcept {
      return end - begin;
    }
  };

  using strategy_type = Strategy;
  using commit_policy = typename Strategy::commit_policy;
  static constexpr bool parallel_groups = std::is_same_v<commit_policy, commit::parallel_groups>;

  executor_phase phase() const noexcept {
    return phase_;
  }

  size_t source_capacity() const noexcept {
    return source_capacity_;
  }

  uint32_t sequence_capacity() const noexcept {
    return sequence_capacity_;
  }

  size_t call_capacity() const noexcept {
    return call_capacity_;
  }

  size_t size() const noexcept {
    return sealed_indices_.size();
  }

  size_t group_count() const noexcept {
    return groups_.size();
  }

  const group_view& group(const size_t index) const {
    return groups_.at(index);
  }

  const call_metadata& metadata(const size_t sealed_index) const {
    return slots_[sealed_indices_.at(sealed_index)].metadata();
  }

  // Allocates/grows the dense journal and opens a new record phase. sequence_capacity is the maximum
  // number of effect positions in one source script pass; call_capacity is this strategy's actual
  // per-pass budget and does not multiply source_capacity by sequence_capacity.
  void begin_record(const size_t source_capacity, const uint32_t sequence_capacity,
                    const size_t call_capacity) {
    if (phase_ == executor_phase::recording || phase_ == executor_phase::sealed) {
      utils::error{}("catalogue::mt executor begin_record called before the previous phase was committed");
    }
    if (sequence_capacity == 0 && source_capacity != 0) {
      utils::error{}("catalogue::mt executor sequence_capacity must be non-zero when sources are present");
    }
    if (source_capacity == 0 && call_capacity != 0) {
      utils::error{}("catalogue::mt executor cannot budget {} calls without sources", call_capacity);
    }

    const size_t previous_count = record_count_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < previous_count; ++i)
      slots_[i].reset();
    if (storage_capacity_ < call_capacity) {
      slots_ = std::make_unique<detail::call_slot[]>(call_capacity);
      storage_capacity_ = call_capacity;
    }
    source_capacity_ = source_capacity;
    sequence_capacity_ = sequence_capacity;
    call_capacity_ = call_capacity;
    record_count_.store(0, std::memory_order_relaxed);
    sealed_indices_.clear();
    groups_.clear();
    sources_.clear();
    phase_ = executor_phase::recording;
  }

  template <auto Fn, typename... Args>
  void record(const uint64_t function, const detail::record_context& context,
              const uint32_t local_ordinal, const Args&... args) {
    static_assert(std::is_void_v<typename detail::callable_traits<decltype(Fn)>::result_type>,
                  "catalogue::mt deferred functions must return void");
    if (phase_ != executor_phase::recording) {
      utils::error{}("catalogue::mt deferred function recorded outside begin_record/seal phase");
    }
    if (context.source_index >= source_capacity_) {
      utils::error{}("catalogue::mt source index {} is outside executor capacity {}", context.source_index,
                     source_capacity_);
    }
    if (local_ordinal >= sequence_capacity_) {
      utils::error{}("catalogue::mt local ordinal {} exceeds per-source capacity {}", local_ordinal,
                     sequence_capacity_);
    }
    if (uint64_t(context.source_index) >
        (std::numeric_limits<uint64_t>::max() - local_ordinal) / sequence_capacity_) {
      utils::error{}("catalogue::mt global sequence overflow: source index {}, local ordinal {}",
                     context.source_index, local_ordinal);
    }
    const uint64_t global_sequence =
      uint64_t(context.source_index) * uint64_t(sequence_capacity_) + local_ordinal;

    size_t index = record_count_.load(std::memory_order_relaxed);
    while (true) {
      if (index >= call_capacity_) {
        utils::error{}("catalogue::mt dense journal capacity {} exceeded", call_capacity_);
      }
      if (record_count_.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) break;
    }
    const uint64_t group_key = Strategy::key_policy::get(args...);
    const call_metadata metadata{group_key, context.source, global_sequence, function, context.source_index};
    slots_[index].template emplace<Fn>(metadata, args...);
  }

  // Barrier-side deterministic preparation. No worker may record after this call.
  void seal() {
    if (phase_ != executor_phase::recording) {
      utils::error{}("catalogue::mt executor seal called outside the recording phase");
    }

    const size_t recorded_count = record_count_.load(std::memory_order_acquire);
    sealed_indices_.clear();
    sealed_indices_.reserve(recorded_count);
    for (size_t i = 0; i < recorded_count; ++i) {
      if (slots_[i].occupied()) sealed_indices_.push_back(i);
    }

    std::sort(sealed_indices_.begin(), sealed_indices_.end(), [this](const size_t a, const size_t b) {
      const call_metadata& ma = slots_[a].metadata();
      const call_metadata& mb = slots_[b].metadata();
      if (ma.key != mb.key) return ma.key < mb.key;
      return Strategy::order_policy::less(ma, mb);
    });

    if constexpr (Strategy::arbitration_policy == arbitration::elect &&
                  std::is_same_v<typename Strategy::conflict_policy, conflict::target_not_source>) {
      sources_.reserve(sealed_indices_.size());
      for (const size_t index : sealed_indices_)
        sources_.push_back(slots_[index].metadata().source);
      std::sort(sources_.begin(), sources_.end());
      sources_.erase(std::unique(sources_.begin(), sources_.end()), sources_.end());
    }

    groups_.clear();
    size_t begin = 0;
    while (begin < sealed_indices_.size()) {
      const uint64_t group_key = slots_[sealed_indices_[begin]].metadata().key;
      size_t end = begin + 1;
      while (end < sealed_indices_.size() && slots_[sealed_indices_[end]].metadata().key == group_key) {
        ++end;
      }
      bool eligible = true;
      if constexpr (Strategy::arbitration_policy == arbitration::elect &&
                    std::is_same_v<typename Strategy::conflict_policy, conflict::target_not_source>) {
        eligible = !std::binary_search(sources_.begin(), sources_.end(), group_key);
      }
      groups_.push_back(group_view{group_key, begin, end, eligible});
      begin = end;
    }
    phase_ = executor_phase::sealed;
  }

  // Executes one independent key-group. For commit::parallel_groups callers may distribute distinct
  // group indices to workers, wait, then call finish_commit(). Calls inside one collect group stay serial.
  void dispatch_group(const size_t group_index) {
    if (phase_ != executor_phase::sealed) {
      utils::error{}("catalogue::mt dispatch_group called before seal or after finish_commit");
    }
    const group_view& g = groups_.at(group_index);
    if (!g.eligible) return;
    if constexpr (Strategy::arbitration_policy == arbitration::elect) {
      slots_[sealed_indices_[g.begin]].invoke(); // first = lowest source/local_sequence by total order
    } else {
      for (size_t i = g.begin; i < g.end; ++i) {
        slots_[sealed_indices_[i]].invoke();
      }
    }
  }

  // Serial convenience path, mandatory for serial/serial_structural and useful as a deterministic
  // fallback for parallel_groups.
  void commit() {
    if (phase_ != executor_phase::sealed) {
      utils::error{}("catalogue::mt executor commit called before seal");
    }
    for (size_t i = 0; i < groups_.size(); ++i)
      dispatch_group(i);
    phase_ = executor_phase::committed;
  }

  // Completes an externally parallelized group commit after the caller's worker barrier.
  void finish_commit() {
    if (phase_ != executor_phase::sealed) {
      utils::error{}("catalogue::mt finish_commit called before seal or after commit");
    }
    phase_ = executor_phase::committed;
  }

private:
  std::unique_ptr<detail::call_slot[]> slots_;
  std::vector<size_t> sealed_indices_;
  std::vector<group_view> groups_;
  std::vector<uint64_t> sources_;
  std::atomic_size_t record_count_ = 0;
  size_t source_capacity_ = 0;
  size_t call_capacity_ = 0;
  size_t storage_capacity_ = 0;
  uint32_t sequence_capacity_ = 0;
  executor_phase phase_ = executor_phase::idle;
};

template <typename Strategy>
struct domain {
  inline static executor<Strategy>* executor_i = nullptr;

  static void set_executor(executor<Strategy>* ptr) noexcept {
    executor_i = ptr;
  }

  static executor<Strategy>* executor_instance() noexcept {
    return executor_i;
  }

  template <auto Fn, utils::template_string_t Name, utils::template_string_t... ArgNames>
  struct fn_traits {
    using fn_t = decltype(Fn);
    using traits = detail::callable_traits<fn_t>;
    using return_t = typename traits::result_type;

    static_assert(std::is_pointer_v<fn_t> && std::is_function_v<std::remove_pointer_t<fn_t>>,
                  "catalogue::mt first slice supports free function pointers");
    static_assert(std::is_void_v<return_t>, "catalogue::mt fn_deferred_ptr is only valid for effect functions");

    static constexpr std::string_view name = Name.sv();
    static constexpr utils::id function_id = utils::murmur_hash64A(name);
    static constexpr size_t argument_count = traits::argument_count;
    static constexpr std::array<std::string_view, sizeof...(ArgNames)> argument_names{ArgNames.sv()...};

    template <typename T>
    struct maker;

    template <typename... Args>
    struct maker<void (*)(Args...)> {
      using pointer_t = void (*)(Args...);

      static void call(Args... args) {
        executor<Strategy>* ex = executor_i;
        if (ex == nullptr) {
          utils::error{}("catalogue::mt deferred function '{}' called without a strategy executor", name);
        }
        detail::record_context& context = detail::require_record_context();
        if (context.next_local_ordinal == std::numeric_limits<uint32_t>::max()) {
          utils::error{}("catalogue::mt local ordinal overflow for source {}", context.source);
        }
        const uint32_t local_ordinal = context.next_local_ordinal++;
        ex->template record<Fn>(function_id, context, local_ordinal, args...);
      }
    };

    template <typename... Args>
    struct maker<void (*)(Args...) noexcept> : maker<void (*)(Args...)> {};

    using pointer_t = typename maker<fn_t>::pointer_t;
    static constexpr pointer_t fn_deferred_ptr = &maker<fn_t>::call;
  };
};

} // namespace mt
} // namespace catalogue
} // namespace devils_engine

#endif
