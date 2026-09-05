#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <new>

#include <devils_engine/network/network.h>
#include <devils_engine/utils/float_bits.h>
#include <doctest/doctest.h>

// This executable alone replaces new. Count the measured region, not doctest,
// setup or assertions. Include aligned/array allocations so a container cannot
// escape the check simply by changing its element alignment.
namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocations = 0;

void* allocate(const std::size_t size, const std::size_t alignment = 0) {
  if (count_allocations) ++allocations;
  const auto amount = size == 0 ? 1 : size;
  void* ptr = alignment == 0 ? std::malloc(amount)
                             : std::aligned_alloc(alignment, ((amount + alignment - 1) / alignment) * alignment);
  if (!ptr) std::abort();
  return ptr;
}

void start_measurement() {
  allocations = 0;
  count_allocations = true;
}
std::size_t stop_measurement() {
  count_allocations = false;
  return allocations;
}
} // namespace

void* operator new(std::size_t n) {
  return allocate(n);
}
void* operator new[](std::size_t n) {
  return allocate(n);
}
void* operator new(std::size_t n, std::align_val_t a) {
  return allocate(n, std::size_t(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return allocate(n, std::size_t(a));
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

namespace {
namespace net = devils_engine::network;
using snapshot = net::keyed_snapshot<int, int, unsigned>;
using delta = net::keyed_delta<int, int, unsigned>;

struct wire_size {
  std::size_t operator()(const std::uint64_t&) const noexcept {
    return 24;
  }
};
struct faults {
  net::link_fault_effect operator()(const net::link_transmission& t) const noexcept {
    return {.drop = t.sequence % 3 == 0 && t.attempt == 0,
            .extra_delay_steps = t.sequence % 2,
            .duplicate_count = 1};
  }
};
struct record {
  unsigned tick, sequence;
};
struct tick_of {
  unsigned operator()(const record& v) const {
    return v.tick;
  }
};
struct less {
  bool operator()(const record& a, const record& b) const {
    return a.sequence < b.sequence;
  }
};
struct equal {
  bool operator()(const record& a, const record& b) const {
    return a.sequence == b.sequence;
  }
};

struct host {
  using staging_type = std::uint64_t;
  std::uint64_t value = 0;
};
struct section {
  static constexpr std::uint32_t id = 1, version = 1;
  static void write(const host& h, net::state_writer& w) {
    w.u64(h.value);
  }
  static bool read(host::staging_type& h, net::state_reader& r) {
    h = r.u64();
    return r.good();
  }
  static bool validate(const host::staging_type&) {
    return true;
  }
};
using schema = net::state_schema<host, net::state_writer, net::state_reader, section>;

struct moving_value {
  inline static std::size_t copies = 0, moves = 0;
  int value = 0;
  moving_value() = default;
  moving_value(const moving_value& other) : value(other.value) {
    ++copies;
  }
  moving_value(moving_value&& other) noexcept : value(other.value) {
    ++moves;
  }
  moving_value& operator=(const moving_value& other) {
    value = other.value;
    ++copies;
    return *this;
  }
  moving_value& operator=(moving_value&& other) noexcept {
    value = other.value;
    ++moves;
    return *this;
  }
  bool operator==(const moving_value&) const = default;
};

struct causal_state {
  float x = 0, velocity = 0;
  unsigned tick = 0;
};
struct causal_host {
  using staging_type = causal_state;
  causal_state state;
  unsigned presentation = 0;
};
struct causal_section {
  static constexpr std::uint32_t id = 1, version = 1;
  static void write(const causal_host& h, net::state_writer& w) {
    w.u32(std::bit_cast<std::uint32_t>(h.state.x));
    w.u32(std::bit_cast<std::uint32_t>(h.state.velocity));
    w.u32(h.state.tick);
  }
  static bool read(causal_state& s, net::state_reader& r) {
    s.x = std::bit_cast<float>(r.u32());
    s.velocity = std::bit_cast<float>(r.u32());
    s.tick = r.u32();
    return r.good();
  }
  static bool validate(const causal_state&) {
    return true;
  }
};
using causal_schema = net::state_schema<causal_host, net::state_writer, net::state_reader, causal_section>;
struct late_intent {
  unsigned tick;
  float velocity;
};
struct intent_size {
  std::size_t operator()(const late_intent&) const noexcept {
    return 8;
  }
};
struct blob_size {
  std::size_t operator()(const std::vector<std::byte>& b) const noexcept {
    return b.size();
  }
};
} // namespace

TEST_CASE("network prepared delta merge has no allocations and refuses without mutation") {
  const snapshot base{{1, 1, 10}, {2, 1, 20}, {4, 1, 40}};
  const snapshot current{{1, 2, 11}, {3, 1, 30}, {4, 1, 40}};
  delta changes;
  snapshot output;
  changes.reserve(8);
  output.reserve(8);
  bool ok = true;
  start_measurement();
  for (int i = 0; i < 10000; ++i) {
    ok &= net::make_keyed_delta_into(base, current, changes) == net::keyed_delta_status::success;
    ok &= net::apply_keyed_delta_into(base, changes, output) == net::keyed_delta_status::success;
    ok &= output == current;
  }
  const auto count = stop_measurement();
  CHECK(ok);
  CHECK(count == 0);
  snapshot small;
  CHECK(net::apply_keyed_delta_into(base, changes, small) == net::keyed_delta_status::capacity_exceeded);
  CHECK(small.empty());
  auto invalid = changes;
  invalid.back().expected_version = 99;
  const auto saved = output;
  CHECK(net::apply_keyed_delta_into(base, invalid, output) == net::keyed_delta_status::precondition_failed);
  CHECK(output == saved);
  CHECK(net::apply_keyed_delta_into(output, changes, output) == net::keyed_delta_status::aliased_output);
  delta tiny;
  CHECK(net::make_keyed_delta_into(base, current, tiny) == net::keyed_delta_status::capacity_exceeded);
  CHECK(tiny.empty());
}

TEST_CASE("network link bounds unread and delayed messages and trace") {
  net::in_memory_link<std::uint64_t, wire_size> link({1, 24, 24, 1, 1, 8});
  link.connect();
  std::size_t accepted = 0;
  for (int i = 0; i < 1000; ++i) {
    accepted += link.try_send(net::link_endpoint::first, {}, 7) == net::link_send_status::accepted;
    link.advance();
  }
  CHECK(accepted == 1);
  CHECK(link.queued_count(net::link_endpoint::first) == 0);
  CHECK(link.retained_count(net::link_endpoint::first) == 1);
  CHECK(link.retained_bytes(net::link_endpoint::first) == 24);
  CHECK(link.trace().size() == 8);
  CHECK(link.omitted_trace_events() > 0);
  link.consume(net::link_endpoint::second, [](const auto&) {});
  CHECK(link.retained_count(net::link_endpoint::first) == 0);
  CHECK(link.try_send(net::link_endpoint::first, {}, 8) == net::link_send_status::accepted);
  link.advance();
  CHECK(link.retained_count(net::link_endpoint::first) == 1);
  link.disconnect();
  CHECK(link.retained_bytes(net::link_endpoint::first) == 0);
  link.connect();
  link.clear_trace();
  CHECK(link.trace().empty());
  CHECK(link.omitted_trace_events() == 0);
}

TEST_CASE("network prepared link retries duplicates reconnects without allocations") {
  net::in_memory_link<std::uint64_t, wire_size, faults> link({8, 192, 192, 1, 1, 64});
  link.connect();
  bool ok = true;
  std::size_t reliable_deliveries = 0;
  start_measurement();
  for (std::uint64_t i = 0; i < 1000; ++i) {
    ok &= link.try_send(net::link_endpoint::first, {0, net::link_reliability::reliable_ordered}, i) == net::link_send_status::accepted;
    ok &= link.try_send(net::link_endpoint::second, {}, i) == net::link_send_status::accepted;
    for (int step = 0; step < 5; ++step)
      link.advance();
    link.consume(net::link_endpoint::second, [&](const auto& v) {
      ++reliable_deliveries;
      ok &= v.message == i;
    });
    link.consume(net::link_endpoint::first, [&](const auto& v) {
      ok &= v.message == i;
    });
    if (i % 100 == 0) {
      link.disconnect();
      link.connect();
      link.clear_trace();
    }
  }
  const auto count = stop_measurement();
  CHECK(ok);
  CHECK(reliable_deliveries == 1000);
  CHECK(count == 0);
}

TEST_CASE("network duplicate injection cannot exceed retained budgets") {
  const auto duplicate_all = [](const net::link_transmission&) {
    return net::link_fault_effect{.duplicate_count = UINT32_MAX};
  };
  net::in_memory_link<std::uint64_t, wire_size, decltype(duplicate_all)> link(
    {4, 48, 96, 1, 1, 0}, {}, duplicate_all);
  link.connect();
  REQUIRE(link.try_send(net::link_endpoint::first, {}, 1) == net::link_send_status::accepted);
  link.advance();
  CHECK(link.retained_count(net::link_endpoint::first) == 2);
  CHECK(link.retained_bytes(net::link_endpoint::first) == 48);
  CHECK(link.suppressed_duplicates() == std::size_t(UINT32_MAX) - 1);
  link.advance();
  CHECK(link.drain(net::link_endpoint::second).size() == 2);
  CHECK(link.retained_bytes(net::link_endpoint::first) == 0);
}

TEST_CASE("network journal and immutable history explicitly recycle batch ownership") {
  net::tick_journal<record, unsigned, tick_of, less, equal> journal;
  std::vector<record> storage;
  storage.reserve(4);
  journal.recycle(std::move(storage));
  net::bounded_history<unsigned, net::sealed_tick_batch<record, unsigned>> history(1, 64);
  bool ok = true;
  start_measurement();
  for (unsigned tick = 0; tick < 10000; ++tick) {
    if (auto retired = history.take_oldest()) journal.recycle(std::move(*retired).release_storage());
    const auto tag = journal.begin(tick, 4);
    ok &= journal.try_record(record{tick, 2}) == net::tick_record_result::recorded;
    ok &= journal.try_record(record{tick, 1}) == net::tick_record_result::recorded;
    ok &= journal.seal() == net::tick_seal_result::sealed;
    ok &= history.try_store(tick, journal.consume(tag), 16).stored();
    journal.retire(tag);
    ok &= history.find(tick)->records()[0].sequence == 1;
  }
  const auto count = stop_measurement();
  CHECK(ok);
  CHECK(count == 0);
}

TEST_CASE("network canonical checkpoint and murmur diagnostics use prepared bytes") {
  std::vector<std::byte> bytes, scratch;
  bytes.reserve(40);
  scratch.reserve(8);
  net::state_digest_report<std::uint64_t> report;
  report.sections.reserve(schema::section_count);
  const auto expected = net::make_state_digest<schema, net::buffered_murmur64_state_hasher>(host{99});
  bool ok = true;
  start_measurement();
  for (int i = 0; i < 10000; ++i) {
    ok &= schema::try_write(host{99}, bytes, scratch);
    ok &= net::try_murmur64_digest<schema>(bytes, report) == net::state_digest_build_status::built;
    ok &= report == expected;
  }
  const auto count = stop_measurement();
  CHECK(ok);
  CHECK(count == 0);
  CHECK(bytes == schema::write(host{99}));
  std::vector<std::byte> empty;
  CHECK_FALSE(schema::try_write(host{99}, bytes, empty));
  CHECK_FALSE(schema::try_write(host{99}, empty, scratch));
  CHECK_FALSE(schema::try_write(host{99}, bytes, bytes));
  REQUIRE(schema::try_write(host{99}, bytes, scratch));
  bytes.pop_back();
  CHECK(net::try_murmur64_digest<schema>(bytes, report) == net::state_digest_build_status::invalid_document);
  CHECK(report == expected);
}

TEST_CASE("network float equality follows serialized bits including zero and NaN") {
  using float_snapshot = net::keyed_snapshot<int, float, unsigned>;
  const auto equal_bits = devils_engine::utils::float_bits_equal{};
  const float_snapshot positive{{1, 1, 0.0f}};
  const float_snapshot negative{{1, 1, -0.0f}};
  CHECK(net::make_keyed_delta(positive, negative, std::less<int>{}, equal_bits).status == net::keyed_delta_status::version_not_advanced);
  auto advanced = negative;
  advanced[0].version = 2;
  const auto changes = net::make_keyed_delta(positive, advanced, std::less<int>{}, equal_bits);
  REQUIRE(changes.succeeded());
  const auto applied = net::apply_keyed_delta(positive, changes.delta, std::less<int>{}, equal_bits);
  REQUIRE(applied.succeeded());
  CHECK(equal_bits(applied.snapshot->front().value, -0.0f));
  const float nan = std::bit_cast<float>(UINT32_C(0x7fc00001));
  const float_snapshot nans{{1, 1, nan}};
  CHECK(net::make_keyed_delta(nans, nans, std::less<int>{}, equal_bits).delta.empty());
  CHECK_FALSE(equal_bits(nan, std::bit_cast<float>(UINT32_C(0x7fc00002))));
  static_assert(!devils_engine::utils::float_bits_equal{}(0.0, -0.0));
}

TEST_CASE("network journal refuses indistinguishable ordering keys") {
  const auto tied = [](const record&, const record&) {
    return false;
  };
  for (unsigned first = 1; first <= 2; ++first) {
    net::tick_journal<record, unsigned, tick_of, decltype(tied), equal> journal;
    const auto tag = journal.begin(1, 2);
    journal.try_record(record{1, first});
    journal.try_record(record{1, 3 - first});
    CHECK(journal.seal() == net::tick_seal_result::ambiguous_order);
    journal.retire(tag);
  }
}

TEST_CASE("network delta mass deletion has linear work and does not copy survivors twice") {
  for (const std::size_t n : {1000, 2000, 4000}) {
    net::keyed_snapshot<int, moving_value, unsigned> base(n), output;
    net::keyed_delta<int, moving_value, unsigned> changes;
    changes.reserve(n);
    output.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      base[i].key = int(i);
      base[i].version = 1;
      changes.push_back({int(i), 1, std::nullopt});
    }
    moving_value::copies = moving_value::moves = 0;
    REQUIRE(net::apply_keyed_delta_into(base, changes, output) == net::keyed_delta_status::success);
    CHECK(output.empty());
    CHECK(moving_value::copies == 0);
    CHECK(moving_value::moves == 0);
    changes.clear();
    moving_value::copies = moving_value::moves = 0;
    REQUIRE(net::apply_keyed_delta_into(base, changes, output) == net::keyed_delta_status::success);
    CHECK(output.size() == n);
    CHECK(moving_value::copies == n);
    CHECK(moving_value::moves <= n);
  }
}

TEST_CASE("network history slot addresses survive rotation and moved-from storage is empty") {
  net::bounded_history<unsigned, unsigned> history(3, 12);
  REQUIRE(history.try_store(1, 10, 4).stored());
  REQUIRE(history.try_store(2, 20, 4).stored());
  const auto* second = history.find_entry(2);
  REQUIRE(history.try_store(3, 30, 4).stored());
  REQUIRE(history.try_store(4, 40, 4).stored());
  CHECK(history.find_entry(2) == second);
  CHECK(history.entries()[0].tick == 2);
  CHECK(history.entries()[2].tick == 4);
  auto moved = std::move(history);
  CHECK(moved.find_entry(2) == second);
  CHECK(history.empty());
  CHECK(history.find(2) == nullptr);
  CHECK(history.try_store(5, 50, 4).status == net::history_store_status::budget_exceeded);
}

TEST_CASE("network delayed authoritative input restores checkpoint replays and matches digest") {
  // The reference and prediction share a fixed step, but deliberately receive
  // different input at tick 2. Reliable loss/retry + latency delivers authority
  // only after prediction has reached tick 6. No rendering is replayed.
  const auto delay = [](const net::link_transmission& t) {
    return net::link_fault_effect{.drop = t.attempt == 0, .extra_delay_steps = 3};
  };
  net::in_memory_link<late_intent, intent_size, decltype(delay)> link({4, 32, 32, 1, 1, 64}, {}, delay);
  link.connect();
  net::checkpoint_ring<unsigned, std::vector<std::byte>, blob_size> checkpoints(2, 128);
  std::vector<std::byte> bytes, scratch;
  bytes.reserve(64);
  scratch.reserve(16);
  causal_host reference, predicted;
  REQUIRE(causal_schema::try_write(reference, bytes, scratch));
  REQUIRE(checkpoints.try_store(0, std::move(bytes)).stored());
  bytes.reserve(64);
  struct entry {
    unsigned tick;
    float bundle;
  };
  std::array<entry, 6> history{};
  const auto integrate = [](causal_host& h, const unsigned tick, const bool presentation) {
    h.state.x += h.state.velocity * 0.1f;
    h.state.tick = tick;
    if (presentation) ++h.presentation;
  };
  for (unsigned tick = 1; tick <= 6; ++tick) {
    const float velocity = tick == 2 ? 2.25f : 1.5f;
    history[tick - 1] = {tick, 1.5f};
    reference.state.velocity = velocity;
    predicted.state.velocity = 1.5f;
    integrate(reference, tick, true);
    integrate(predicted, tick, true);
  }
  net::state_digest_report<std::uint64_t> expected, actual;
  expected.sections.reserve(1);
  actual.sections.reserve(1);
  REQUIRE(causal_schema::try_write(reference, bytes, scratch));
  REQUIRE(net::try_murmur64_digest<causal_schema>(bytes, expected) == net::state_digest_build_status::built);
  REQUIRE(causal_schema::try_write(predicted, bytes, scratch));
  REQUIRE(net::try_murmur64_digest<causal_schema>(bytes, actual) == net::state_digest_build_status::built);
  CHECK_FALSE(net::compare_state_digests(expected, actual).matched());
  REQUIRE(link.try_send(net::link_endpoint::first, {0, net::link_reliability::reliable_ordered}, late_intent{2, 2.25f}) == net::link_send_status::accepted);

  bool received = false;
  bool ok = true;
  net::replay_result<unsigned> replayed;
  start_measurement();
  for (int step = 0; step < 6; ++step)
    link.advance();
  link.consume(net::link_endpoint::second, [&](const auto& delivery) {
    received = true;
    const auto input = delivery.message;
    history[input.tick - 1].bundle = input.velocity;
    const auto* checkpoint = checkpoints.latest_at_or_before(input.tick - 1);
    causal_host staging = predicted;
    replayed = net::replay_to(staging, checkpoint->tick, checkpoint->bundle, 6u, history, [](causal_host& h, const std::vector<std::byte>& blob) {
      net::state_reader reader{blob};
      return causal_schema::load(h, reader, causal_state{}, [](const causal_state&) {
               return true;
             },
                                 [](causal_host& live, causal_state&& state) noexcept {
                                   live.state = state;
                                 })
        .loaded();
    },
                              [&](causal_host& h, const float velocity, const net::replay_context ctx) {
                                ok &= ctx.presentation_suppressed();
                                h.state.velocity = velocity;
                                return true;
                              },
                              [&](causal_host& h, const unsigned tick, const net::replay_context ctx) {
                                integrate(h, tick, !ctx.presentation_suppressed());
                                return true;
                              },
                              [](const causal_host&, unsigned) {
                                return true;
                              },
                              net::checked_tick_successor<unsigned>{});
    if (replayed.completed()) predicted = staging;
  });
  ok &= causal_schema::try_write(predicted, bytes, scratch);
  ok &= net::try_murmur64_digest<causal_schema>(bytes, actual) == net::state_digest_build_status::built;
  const auto count = stop_measurement();
  CHECK(ok);
  CHECK(received);
  CHECK(replayed.completed());
  CHECK(replayed.replayed_ticks == 6);
  CHECK(predicted.presentation == 6);
  CHECK(net::compare_state_digests(expected, actual).matched());
  CHECK(count == 0);
}
