#include "devils_engine/utils/serialization_sink.h"

#include <cstring>
#include <limits>
#include <string_view>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/serialization.h"
#include "devils_engine/utils/type_traits.h"

// The checksum covers raw canonical bytes, so compression level and preview attachment cannot alter
// checkpoint identity. Prepared output/scratch vectors retain their capacity between snapshots.

namespace devils_engine::utils::serial {
namespace {
constexpr std::uint8_t flag_screenshot = 0x1;
constexpr std::uint8_t flag_compressed = 0x2;

bool overlaps(const void* data, const std::size_t size, const std::vector<std::byte>& buffer) {
  if (size == 0 || buffer.capacity() == 0) return false;
  const auto start = reinterpret_cast<std::uintptr_t>(data);
  const auto target = reinterpret_cast<std::uintptr_t>(buffer.data());
  return start >= target ? start - target < buffer.capacity() : target - start < size;
}

std::uint64_t checksum_of(const std::byte* data, const std::size_t size) noexcept {
  return utils::murmur_hash64A(
    std::string_view(reinterpret_cast<const char*>(data), size));
}
} // namespace

bool seal(const std::span<const std::byte> raw, std::vector<std::byte>& output,
          std::vector<std::byte>& scratch, const sink_policy& policy,
          const std::span<const std::uint8_t> screenshot) {
  if (&output == &scratch) return false;
  if (overlaps(raw.data(), raw.size(), output) || overlaps(raw.data(), raw.size(), scratch) ||
      overlaps(screenshot.data(), screenshot.size(), output) ||
      overlaps(screenshot.data(), screenshot.size(), scratch)) return false;
  const bool with_screenshot = policy.embed_screenshot && !screenshot.empty();
  if (with_screenshot && (screenshot.size() > UINT32_MAX ||
                          screenshot.size() > SIZE_MAX - 36)) return false;

  scratch.resize(compress_safe_size(raw.size()));
  const std::size_t compressed_size = compress(
    reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size(),
    reinterpret_cast<std::uint8_t*>(scratch.data()), scratch.size(), policy.level);
  const bool use_compressed = compressed_size != SIZE_MAX && compressed_size < raw.size();
  if (use_compressed) scratch.resize(compressed_size);

  const std::size_t payload_size = use_compressed ? scratch.size() : raw.size();
  const std::size_t header_size = 32 + (with_screenshot ? 4 + screenshot.size() : 0);
  if (payload_size > std::numeric_limits<std::size_t>::max() - header_size) return false;

  output.clear();
  output.reserve(header_size + payload_size);
  writer writer{output};
  writer.u32(container_magic);
  writer.u16(container_version);
  writer.u8(std::uint8_t(policy.level));
  writer.u8(std::uint8_t((with_screenshot ? flag_screenshot : 0) |
                         (use_compressed ? flag_compressed : 0)));
  writer.u64(raw.size());
  writer.u64(payload_size);
  writer.u64(checksum_of(raw.data(), raw.size()));
  if (with_screenshot) {
    writer.u32(std::uint32_t(screenshot.size()));
    writer.raw(screenshot.data(), screenshot.size());
  }
  writer.bytes(use_compressed ? std::span<const std::byte>{scratch} : raw);
  return writer.good();
}

std::vector<std::byte> seal(const std::span<const std::byte> raw,
                            const sink_policy& policy,
                            const std::span<const std::uint8_t> screenshot) {
  std::vector<std::byte> output;
  std::vector<std::byte> scratch;
  if (!seal(raw, output, scratch, policy, screenshot)) return {};
  return output;
}

bool unseal(const std::span<const std::byte> data, std::vector<std::byte>& raw_output,
            std::vector<std::uint8_t>* screenshot_output, const std::size_t maximum_raw_size) {
  reader reader{data};
  const std::uint32_t magic = reader.u32();
  const std::uint16_t version = reader.u16();
  static_cast<void>(reader.u8());
  const std::uint8_t flags = reader.u8();
  const std::uint64_t raw_size = reader.u64();
  const std::uint64_t payload_size = reader.u64();
  const std::uint64_t checksum = reader.u64();
  if (!reader.good() || magic != container_magic || version != container_version ||
      raw_size > maximum_raw_size ||
      payload_size > std::uint64_t(std::numeric_limits<std::size_t>::max())) return false;
  if ((flags & ~(flag_screenshot | flag_compressed)) != 0) return false;

  std::vector<std::uint8_t> preview;
  if ((flags & flag_screenshot) != 0) {
    const std::uint32_t screenshot_size = reader.u32();
    const auto screenshot = reader.take(screenshot_size);
    if (!reader.good()) return false;
    if (screenshot_output != nullptr) {
      preview.resize(screenshot.size());
      if (!screenshot.empty())
        std::memcpy(preview.data(), screenshot.data(), screenshot.size());
    }
  }

  const auto payload = reader.take(std::size_t(payload_size));
  if (!reader.good() || reader.position() != reader.size()) return false;

  std::vector<std::byte> candidate;
  if ((flags & flag_compressed) != 0) {
    candidate.resize(decompress_safe_size(std::size_t(raw_size)));
    const std::size_t written = decompress(
      reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size(),
      reinterpret_cast<std::uint8_t*>(candidate.data()), candidate.size());
    if (written == SIZE_MAX || written != raw_size) return false;
  } else {
    if (payload.size() != raw_size) return false;
    candidate.assign(payload.begin(), payload.end());
  }
  if (checksum_of(candidate.data(), candidate.size()) != checksum) return false;
  raw_output = std::move(candidate);
  if (screenshot_output != nullptr) *screenshot_output = std::move(preview);
  return true;
}

} // namespace devils_engine::utils::serial
