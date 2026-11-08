#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace devils_engine::utils {
uint32_t crc32c(const uint8_t* data, const size_t len) noexcept;
uint32_t crc32c(const std::span<const uint8_t>& data) noexcept;
uint32_t crc32c(const std::span<const char>& data) noexcept;
uint32_t crc32c(const std::span<uint8_t>& data) noexcept;
uint32_t crc32c(const std::span<char>& data) noexcept;
uint32_t crc32c(const std::string_view& data) noexcept;

} // namespace devils_engine::utils
