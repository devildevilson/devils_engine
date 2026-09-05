#ifndef DEVILS_ENGINE_NETWORK_STATE_DIGEST_H
#define DEVILS_ENGINE_NETWORK_STATE_DIGEST_H

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <devils_engine/utils/sha256cpp.h>
#include <devils_engine/utils/type_traits.h>
#include "state_schema.h"

namespace devils_engine::network {

template <class Hasher>
concept state_digest_hasher =
  std::default_initializable<Hasher> &&
  requires(Hasher& hasher, const std::span<const std::byte> bytes) {
    typename Hasher::digest_type;
    { hasher.update(bytes) } -> std::same_as<void>;
    { hasher.finish() } -> std::same_as<typename Hasher::digest_type>;
  };

// Available reference policy, not a protocol decision. Projects may inject a
// faster non-cryptographic 64-bit hasher or a wider durable/keyed identity.
class sha256_state_hasher {
public:
  using digest_type = utils::digest;

  void update(const std::span<const std::byte> bytes) {
    if (bytes.empty()) return;
    value_.update(bytes.data(), bytes.size());
  }

  digest_type finish() {
    return value_.finalize();
  }

private:
  utils::SHA256 value_;
};

// Murmur64A is a one-shot utility, so this adapter buffers canonical chunks.
// It is the initial frequent diagnostic policy, not authentication or durable
// content identity. Projects that already materialize checkpoint bytes may hash
// those bytes directly and use this report builder only when section roots are
// requested after a mismatch.
class buffered_murmur64_state_hasher {
public:
  using digest_type = std::uint64_t;

  void update(const std::span<const std::byte> input) {
    if (input.empty()) return;
    bytes_.insert(bytes_.end(), input.begin(), input.end());
  }

  digest_type finish() {
    static constexpr char empty = '\0';
    const char* data = bytes_.empty() ? &empty : reinterpret_cast<const char*>(bytes_.data());
    return utils::murmur_hash64A(std::string_view{
      data, bytes_.size()});
  }

private:
  std::vector<std::byte> bytes_;
};

template <state_digest_hasher Hasher>
class canonical_digest_sink {
public:
  using digest_type = typename Hasher::digest_type;

  void u32(const std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
      bytes[i] = std::byte(std::uint8_t(value >> (i * 8)));
    }
    hasher_.update(bytes);
  }

  void u64(const std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) {
      bytes[i] = std::byte(std::uint8_t(value >> (i * 8)));
    }
    hasher_.update(bytes);
  }

  void bytes(const std::span<const std::byte> values) {
    hasher_.update(values);
  }

  digest_type finish() {
    return hasher_.finish();
  }

private:
  Hasher hasher_;
};

template <class Digest>
struct state_section_digest {
  std::uint32_t id = 0;
  std::uint32_t version = 0;
  std::uint64_t canonical_size = 0;
  Digest root{};

  bool operator==(const state_section_digest&) const = default;
};

template <class Digest>
struct state_digest_report {
  Digest root{};
  std::vector<state_section_digest<Digest>> sections;

  bool operator==(const state_digest_report&) const = default;
};

enum class state_digest_comparison_status : std::uint8_t {
  matched,
  root_only_mismatch,
  section_set_mismatch,
  section_mismatch
};

enum class state_digest_build_status : std::uint8_t {
  built,
  capacity_exceeded,
  invalid_document
};

