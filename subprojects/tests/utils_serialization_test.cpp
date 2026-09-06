#include <array>
#include <map>
#include <optional>
#include <vector>

#include <doctest/doctest.h>
#include <devils_engine/utils/serialization.h>
#include <devils_engine/utils/serialization_sink.h>
#include <devils_engine/utils/state_schema.h>

// Общий байтовый слой проверяется без зависимости на ECS или network. Отказ декодера
// оставляет произвольный кандидат; envelope, напротив, публикует оба результата только целиком.

namespace serial = devils_engine::utils::serial;

namespace {
struct document_state {
  std::uint64_t tick = 0;
  std::vector<std::uint32_t> values;
};

struct document_host {
  using staging_type = document_state;
  document_state state;
  bool refuse = false;
};

struct tick_section {
  static constexpr std::uint32_t id = 10;
  static constexpr std::uint32_t version = 1;
  static void write(const document_host& host, serial::writer& writer) {
    serial::serialize(writer, host.state.tick);
  }
  static bool read(document_state& state, serial::reader& reader) {
    serial::deserialize(reader, state.tick);
    return reader.good();
  }
  static bool validate(const document_state&) {
    return true;
  }
};

struct values_section {
  static constexpr std::uint32_t id = 20;
  static constexpr std::uint32_t version = 1;
  static void write(const document_host& host, serial::writer& writer) {
    if (host.refuse) writer.fail();
    serial::serialize(writer, host.state.values);
  }
  static bool read(document_state& state, serial::reader& reader) {
    serial::deserialize(reader, state.values);
    return reader.good();
  }
  static bool validate(const document_state&) {
    return true;
  }
};

using document_schema = serial::state_schema<document_host, serial::writer, serial::reader,
                                             values_section, tick_section>;
} // namespace

TEST_CASE("sectioned documents reuse prepared buffers and propagate write failure") {
  document_host host{{42, std::vector<std::uint32_t>(1024, 99)}};
  std::vector<std::byte> document, scratch;
  REQUIRE(document_schema::write(host, document, scratch));
  const auto expected = document_schema::write(host);
  CHECK(document == expected);
  const auto* document_address = document.data();
  const auto* scratch_address = scratch.data();
  REQUIRE(document_schema::write(host, document, scratch));
  REQUIRE(document_schema::try_write(host, document, scratch));
  CHECK(document == expected);
  CHECK(document.data() == document_address);
  CHECK(scratch.data() == scratch_address);
  host.state.values.resize(1);
  REQUIRE(document_schema::try_write(host, document, scratch));
  CHECK(document == document_schema::write(host));
  CHECK(document.size() < expected.size());
  CHECK(document.data() == document_address);
  CHECK(scratch.data() == scratch_address);
  CHECK_FALSE(document_schema::write(host, document, document));
  CHECK_FALSE(document_schema::try_write(host, document, document));

  host.refuse = true;
  CHECK_FALSE(document_schema::write(host, document, scratch));
  CHECK_FALSE(document_schema::try_write(host, document, scratch));
  CHECK(document_schema::write(host).empty());
  document.clear();
  serial::writer writer{document};
  CHECK_FALSE(document_schema::write(host, writer));
  CHECK_FALSE(writer.good());

  host.refuse = false;
  std::vector<std::byte> empty_scratch;
  std::size_t observed = 0;
  document.clear();
  serial::writer bounded{document, false};
  CHECK_FALSE(document_schema::try_emit_canonical(host, bounded, empty_scratch,
                                                  [&](std::uint32_t, std::uint32_t, std::span<const std::byte>) {
                                                    ++observed;
                                                  }));
  CHECK(observed == 0);
  CHECK_FALSE(bounded.good());
  std::vector<std::byte> no_capacity;
  serial::writer full{no_capacity, false};
  CHECK_FALSE(document_schema::emit_canonical(host, full));
  CHECK(no_capacity.empty());
}

