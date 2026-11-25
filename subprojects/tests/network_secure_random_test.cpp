#include <array>
#include <cstddef>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/network/secure_random.h"

namespace net = devils_engine::network;

TEST_CASE("secure_random fills the whole buffer and does not repeat [network::secure_random]") {
  // Вероятность ложного провала — 2^-256 на сравнение: равные 32-байтные выборки значат сломанный
  // источник, а не невезение.
  std::array<std::byte, 32> a{}, b{};
  net::secure_random_bytes(a);
  net::secure_random_bytes(b);
  CHECK(a != b);
  CHECK(a != std::array<std::byte, 32>{});

  // Пустой буфер — ничего не делать, а не отказ.
  net::secure_random_bytes(std::span<std::byte>{});

  // Хвост длинного буфера тоже заполнен (не только первый кусок).
  std::vector<std::byte> large(4096);
  net::secure_random_bytes(large);
  bool tail_touched = false;
  for (size_t i = large.size() - 64; i < large.size(); ++i) tail_touched |= large[i] != std::byte{0};
  CHECK(tail_touched);

  CHECK(net::secure_random_nonzero_u64() != 0);
  CHECK(net::secure_random_nonzero_u64() != net::secure_random_nonzero_u64());
}
