#ifndef DEVILS_ENGINE_NETWORK_STATE_SCHEMA_H
#define DEVILS_ENGINE_NETWORK_STATE_SCHEMA_H

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <devils_engine/utils/hash.h>

namespace devils_engine::network {

// Minimal canonical little-endian byte adapters. A project may substitute
// compatible Writer/Reader types in state_schema without making this library
// depend on its serializer.
class state_writer {
public:
  explicit state_writer(std::vector<std::byte>& bytes) noexcept : bytes_(bytes) {}

  std::size_t position() const noexcept {
    return bytes_.size();
  }

  void u32(const std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
      bytes_.push_back(std::byte(std::uint8_t(value >> (i * 8))));
    }
  }

  void u64(const std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
      bytes_.push_back(std::byte(std::uint8_t(value >> (i * 8))));
    }
  }

  void bytes(const std::span<const std::byte> values) {
    bytes_.insert(bytes_.end(), values.begin(), values.end());
  }

private:
  std::vector<std::byte>& bytes_;
};

class state_reader {
public:
  explicit state_reader(const std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

  bool good() const noexcept {
    return good_;
  }

  std::size_t position() const noexcept {
    return position_;
  }

  std::size_t size() const noexcept {
    return bytes_.size();
  }

  std::uint32_t u32() noexcept {
    if (!need(4)) return 0;
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
      value |= std::uint32_t(std::to_integer<std::uint8_t>(bytes_[position_ + i])) << (i * 8);
    }
    position_ += 4;
    return value;
  }

  std::uint64_t u64() noexcept {
    if (!need(8)) return 0;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
      value |= std::uint64_t(std::to_integer<std::uint8_t>(bytes_[position_ + i])) << (i * 8);
    }
    position_ += 8;
    return value;
  }

  std::span<const std::byte> take(const std::size_t amount) noexcept {
    if (!need(amount)) return {};
    const auto result = bytes_.subspan(position_, amount);
    position_ += amount;
    return result;
  }

private:
  bool need(const std::size_t amount) noexcept {
    if (amount > bytes_.size() - position_) {
      good_ = false;
      return false;
    }
    return true;
  }

  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
  bool good_ = true;
};

template <class Writer>
concept state_writer_like =
  std::constructible_from<Writer, std::vector<std::byte>&> &&
  requires(Writer& writer, const std::uint32_t u32, const std::uint64_t u64,
           const std::span<const std::byte> bytes) {
    { writer.position() } -> std::same_as<std::size_t>;
    { writer.u32(u32) } -> std::same_as<void>;
    { writer.u64(u64) } -> std::same_as<void>;
    { writer.bytes(bytes) } -> std::same_as<void>;
  };

template <class Reader>
concept state_reader_like =
  std::constructible_from<Reader, std::span<const std::byte>> &&
  requires(Reader& reader, const std::size_t amount) {
    { reader.good() } -> std::same_as<bool>;
    { reader.position() } -> std::same_as<std::size_t>;
    { reader.size() } -> std::same_as<std::size_t>;
    { reader.u32() } -> std::same_as<std::uint32_t>;
    { reader.u64() } -> std::same_as<std::uint64_t>;
    { reader.take(amount) } -> std::same_as<std::span<const std::byte>>;
  };

template <class Sink>
concept state_canonical_sink =
  requires(Sink& sink, const std::uint32_t u32, const std::uint64_t u64,
           const std::span<const std::byte> bytes) {
    { sink.u32(u32) } -> std::same_as<void>;
    { sink.u64(u64) } -> std::same_as<void>;
    { sink.bytes(bytes) } -> std::same_as<void>;
  };

enum class state_compatibility_policy : std::uint8_t {
  exact
};

enum class state_load_status : std::uint8_t {
  loaded,
  truncated_header,
  bad_magic,
  unsupported_format,
  missing_section,
  unknown_section,
  duplicate_section,
  non_canonical_order,
  version_mismatch,
  truncated_section_header,
  truncated_section_body,
  section_decode_failed,
  section_trailing_bytes,
  section_validation_failed,
  schema_fingerprint_mismatch,
  trailing_bytes,
  host_validation_failed
};

struct state_load_result {
  state_load_status status = state_load_status::loaded;
  std::uint32_t section_id = 0;
  std::uint32_t expected_version = 0;
  std::uint32_t actual_version = 0;

  constexpr bool loaded() const noexcept {
    return status == state_load_status::loaded;
  }
};

