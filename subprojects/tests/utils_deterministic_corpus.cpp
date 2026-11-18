#include <bit>
#include <cstdint>
#include <cstdio>

#include "devils_engine/utils/deterministic_math.h"
#include "devils_engine/utils/deterministic_sort.h"

namespace {

void print_u32(const uint32_t value) {
  std::printf("%08x", static_cast<unsigned>(value));
}

struct record {
  uint32_t key;
  uint32_t identity;
};

} // namespace

int main(int argc, char**) {
  for (uint32_t i = 0; i < 4096; ++i) {
    const float angle = float(int32_t(i) - 2048 + argc) * (1.0f / 16.0f);
    const auto value = devils_engine::utils::deterministic::sin_cos(angle);
    print_u32(std::bit_cast<uint32_t>(value.sine));
    print_u32(std::bit_cast<uint32_t>(value.cosine));
  }

  // double-функции порта musl: большие углы проходят через __rem_pio2_large.
  for (uint32_t i = 0; i < 1024; ++i) {
    const double x = double(int32_t(i) - 512 + argc) * (1.0 / 8.0);
    const double values[] = {
      devils_engine::utils::deterministic::sin(x),
      devils_engine::utils::deterministic::cos(x * 65536.0 * 1024.0),
      devils_engine::utils::deterministic::tan(x),
      devils_engine::utils::deterministic::asin(x / 64.0),
      devils_engine::utils::deterministic::acos(x / 64.0),
      devils_engine::utils::deterministic::atan(x),
      devils_engine::utils::deterministic::atan2(x, double(argc) - x),
      devils_engine::utils::deterministic::exp(x / 4.0),
      devils_engine::utils::deterministic::pow(1.0 + double(i) / 64.0, x / 16.0),
      devils_engine::utils::deterministic::hypot(x, double(i)),
    };
    for (const double value : values) {
      const uint64_t bits = std::bit_cast<uint64_t>(value);
      print_u32(uint32_t(bits >> 32));
      print_u32(uint32_t(bits));
    }
  }

  record records[97];
  for (uint32_t i = 0; i < 97; ++i) records[i] = {(i * 37u) % 11u, i};
  devils_engine::utils::deterministic_sort(
      records, records + 97, [](const record& left, const record& right) {
        return left.key < right.key;
      });
  for (const record value : records) {
    print_u32(value.key);
    print_u32(value.identity);
  }
  std::putchar('\n');
}
