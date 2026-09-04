#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <devils_engine/network/network.h>
#include <doctest/doctest.h>

namespace network = devils_engine::network;

namespace {

struct fake_state {
  std::uint32_t actors = 0;
  std::uint64_t clock = 0;
  bool operator==(const fake_state&) const = default;
};

struct fake_host {
  using staging_type = fake_state;

  fake_state causal;
  std::uint64_t derived_cache = 0;
  std::uint32_t replacements = 0;
  bool operator==(const fake_host&) const = default;
};

struct actor_section {
  static constexpr std::uint32_t id = 0x1001;
  static constexpr std::uint32_t version = 1;

  static void write(const fake_host& host, network::state_writer& writer) {
    writer.u32(host.causal.actors);
  }

  static bool read(fake_state& staging, network::state_reader& reader) {
    staging.actors = reader.u32();
    return reader.good() && staging.actors != UINT32_MAX;
  }

  static bool validate(const fake_state& staging) {
    return staging.actors <= 100;
  }
};

struct clock_section {
  static constexpr std::uint32_t id = 0x2002;
  static constexpr std::uint32_t version = 3;

  static void write(const fake_host& host, network::state_writer& writer) {
    writer.u64(host.causal.clock);
  }

  static bool read(fake_state& staging, network::state_reader& reader) {
    staging.clock = reader.u64();
    return reader.good();
  }

  static bool validate(const fake_state& staging) {
    return staging.clock <= 1'000'000;
  }
};

struct actor_section_v2 : actor_section {
  static constexpr std::uint32_t version = 2;
};

struct actor_section_other_id : actor_section {
  static constexpr std::uint32_t id = 0x1002;
};

using schema = network::state_schema<fake_host, network::state_writer, network::state_reader,
                                     actor_section, clock_section>;
using reversed_schema = network::state_schema<fake_host, network::state_writer, network::state_reader,
                                              clock_section, actor_section>;
using versioned_schema = network::state_schema<fake_host, network::state_writer, network::state_reader,
                                               actor_section_v2, clock_section>;
using reidentified_schema = network::state_schema<fake_host, network::state_writer, network::state_reader,
                                                  actor_section_other_id, clock_section>;

static_assert(network::unique_state_section_ids_v<actor_section, clock_section>);
static_assert(!network::unique_state_section_ids_v<actor_section, actor_section_v2>);
static_assert(schema::schema_digest() == reversed_schema::schema_digest());
static_assert(schema::schema_digest() != versioned_schema::schema_digest());
static_assert(schema::schema_digest() != reidentified_schema::schema_digest());

struct validate_host {
  bool operator()(const fake_state& staging) const noexcept {
    return staging.actors <= staging.clock;
  }
};

struct replace_host {
  void operator()(fake_host& host, fake_state&& staging) const noexcept {
    host.causal = std::move(staging);
    host.derived_cache = host.causal.actors + host.causal.clock;
    ++host.replacements;
  }
};

network::state_load_result load(fake_host& destination, const std::span<const std::byte> bytes) {
  network::state_reader reader{bytes};
  return schema::load(destination, reader, fake_state{}, validate_host{}, replace_host{});
}

void patch_u32(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value) {
  REQUIRE(offset + 4 <= bytes.size());
  for (unsigned i = 0; i < 4; ++i) {
    bytes[offset + i] = std::byte(std::uint8_t(value >> (i * 8)));
  }
}

void patch_u64(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value) {
  REQUIRE(offset + 8 <= bytes.size());
  for (unsigned i = 0; i < 8; ++i) {
    bytes[offset + i] = std::byte(std::uint8_t(value >> (i * 8)));
  }
}

constexpr std::uint64_t fnv_offset = UINT64_C(14695981039346656037);
constexpr std::uint64_t fnv_prime = UINT64_C(1099511628211);

void hash_byte(std::uint64_t& hash, const std::uint8_t value) {
  hash ^= value;
  hash *= fnv_prime;
}

std::uint64_t hash_bytes(const std::span<const std::byte> bytes) {
  std::uint64_t result = fnv_offset;
  for (const std::byte value : bytes)
    hash_byte(result, std::to_integer<std::uint8_t>(value));
  return result;
}

struct hash_sink {
  std::uint64_t value = fnv_offset;

  void u32(const std::uint32_t input) {
    for (unsigned i = 0; i < 4; ++i)
      hash_byte(value, std::uint8_t(input >> (i * 8)));
  }

  void u64(const std::uint64_t input) {
    for (unsigned i = 0; i < 8; ++i)
      hash_byte(value, std::uint8_t(input >> (i * 8)));
  }

  void bytes(const std::span<const std::byte> input) {
    for (const std::byte byte : input)
      hash_byte(value, std::to_integer<std::uint8_t>(byte));
  }
};

} // namespace

TEST_CASE("network state schema order is canonical and metadata changes its identity") {
  const fake_host source{{17, 90}, 999, 4};
  CHECK(schema::write(source) == reversed_schema::write(source));
  CHECK(schema::write(source) != versioned_schema::write(source));
  CHECK(schema::write(source) != reidentified_schema::write(source));
  CHECK(schema::compatibility == network::state_compatibility_policy::exact);
}

