#ifndef DEVILS_ENGINE_NETWORK_ENDPOINT_H
#define DEVILS_ENGINE_NETWORK_ENDPOINT_H

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

// Адрес «HOST:PORT» из командной строки. HOST — четыре десятичных октета (IPv4), порт 1..65535.
// Порядок байт хоста — как его ждёт SteamNetworkingIPAddr::SetIPv4 (старший октет первым). Всё, что
// разбор не понимает, — отказ: молча понятый не тот адрес потом ищут часами.

namespace devils_engine::network {

struct ipv4_endpoint {
  uint32_t host = 0;
  uint16_t port = 0;
};

[[nodiscard]] inline std::optional<uint16_t> parse_port(const std::string_view text) noexcept {
  uint32_t value = 0;
  const auto* last = text.data() + text.size();
  const auto result = std::from_chars(text.data(), last, value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != last) return std::nullopt;
  if (value == 0 || value > 65535) return std::nullopt;
  return uint16_t(value);
}

[[nodiscard]] inline std::optional<uint32_t> parse_ipv4(const std::string_view text) noexcept {
  uint32_t host = 0;
  size_t at = 0;
  for (unsigned i = 0; i < 4; ++i) {
    const auto dot = text.find('.', at);
    if (i < 3 && dot == std::string_view::npos) return std::nullopt;
    if (i == 3 && dot != std::string_view::npos) return std::nullopt;
    const auto end = (i == 3) ? text.size() : dot;
    const auto part = text.substr(at, end - at);
    if (part.empty() || part.size() > 3) return std::nullopt;
    uint32_t octet = 0;
    const auto* last = part.data() + part.size();
    const auto result = std::from_chars(part.data(), last, octet);
    if (result.ec != std::errc{} || result.ptr != last || octet > 255) return std::nullopt;
    host = (host << 8) | octet;
    at = end + 1;
  }
  return host;
}

[[nodiscard]] inline std::optional<ipv4_endpoint> parse_endpoint(const std::string_view text) noexcept {
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const auto host = parse_ipv4(text.substr(0, colon));
  const auto port = parse_port(text.substr(colon + 1));
  if (!host || !port) return std::nullopt;
  return ipv4_endpoint{*host, *port};
}

} // namespace devils_engine::network

#endif
