#include "net/secure_random.h"

#include <algorithm>
#include <array>
#include <climits>

#include <openssl/err.h>
#include <openssl/rand.h>

#include <devils_engine/utils/core.h>

namespace frontier_online::net {

void secure_random_bytes(const std::span<std::byte> output) {
  // RAND_bytes принимает int: большие буферы режутся, хотя рукопожатию нужно 32 байта.
  size_t done = 0;
  while (done < output.size()) {
    const size_t chunk = std::min<size_t>(output.size() - done, size_t(INT_MAX));
    if (RAND_bytes(reinterpret_cast<unsigned char*>(output.data() + done), int(chunk)) != 1) {
      devils_engine::utils::error{}(
        "frontier_online net: OpenSSL RAND_bytes refused {} bytes (error 0x{:x})", chunk,
        ERR_get_error());
    }
    done += chunk;
  }
}

uint64_t secure_random_nonzero_u64() {
  uint64_t value = 0;
  while (value == 0) {
    std::array<std::byte, 8> bytes{};
    secure_random_bytes(bytes);
    for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t(bytes[i]) << (8 * i);
  }
  return value;
}

} // namespace frontier_online::net