TEST_CASE("network state schema loads staging and replaces the whole host once") {
  const fake_host source{{17, 90}, 999, 4};
  fake_host destination{{2, 10}, 12, 7};

  const auto result = load(destination, schema::write(source));
  REQUIRE(result.loaded());
  CHECK(destination.causal == source.causal);
  CHECK(destination.derived_cache == 107);
  CHECK(destination.replacements == 8);
}

TEST_CASE("network state schema truncated data never changes the live host") {
  const fake_host source{{17, 90}, 999, 4};
  const auto complete = schema::write(source);

  for (std::size_t size = 0; size < complete.size(); ++size) {
    fake_host destination{{2, 10}, 12, 7};
    const fake_host before = destination;
    const auto result = load(destination, std::span<const std::byte>(complete).first(size));
    CHECK_FALSE(result.loaded());
    CHECK(destination == before);
  }
}

TEST_CASE("network state schema section and host validation are transactional") {
  SUBCASE("section decode refusal") {
    const fake_host source{{17, 90}, 0, 0};
    auto bytes = schema::write(source);
    patch_u32(bytes, 36, UINT32_MAX);
    fake_host destination{{2, 10}, 12, 7};
    const fake_host before = destination;
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::section_decode_failed);
    CHECK(result.section_id == actor_section::id);
    CHECK(destination == before);
  }

  SUBCASE("section-local truncation") {
    const fake_host source{{17, 90}, 0, 0};
    auto bytes = schema::write(source);
    patch_u64(bytes, 28, 3);
    fake_host destination{{2, 10}, 12, 7};
    const fake_host before = destination;
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::section_decode_failed);
    CHECK(result.section_id == actor_section::id);
    CHECK(destination == before);
  }

  SUBCASE("section validation") {
    const fake_host source{{101, 200}, 0, 0};
    fake_host destination{{2, 10}, 12, 7};
    const fake_host before = destination;
    const auto result = load(destination, schema::write(source));
    CHECK(result.status == network::state_load_status::section_validation_failed);
    CHECK(result.section_id == actor_section::id);
    CHECK(destination == before);
  }

  SUBCASE("cross-section host validation") {
    const fake_host source{{50, 20}, 0, 0};
    fake_host destination{{2, 10}, 12, 7};
    const fake_host before = destination;
    const auto result = load(destination, schema::write(source));
    CHECK(result.status == network::state_load_status::host_validation_failed);
    CHECK(destination == before);
  }
}

TEST_CASE("network state schema diagnoses missing duplicate unknown and reordered sections") {
  const fake_host source{{17, 90}, 0, 0};
  const auto canonical = schema::write(source);
  // Header is 20 bytes. First section is [16-byte header][4-byte body], so the
  // second section ID begins at byte 40.

  SUBCASE("missing") {
    auto bytes = canonical;
    patch_u32(bytes, 16, 1);
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::missing_section);
    CHECK(result.section_id == clock_section::id);
  }

  SUBCASE("duplicate") {
    auto bytes = canonical;
    patch_u32(bytes, 40, actor_section::id);
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::duplicate_section);
    CHECK(result.section_id == actor_section::id);
  }

  SUBCASE("unknown") {
    auto bytes = canonical;
    patch_u32(bytes, 20, actor_section::id - 1);
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::unknown_section);
    CHECK(result.section_id == actor_section::id - 1);
  }

  SUBCASE("non-canonical order") {
    auto bytes = canonical;
    patch_u32(bytes, 40, actor_section::id - 1);
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::non_canonical_order);
    CHECK(result.section_id == actor_section::id - 1);
  }
}

TEST_CASE("network state schema diagnoses version and schema digest changes") {
  const fake_host source{{17, 90}, 0, 0};
  const auto canonical = schema::write(source);

  SUBCASE("magic") {
    auto bytes = canonical;
    bytes[0] ^= std::byte{1};
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::bad_magic);
  }

  SUBCASE("format") {
    auto bytes = canonical;
    patch_u32(bytes, 4, schema::format_version + 1);
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::unsupported_format);
  }

  SUBCASE("section version") {
    auto bytes = canonical;
    patch_u32(bytes, 24, actor_section::version + 1);
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::version_mismatch);
    CHECK(result.section_id == actor_section::id);
    CHECK(result.expected_version == actor_section::version);
    CHECK(result.actual_version == actor_section::version + 1);
  }

  SUBCASE("header schema digest") {
    auto bytes = canonical;
    bytes[8] ^= std::byte{1};
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::schema_digest_mismatch);
  }
}

TEST_CASE("network state schema rejects section and document trailing bytes") {
  const fake_host source{{17, 90}, 0, 0};
  const auto canonical = schema::write(source);

  SUBCASE("inside section") {
    auto bytes = canonical;
    patch_u64(bytes, 28, 5);
    bytes.insert(bytes.begin() + 40, std::byte{0});
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::section_trailing_bytes);
    CHECK(result.section_id == actor_section::id);
  }

  SUBCASE("after document") {
    auto bytes = canonical;
    bytes.push_back(std::byte{0});
    fake_host destination{};
    const auto result = load(destination, bytes);
    CHECK(result.status == network::state_load_status::trailing_bytes);
  }
}

TEST_CASE("network state schema hash sink covers exactly checkpoint bytes") {
  const fake_host source{{17, 90}, 999, 4};
  const auto bytes = schema::write(source);
  hash_sink sink;
  schema::emit_canonical(source, sink);
  CHECK(sink.value == hash_bytes(bytes));
}
