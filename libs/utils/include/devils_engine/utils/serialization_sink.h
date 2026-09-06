#ifndef DEVILS_ENGINE_UTILS_SERIALIZATION_SINK_H
#define DEVILS_ENGINE_UTILS_SERIALIZATION_SINK_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "compression.h"

// Transport/storage envelope around an already canonical payload. It owns compression, checksum
// and optional opaque preview bytes, but has no knowledge of the document or domain schema inside.

namespace devils_engine::utils::serial {

constexpr std::uint32_t container_magic = UINT32_C(0xDE5AC001);
constexpr std::uint16_t container_version = 1;

struct sink_policy {
  compression_level level;
  bool embed_screenshot;
};

inline constexpr sink_policy disk_policy{compression_level::normal, true};
inline constexpr sink_policy network_policy{compression_level::fast, false};

[[nodiscard]] bool seal(std::span<const std::byte> payload,
                        std::vector<std::byte>& output,
                        std::vector<std::byte>& compression_scratch,
                        const sink_policy& policy = disk_policy,
                        std::span<const std::uint8_t> screenshot = {});

[[nodiscard]] std::vector<std::byte> seal(
  std::span<const std::byte> payload,
  const sink_policy& policy = disk_policy,
  std::span<const std::uint8_t> screenshot = {});

[[nodiscard]] bool unseal(std::span<const std::byte> data,
                          std::vector<std::byte>& raw_output,
                          std::vector<std::uint8_t>* screenshot_output = nullptr,
                          std::size_t maximum_raw_size = 256 * 1024 * 1024);

} // namespace devils_engine::utils::serial

#endif