namespace detail {

template <class... Sections>
consteval bool unique_state_section_ids() {
  constexpr std::array<std::uint32_t, sizeof...(Sections)> ids{
    std::uint32_t(Sections::id)...};
  for (std::size_t i = 0; i < ids.size(); ++i) {
    for (std::size_t j = i + 1; j < ids.size(); ++j) {
      if (ids[i] == ids[j]) return false;
    }
  }
  return true;
}

template <std::size_t Size>
consteval void schema_append_u32(
  std::array<std::byte, Size>& bytes,
  std::size_t& position,
  const std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    bytes[position++] = std::byte(std::uint8_t(value >> (i * 8)));
  }
}

} // namespace detail

template <class... Sections>
inline constexpr bool unique_state_section_ids_v = detail::unique_state_section_ids<Sections...>();

// A Section owns stable id/version metadata and three project operations:
//   write(const Host&, Writer&)
//   read(Host::staging_type&, Reader&) -> bool
//   validate(const Host::staging_type&) -> bool
// write must be a deterministic read-only projection; read and validate must
// not publish effects outside the candidate staging state.
// Cross-section validation and the final whole-host replacement are supplied
// to load() as callables. Replacement is required to be noexcept: all foreign
// bytes and all validation are handled before the one live-state mutation.
template <class Host, state_writer_like Writer, state_reader_like Reader, class... Sections>
class state_schema {
public:
  using host_type = Host;
  using staging_type = typename Host::staging_type;

  static constexpr std::uint32_t magic = UINT32_C(0x4e535430); // "NST0"
  static constexpr std::uint32_t format_version = 1;
  static constexpr state_compatibility_policy compatibility = state_compatibility_policy::exact;
  static constexpr std::size_t section_count = sizeof...(Sections);

private:
  enum class section_decode_status : std::uint8_t {
    decoded,
    failed,
    trailing_bytes
  };

  using write_fn = void (*)(const Host&, Writer&);
  using read_fn = section_decode_status (*)(staging_type&, std::span<const std::byte>);
  using validate_fn = bool (*)(const staging_type&);

  struct descriptor {
    std::uint32_t id;
    std::uint32_t version;
    write_fn write;
    read_fn read;
    validate_fn validate;
  };

  template <class Section>
  static void write_section(const Host& host, Writer& writer) {
    Section::write(host, writer);
  }

  template <class Section>
  static section_decode_status read_section(
    staging_type& staging,
    const std::span<const std::byte> bytes) {
    Reader reader{bytes};
    if (!Section::read(staging, reader) || !reader.good()) {
      return section_decode_status::failed;
    }
    return reader.position() == reader.size()
             ? section_decode_status::decoded
             : section_decode_status::trailing_bytes;
  }

  template <class Section>
  static bool validate_section(const staging_type& staging) {
    return Section::validate(staging);
  }

  template <class Section>
  static consteval descriptor make_descriptor() {
    return {
      std::uint32_t(Section::id),
      std::uint32_t(Section::version),
      &write_section<Section>,
      &read_section<Section>,
      &validate_section<Section>,
    };
  }

  static consteval auto make_descriptors() {
    std::array<descriptor, section_count> result{make_descriptor<Sections>()...};
    for (std::size_t i = 0; i < result.size(); ++i) {
      for (std::size_t j = i + 1; j < result.size(); ++j) {
        if (result[j].id < result[i].id) std::swap(result[i], result[j]);
      }
    }
    return result;
  }

  inline static constexpr auto descriptors_ = make_descriptors();

  static consteval std::uint32_t make_schema_fingerprint() {
    std::array<std::byte, (2 + section_count * 2) * sizeof(std::uint32_t)> bytes{};
    std::size_t position = 0;
    detail::schema_append_u32(bytes, position, format_version);
    detail::schema_append_u32(bytes, position, std::uint32_t(section_count));
    for (const descriptor& value : descriptors_) {
      detail::schema_append_u32(bytes, position, value.id);
      detail::schema_append_u32(bytes, position, value.version);
    }
    return utils::murmur_hash3_32(std::span<const std::byte>{bytes});
  }

public:
  static_assert(section_count > 0, "network::state_schema requires at least one section");
  static_assert(section_count <= std::numeric_limits<std::uint32_t>::max(),
                "network::state_schema has too many sections");
  static_assert(unique_state_section_ids_v<Sections...>,
                "network::state_schema section IDs must be unique");
  static_assert(((std::uint32_t(Sections::version) != 0) && ...),
                "network::state_schema section versions start at one");
  static_assert((requires(const Host& host, Writer& writer, staging_type& staging,
                          const staging_type& const_staging, Reader& reader) {
                  { Sections::write(host, writer) } -> std::same_as<void>;
                  { Sections::read(staging, reader) } -> std::same_as<bool>;
                  { Sections::validate(const_staging) } -> std::same_as<bool>;
                } &&
                 ...),
                "network::state_schema section does not satisfy its policy contract");