// Prepared checkpoint bytes are already contiguous: hashing them directly
// avoids buffering the same document again in buffered_murmur64_state_hasher.
// Checks framing, NOT project section semantics; this is for locally emitted
// canonical bytes, not a substitute for Schema::load on foreign input.
// Refusals leave the report untouched. Reserve sections before the hot loop.
template <class Schema>
[[nodiscard]] state_digest_build_status try_murmur64_digest(
  const std::span<const std::byte> bytes,
  state_digest_report<std::uint64_t>& output) {
  state_reader reader{bytes};
  const auto magic = reader.u32();
  const auto format = reader.u32();
  const auto fingerprint = reader.u32();
  const auto count = reader.u32();
  if (!reader.good() || magic != Schema::magic || format != Schema::format_version ||
      fingerprint != Schema::schema_fingerprint() || count != Schema::section_count)
    return state_digest_build_status::invalid_document;
  if (count > output.sections.capacity()) return state_digest_build_status::capacity_exceeded;
  std::uint32_t previous = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto id = reader.u32();
    const auto version = reader.u32();
    const auto size = reader.u64();
    if (!reader.good() || (i != 0 && id <= previous) || version == 0 ||
        size > reader.size() - reader.position()) return state_digest_build_status::invalid_document;
    reader.take(std::size_t(size));
    previous = id;
  }
  if (reader.position() != reader.size()) return state_digest_build_status::invalid_document;

  const auto hash = [](const std::span<const std::byte> value) {
    return utils::murmur_hash64A(std::string_view{
      reinterpret_cast<const char*>(value.data()), value.size()});
  };
  reader = state_reader{bytes};
  reader.take(16);
  output.sections.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto start = reader.position();
    const auto id = reader.u32();
    const auto version = reader.u32();
    const auto size = reader.u64();
    reader.take(std::size_t(size));
    const auto frame = bytes.subspan(start, reader.position() - start);
    output.sections.push_back({id, version, std::uint64_t(frame.size()), hash(frame)});
  }
  output.root = hash(bytes);
  return state_digest_build_status::built;
}

struct state_digest_comparison {
  state_digest_comparison_status status = state_digest_comparison_status::matched;
  std::uint32_t section_id = 0;

  constexpr bool matched() const noexcept {
    return status == state_digest_comparison_status::matched;
  }
};

// The full root covers Schema::emit_canonical byte-for-byte. Each diagnostic
// section root covers its canonical [id, version, byte_size, payload] frame.
// Section reports are not a replacement for the full root and need not be sent
// until a root mismatch requires localization.
template <class Schema, state_digest_hasher Hasher, class Host>
[[nodiscard]] state_digest_report<typename Hasher::digest_type> make_state_digest(
  const Host& host) {
  using digest_type = typename Hasher::digest_type;
  state_digest_report<digest_type> result;
  result.sections.reserve(Schema::section_count);

  canonical_digest_sink<Hasher> full;
  Schema::emit_canonical(
    host, full,
    [&result](const std::uint32_t id, const std::uint32_t version,
              const std::span<const std::byte> payload) {
      canonical_digest_sink<Hasher> section;
      section.u32(id);
      section.u32(version);
      section.u64(std::uint64_t(payload.size()));
      section.bytes(payload);
      result.sections.push_back({
        id,
        version,
        std::uint64_t(16) + std::uint64_t(payload.size()),
        section.finish(),
      });
    });
  result.root = full.finish();
  return result;
}

// Reports produced by make_state_digest are in canonical section-ID order.
// A root mismatch with equal section roots means the document envelope differs;
// a set mismatch reports the first canonical ID present on only one side.
template <std::equality_comparable Digest>
[[nodiscard]] state_digest_comparison compare_state_digests(
  const state_digest_report<Digest>& expected,
  const state_digest_report<Digest>& actual) noexcept {
  if (expected.root == actual.root) return {};

  std::size_t expected_index = 0;
  std::size_t actual_index = 0;
  while (expected_index < expected.sections.size() &&
         actual_index < actual.sections.size()) {
    const auto& expected_section = expected.sections[expected_index];
    const auto& actual_section = actual.sections[actual_index];
    if (expected_section.id < actual_section.id) {
      return {state_digest_comparison_status::section_set_mismatch,
              expected_section.id};
    }
    if (actual_section.id < expected_section.id) {
      return {state_digest_comparison_status::section_set_mismatch,
              actual_section.id};
    }
    if (expected_section.version != actual_section.version ||
        expected_section.canonical_size != actual_section.canonical_size ||
        expected_section.root != actual_section.root) {
      return {state_digest_comparison_status::section_mismatch,
              expected_section.id};
    }
    ++expected_index;
    ++actual_index;
  }

  if (expected_index < expected.sections.size()) {
    return {state_digest_comparison_status::section_set_mismatch,
            expected.sections[expected_index].id};
  }
  if (actual_index < actual.sections.size()) {
    return {state_digest_comparison_status::section_set_mismatch,
            actual.sections[actual_index].id};
  }
  return {state_digest_comparison_status::root_only_mismatch};
}

} // namespace devils_engine::network

#endif
