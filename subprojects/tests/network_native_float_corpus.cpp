#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

template <class UInt>
void print_little_endian(const UInt value) {
  for (size_t i = 0; i < sizeof(UInt); ++i) {
    std::printf("%02x", static_cast<unsigned>((value >> (i * 8)) & UInt{0xff}));
  }
}

void print_float(const float value) {
  print_little_endian(std::bit_cast<uint32_t>(value));
}

void print_double(const double value) {
  print_little_endian(std::bit_cast<uint64_t>(value));
}

} // namespace

int main(int argc, char**) {
  // argc keeps the initial state a runtime input, while the test always invokes
  // the executable identically. This corpus intentionally uses ordinary native
  // expressions and several libm functions; it is an observation, not a claim
  // that the transition is portable.
  const double seed = static_cast<double>(argc);
  double px = 1234.125 + seed * 0.03125;
  double py = -987.75 + seed * 0.015625;
  float vx = 17.125f;
  float vy = -9.75f;
  float angle = 0.3125f;

  for (uint32_t tick = 0; tick < 384; ++tick) {
    const double phase = static_cast<double>(tick) * 0.0137 + px * 0.00017 - py * 0.00011;
    const double wave = std::sin(phase) + 0.375 * std::cos(phase * 0.731);
    const double bearing = std::atan2(py + wave * 7.0, px - wave * 3.0);
    const double radius = std::hypot(px * 0.001, py * 0.001);
    const double damping = std::exp(-0.00021 * radius) + std::log1p(radius) * 0.00003;

    const float ax = static_cast<float>(wave * std::cos(bearing) - radius * 0.001);
    const float ay = static_cast<float>(wave * std::sin(bearing) + radius * 0.0007);
    vx = vx * static_cast<float>(damping) + ax * 0.0166666675f;
    vy = vy * static_cast<float>(damping) + ay * 0.0166666675f;
    px = px + static_cast<double>(vx) * 0.01666666753590107;
    py = py + static_cast<double>(vy) * 0.01666666753590107;
    angle = std::atan2(vy, vx);

    print_double(px);
    print_double(py);
    print_float(vx);
    print_float(vy);
    print_float(angle);
  }
  std::putchar('\n');
  return 0;
}