  static constexpr std::uint32_t schema_fingerprint() noexcept {
    return make_schema_fingerprint();
  }

  // This is the single canonical traversal. A byte writer produces checkpoint
  // bytes; a future state hasher can implement the same sink operations and
  // therefore covers exactly the same framing and payload.
  template <state_canonical_sink Sink>
  static void emit_canonical(const Host& host, Sink& sink) {
    sink.u32(magic);
    sink.u32(format_version);
    sink.u32(schema_fingerprint());
    sink.u32(std::uint32_t(section_count));

    for (const descriptor& value : descriptors_) {
      std::vector<std::byte> payload;
      Writer payload_writer{payload};
      value.write(host, payload_writer);
      payload.resize(payload_writer.position());

      sink.u32(value.id);
      sink.u32(value.version);
      sink.u64(std::uint64_t(payload.size()));
      sink.bytes(payload);
    }
  }

  static void write(const Host& host, Writer& writer) {
    emit_canonical(host, writer);
  }

  [[nodiscard]] static std::vector<std::byte> write(const Host& host) {
    std::vector<std::byte> result;
    Writer writer{result};
    write(host, writer);
    result.resize(writer.position());
    return result;
  }

  template <class Validate, class Replace>
    requires std::predicate<Validate, const staging_type&> &&
             std::is_nothrow_invocable_r_v<void, Replace, Host&, staging_type&&>
  [[nodiscard]] static state_load_result load(
    Host& live,
    Reader& reader,
    staging_type staging,
    Validate&& validate,
    Replace&& replace) {
    const std::uint32_t input_magic = reader.u32();
    const std::uint32_t input_format = reader.u32();
    const std::uint32_t input_fingerprint = reader.u32();
    const std::uint32_t input_count = reader.u32();
    if (!reader.good()) return {state_load_status::truncated_header};
    if (input_magic != magic) return {state_load_status::bad_magic};
    if (input_format != format_version) return {state_load_status::unsupported_format};

    std::size_t expected_index = 0;
    std::uint32_t previous_id = 0;
    bool have_previous = false;
    for (std::uint32_t input_index = 0; input_index < input_count; ++input_index) {
      const std::uint32_t id = reader.u32();
      const std::uint32_t version = reader.u32();
      const std::uint64_t byte_size = reader.u64();
      if (!reader.good()) {
        return {state_load_status::truncated_section_header, id};
      }
      if (have_previous && id == previous_id) {
        return {state_load_status::duplicate_section, id};
      }
      if (have_previous && id < previous_id) {
        return {state_load_status::non_canonical_order, id};
      }
      have_previous = true;
      previous_id = id;

      if (expected_index == descriptors_.size()) {
        return {state_load_status::unknown_section, id};
      }
      const descriptor& expected = descriptors_[expected_index];
      if (id < expected.id) {
        return {state_load_status::unknown_section, id};
      }
      if (id > expected.id) {
        return {state_load_status::missing_section, expected.id, expected.version, 0};
      }
      if (version != expected.version) {
        return {state_load_status::version_mismatch, id, expected.version, version};
      }
      if (byte_size > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
        return {state_load_status::truncated_section_body, id};
      }
      const auto payload = reader.take(std::size_t(byte_size));
      if (!reader.good()) {
        return {state_load_status::truncated_section_body, id};
      }

      const section_decode_status decoded = expected.read(staging, payload);
      if (decoded == section_decode_status::failed) {
        return {state_load_status::section_decode_failed, id};
      }
      if (decoded == section_decode_status::trailing_bytes) {
        return {state_load_status::section_trailing_bytes, id};
      }
      ++expected_index;
    }

    if (expected_index != descriptors_.size()) {
      const descriptor& missing = descriptors_[expected_index];
      return {state_load_status::missing_section, missing.id, missing.version, 0};
    }
    if (input_fingerprint != schema_fingerprint()) {
      return {state_load_status::schema_fingerprint_mismatch};
    }
    if (reader.position() != reader.size()) {
      return {state_load_status::trailing_bytes};
    }

    for (const descriptor& value : descriptors_) {
      if (!value.validate(staging)) {
        return {state_load_status::section_validation_failed, value.id};
      }
    }
    if (!std::invoke(std::forward<Validate>(validate), std::as_const(staging))) {
      return {state_load_status::host_validation_failed};
    }

    std::invoke(std::forward<Replace>(replace), live, std::move(staging));
    return {};
  }
};

} // namespace devils_engine::network

#endif
