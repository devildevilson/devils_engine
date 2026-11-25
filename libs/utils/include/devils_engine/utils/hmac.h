#ifndef DEVILS_ENGINE_UTILS_HMAC_H
#define DEVILS_ENGINE_UTILS_HMAC_H

#include <array>
#include <cstddef>
#include <span>

#include "sha256cpp.h"

// HMAC-SHA256 (RFC 2104) над SHA-256 из utils. Конкретный примитив для внедряемых MAC-политик
// (`network::credential_mac_policy`): сама `network/credential.h` примитив не выбирает.
namespace devils_engine::utils {

inline constexpr size_t hmac_sha256_bytes = 32;
using hmac_sha256_tag = std::array<std::byte, hmac_sha256_bytes>;

[[nodiscard]] inline hmac_sha256_tag hmac_sha256(const std::span<const std::byte> key,
                                                 const std::span<const std::byte> message) {
  constexpr size_t block_bytes = 64;
  std::array<std::byte, block_bytes> padded{};
  if (key.size() > block_bytes) {
    SHA256 shrink;
    shrink.update(key.data(), key.size());
    const auto value = shrink.finalize();
    for (size_t i = 0; i < value.size(); ++i) padded[i] = std::byte(value[i]);
  } else {
    for (size_t i = 0; i < key.size(); ++i) padded[i] = key[i];
  }

  std::array<std::byte, block_bytes> inner_pad{};
  std::array<std::byte, block_bytes> outer_pad{};
  for (size_t i = 0; i < block_bytes; ++i) {
    const auto value = std::to_integer<unsigned char>(padded[i]);
    inner_pad[i] = std::byte(static_cast<unsigned char>(value ^ 0x36u));
    outer_pad[i] = std::byte(static_cast<unsigned char>(value ^ 0x5cu));
  }

  SHA256 inner;
  inner.update(inner_pad.data(), inner_pad.size());
  if (!message.empty()) inner.update(message.data(), message.size());
  const auto inner_digest = inner.finalize();

  SHA256 outer;
  outer.update(outer_pad.data(), outer_pad.size());
  outer.update(inner_digest.data(), inner_digest.size());
  const auto outer_digest = outer.finalize();

  hmac_sha256_tag result{};
  for (size_t i = 0; i < result.size(); ++i) result[i] = std::byte(outer_digest[i]);
  return result;
}

} // namespace devils_engine::utils

#endif