TEST_CASE("codec appends canonical bytes and respects prepared capacity") {
  std::vector<std::byte> bytes{std::byte{0xaa}};
  serial::writer output{bytes};
  output.u16(0x1234);
  output.u32(0x89abcdef);
  const std::vector<std::byte> expected{
    std::byte{0xaa}, std::byte{0x34}, std::byte{0x12}, std::byte{0xef},
    std::byte{0xcd}, std::byte{0xab}, std::byte{0x89}};
  CHECK(bytes == expected);
  CHECK(output.position() == expected.size());
  bytes.resize(bytes.capacity());
  const auto capacity = bytes.capacity();
  const auto* address = bytes.data();
  serial::writer bounded{bytes, false};
  bounded.u8(1);
  CHECK_FALSE(bounded.good());
  bounded.u64(2);
  bounded.patch_u32(0, 0);
  CHECK(bytes.front() == std::byte{0xaa});
  CHECK(bytes.size() == capacity);
  CHECK(bytes.data() == address);
  serial::reader input{std::span<const std::byte>{expected}.subspan(1)};
  CHECK(input.u16() == 0x1234);
  CHECK(input.u32() == 0x89abcdef);
  CHECK(input.u8() == 0);
  CHECK_FALSE(input.good());
  CHECK(input.position() == input.size());
}

TEST_CASE("codec bounds zero-byte sequences nesting and presence flags") {
  struct empty {};
  std::vector<std::byte> bytes;
  serial::writer output{bytes};
  output.u64(UINT64_MAX);
  serial::reader huge{bytes};
  std::vector<empty> values;
  serial::deserialize(huge, values);
  CHECK_FALSE(huge.good());
  CHECK(values.empty());

  const std::array bad{std::byte{2}};
  serial::reader boolean{bad};
  bool value = false;
  serial::deserialize(boolean, value);
  CHECK_FALSE(boolean.good());
  serial::reader optional{bad};
  std::optional<int> number;
  serial::deserialize(optional, number);
  CHECK_FALSE(optional.good());

  bytes.clear();
  serial::writer nested_output{bytes};
  const std::vector<std::vector<int>> nested{{1}};
  serial::serialize(nested_output, nested);
  serial::reader nested_input{bytes, {.values = 10, .depth = 2}};
  std::vector<std::vector<int>> restored;
  serial::deserialize(nested_input, restored);
  CHECK_FALSE(nested_input.good());
}

TEST_CASE("generic envelope is transactional and bounded") {
  const std::vector<std::byte> raw(512, std::byte{0x42});
  const std::array<std::uint8_t, 3> screenshot{1, 2, 3};
  std::vector<std::byte> packet, scratch;
  REQUIRE(serial::seal(raw, packet, scratch, serial::disk_policy, screenshot));
  const auto* address = packet.data();
  const auto* scratch_address = scratch.data();
  REQUIRE(serial::seal(raw, packet, scratch, serial::disk_policy, screenshot));
  CHECK(packet.data() == address);
  CHECK(scratch.data() == scratch_address);
  std::vector<std::byte> decoded;
  std::vector<std::uint8_t> preview;
  REQUIRE(serial::unseal(packet, decoded, &preview, raw.size()));
  CHECK(decoded == raw);
  CHECK(preview == std::vector<std::uint8_t>(screenshot.begin(), screenshot.end()));
  for (std::size_t size = 0; size < packet.size(); ++size) {
    CHECK_FALSE(serial::unseal(std::span<const std::byte>{packet}.first(size), decoded, &preview));
    CHECK(decoded == raw);
    CHECK(preview.size() == screenshot.size());
  }
  CHECK_FALSE(serial::unseal(packet, decoded, &preview, raw.size() - 1));
  auto corrupt = packet;
  corrupt[24] ^= std::byte{1};
  CHECK_FALSE(serial::unseal(corrupt, decoded, &preview));
  CHECK(decoded == raw);
  CHECK(preview == std::vector<std::uint8_t>(screenshot.begin(), screenshot.end()));
  CHECK_FALSE(serial::seal(packet, packet, scratch));
  CHECK_FALSE(serial::seal(raw, scratch, scratch));
  const std::vector<std::byte> empty;
  REQUIRE(serial::unseal(serial::seal(empty), decoded, &preview));
  CHECK(decoded.empty());
  CHECK(preview.empty());
}
