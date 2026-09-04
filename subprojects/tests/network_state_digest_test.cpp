#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <devils_engine/network/network.h>
#include <devils_engine/utils/sha256cpp.h>
#include <doctest/doctest.h>

namespace network = devils_engine::network;

namespace {

struct causal_state {
  std::uint32_t actors = 0;
  std::uint64_t clock = 0;
};

struct host {
  using staging_type = causal_state;

  causal_state causal;
  std::uint64_t derived_cache = 0;
};

struct actor_section {
  static constexpr std::uint32_t id = 0x1001;
  static constexpr std::uint32_t version = 1;

  static void write(const host& value, network::state_writer& writer) {
    writer.u32(value.causal.actors);
  }

  static bool read(causal_state& value, network::state_reader& reader) {
    value.actors = reader.u32();
    return reader.good();
  }

  static bool validate(const causal_state&) {
    return true;
  }
};

struct clock_section {
  static constexpr std::uint32_t id = 0x2002;
  static constexpr std::uint32_t version = 3;

  static void write(const host& value, network::state_writer& writer) {
    writer.u64(value.causal.clock);
  }

  static bool read(causal_state& value, network::state_reader& reader) {
    value.clock = reader.u64();
    return reader.good();
  }

  static bool validate(const causal_state&) {
    return true;
  }
};

using schema = network::state_schema<host, network::state_writer, network::state_reader,
                                     actor_section, clock_section>;
using reversed_schema = network::state_schema<host, network::state_writer, network::state_reader,
                                              clock_section, actor_section>;
using sha_report = network::state_digest_report<devils_engine::utils::digest>;

static_assert(network::state_digest_hasher<network::sha256_state_hasher>);
static_assert(network::state_digest_hasher<network::buffered_murmur64_state_hasher>);

[[nodiscard]] sha_report sha_digest(const host& value) {
  return network::make_state_digest<schema, network::sha256_state_hasher>(value);
}

} // namespace

TEST_CASE("state digest full root covers exact canonical bytes and ignores derived state") {
  const host source{{17, 90}, 999};
  const auto report = sha_digest(source);
  const auto canonical = schema::write(source);

  devils_engine::utils::SHA256 direct;
  direct.update(canonical.data(), canonical.size());
  CHECK(report.root == direct.finalize());
  REQUIRE(report.sections.size() == 2);
  CHECK(report.sections[0].id == actor_section::id);
  CHECK(report.sections[0].canonical_size == 20);
  CHECK(report.sections[1].id == clock_section::id);
  CHECK(report.sections[1].canonical_size == 24);
  CHECK(report == network::make_state_digest<reversed_schema, network::sha256_state_hasher>(source));

  host derived_only = source;
  derived_only.derived_cache += 1;
  CHECK(report == sha_digest(derived_only));
}

TEST_CASE("state digest localizes content, section-set and envelope mismatches") {
  const host baseline{{17, 90}, 0};
  const auto expected = sha_digest(baseline);

  host actor_changed = baseline;
  actor_changed.causal.actors += 1;
  const auto actor_result = network::compare_state_digests(expected, sha_digest(actor_changed));
  CHECK(actor_result.status == network::state_digest_comparison_status::section_mismatch);
  CHECK(actor_result.section_id == actor_section::id);

  host clock_changed = baseline;
  clock_changed.causal.clock += 1;
  const auto clock_result = network::compare_state_digests(expected, sha_digest(clock_changed));
  CHECK(clock_result.status == network::state_digest_comparison_status::section_mismatch);
  CHECK(clock_result.section_id == clock_section::id);

  auto missing = expected;
  missing.root[0] ^= 1;
  missing.sections.erase(missing.sections.begin());
  const auto set_result = network::compare_state_digests(expected, missing);
  CHECK(set_result.status == network::state_digest_comparison_status::section_set_mismatch);
  CHECK(set_result.section_id == actor_section::id);

  auto envelope = expected;
  envelope.root[0] ^= 1;
  const auto envelope_result = network::compare_state_digests(expected, envelope);
  CHECK(envelope_result.status == network::state_digest_comparison_status::root_only_mismatch);
  CHECK_FALSE(envelope_result.matched());
}

TEST_CASE("state digest accepts an explicit 64-bit non-cryptographic policy") {
  network::buffered_murmur64_state_hasher empty;
  CHECK(empty.finish() == devils_engine::utils::murmur_hash64A(std::string_view{}));

  const host baseline{{17, 90}, 0};
  const auto expected = network::make_state_digest<schema, network::buffered_murmur64_state_hasher>(baseline);
  const auto canonical = schema::write(baseline);
  CHECK(expected.root == devils_engine::utils::murmur_hash64A(std::string_view{
                           reinterpret_cast<const char*>(canonical.data()), canonical.size()}));
  auto changed = baseline;
  changed.causal.actors += 1;
  const auto actual = network::make_state_digest<schema, network::buffered_murmur64_state_hasher>(changed);
  const auto result = network::compare_state_digests(expected, actual);
  CHECK(result.status == network::state_digest_comparison_status::section_mismatch);
  CHECK(result.section_id == actor_section::id);
}

TEST_CASE("replay digest diagnostic identifies first divergent tick and owning section") {
  struct bundle_entry {
    std::uint64_t tick = 0;
    std::uint32_t bundle = 0;
  };

  const std::array bundles{
    bundle_entry{1, 2},
    bundle_entry{2, 3},
    bundle_entry{3, 5},
    bundle_entry{4, 7},
  };
  std::array<sha_report, 5> expected;
  host uninterrupted{};
  expected[0] = sha_digest(uninterrupted);
  for (const auto& entry : bundles) {
    uninterrupted.causal.actors += entry.bundle;
    uninterrupted.causal.clock = entry.tick;
    expected[entry.tick] = sha_digest(uninterrupted);
  }

  struct diagnostic {
    std::uint64_t tick = 0;
    network::state_digest_comparison comparison;
  };
  std::optional<diagnostic> first_mismatch;
  host replayed{};
  const host checkpoint{};
  const auto result = network::replay_to(
    replayed, std::uint64_t{0}, checkpoint, std::uint64_t{4}, bundles,
    [](host& destination, const host& source) {
      destination = source;
      return true;
    },
    [](host& value, const std::uint32_t bundle, const network::replay_context&) {
      value.causal.actors += bundle;
      return true;
    },
    [](host& value, const std::uint64_t tick, const network::replay_context&) {
      value.causal.clock = tick;
      if (tick == 3) value.causal.actors += 1; // injected one-field simulation divergence
      return true;
    },
    [&expected, &first_mismatch](const host& value, const std::uint64_t tick) {
      const auto comparison = network::compare_state_digests(expected[tick], sha_digest(value));
      if (!comparison.matched() && !first_mismatch.has_value()) {
        first_mismatch = diagnostic{tick, comparison};
      }
      return comparison.matched();
    },
    network::checked_tick_successor<std::uint64_t>{});

  CHECK(result.status == network::replay_status::state_mismatch);
  REQUIRE(result.tick.has_value());
  CHECK(*result.tick == 3);
  REQUIRE(first_mismatch.has_value());
  CHECK(first_mismatch->tick == 3);
  CHECK(first_mismatch->comparison.status == network::state_digest_comparison_status::section_mismatch);
  CHECK(first_mismatch->comparison.section_id == actor_section::id);
}
