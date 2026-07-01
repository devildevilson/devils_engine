// Демо/бенчмарк стоимости снапшота: размеры пейлоада в разных режимах компрессии + тайминги
// dump/pack/unpack. Не doctest — просто прогон с печатью таблицы.
//
//   ./aesthetics_serialization_benchmark [N ...]
//
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <vector>
#include <array>
#include <string>

#include <gtl/phmap.hpp>

#include "devils_engine/aesthetics/world.h"
#include "devils_engine/aesthetics/serialization.h"
#include "devils_engine/aesthetics/sink.h"

using namespace devils_engine;
using clk = std::chrono::steady_clock;

namespace {

// репрезентативный набор: фикс-массивы (transform), скаляры, строка, вектор+хеш-мапа.
struct b_transform { std::array<float, 3> pos; std::array<float, 4> rot; float scale; };
struct b_health    { int32_t hp; int32_t max; };
struct b_name      { std::string value; };
struct b_inventory { std::vector<int32_t> items; gtl::flat_hash_map<int32_t, int32_t> stacks; };

} // namespace

SERIALIZABLE_COMPONENT(b_transform)
SERIALIZABLE_COMPONENT(b_health)
SERIALIZABLE_COMPONENT(b_name)
SERIALIZABLE_COMPONENT(b_inventory)

namespace {

void populate(aesthetics::world& w, const size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const auto e = w.gen_entityid();
    const float f = float(i);
    w.create<b_transform>(e, b_transform{ {f, f * 2, f * 3}, {0, 0, 0, 1}, 1.0f });
    w.create<b_health>(e, b_health{ int32_t(i % 100), 100 });
    if (i % 2 == 0) w.create<b_name>(e, b_name{ "entity_" + std::to_string(i) });
    if (i % 4 == 0) {
      b_inventory inv;
      inv.items = { int32_t(i), int32_t(i + 1), int32_t(i + 2) };
      inv.stacks[int32_t(i % 7)] = 3;
      inv.stacks[int32_t(i % 5)] = 9;
      w.create<b_inventory>(e, std::move(inv));
    }
  }
}

template <typename F>
double ms(F&& f, const int reps) {
  const auto t0 = clk::now();
  for (int i = 0; i < reps; ++i) f();
  const auto t1 = clk::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
}

struct row { const char* mode; size_t bytes; double pack_ms; double unpack_ms; };

row measure(const aesthetics::world& w, const char* mode, aesthetics::serial::sink_policy pol, const int reps) {
  std::vector<uint8_t> packed;
  const double p = ms([&] { packed = aesthetics::serial::pack(&w, pol); }, reps);
  const double u = ms([&] { aesthetics::world tmp; aesthetics::serial::unpack(packed, &tmp); }, reps);
  return row{ mode, packed.size(), p, u };
}

void run(const size_t n) {
  aesthetics::world w;
  populate(w, n);

  // сырой размер снапшота (без контейнера/компрессии); dump_world(w) пред-размечает буфер.
  std::vector<std::byte> raw;
  const int reps = n >= 50000 ? 5 : 20;
  const double dump_ms = ms([&] { raw = aesthetics::serial::dump_world(&w); }, reps);
  const size_t raw_size = raw.size();

  using namespace aesthetics::serial;
  const row rows[] = {
    measure(w, "zstd fast(1)",   sink_policy{ utils::compression_level::fast,   false }, reps),
    measure(w, "zstd normal(3)", sink_policy{ utils::compression_level::normal, false }, reps),
    measure(w, "zstd high(12)",  sink_policy{ utils::compression_level::high,   false }, reps),
    measure(w, "zstd best(19)",  sink_policy{ utils::compression_level::best,   false }, reps),
  };

  std::printf("\n=== %zu entities ===\n", n);
  std::printf("  raw dump: %8zu B (%.1f KB)   dump %.3f ms\n", raw_size, raw_size / 1024.0, dump_ms);
  std::printf("  %-16s %10s %8s %10s %10s\n", "mode", "packed B", "ratio", "pack ms", "unpack ms");
  for (const auto& r : rows) {
    std::printf("  %-16s %10zu %7.2fx %10.3f %10.3f\n",
                r.mode, r.bytes, double(raw_size) / double(r.bytes), r.pack_ms, r.unpack_ms);
  }
}

} // namespace

int main(int argc, char** argv) {
  std::printf("snapshot cost: raw dump + sink(pack/unpack) across compression levels\n");
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) run(size_t(std::strtoull(argv[i], nullptr, 10)));
  } else {
    for (const size_t n : { size_t(1000), size_t(20000), size_t(100000) }) run(n);
  }
  return 0;
}
