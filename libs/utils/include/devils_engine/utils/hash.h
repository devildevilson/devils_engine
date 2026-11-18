#ifndef DEVILS_ENGINE_UTILS_HASH_H
#define DEVILS_ENGINE_UTILS_HASH_H

#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace devils_engine {
namespace utils {

// Примитивы хеширования. Никакого состояния, всё constexpr и потокобезопасно.
// Разделение по назначению:
//   - числовые миксеры (fmix32/fmix64/wyhash64/splitmix/hash_combine) — рассеять/сложить уже-числовые
//     ключи (биты состояния, id, кортежи полей);
//   - murmur_hash3_32 — хеш БАЙТОВОЙ ПОСЛЕДОВАТЕЛЬНОСТИ (в т.ч. compile-time, для
//     строковых id вроде catalogue и канонических бинарных метаданных).
// Для последовательного id строк есть string_hash (rapidhash) в string_id.h; murmur_hash64A остаётся
// в type_traits.h — он тесно завязан на type_id.

// --- числовые миксеры ---

// Финализатор murmur3 на 32 бита: рассеивает биты одного 32-битного слова.
[[nodiscard]] constexpr uint32_t fmix32(uint32_t h) noexcept {
  h ^= h >> 16;
  h *= 0x85ebca6bU;
  h ^= h >> 13;
  h *= 0xc2b2ae35U;
  h ^= h >> 16;
  return h;
}

// Финализатор murmur3 на 64 бита (fmix64): полные два раунда умножения — лавинообразность по всем битам.
[[nodiscard]] constexpr uint64_t fmix64(uint64_t h) noexcept {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

// Подмешать значение v в накопитель seed (вариант boost::hash_combine на 64-битной золотой
// константе). Ассоциативно по порядку — годится для последовательного складывания полей ключа.
[[nodiscard]] constexpr uint64_t hash_combine(const uint64_t seed, const uint64_t v) noexcept {
  return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

[[nodiscard]] constexpr uint64_t wyhash64(uint64_t x) noexcept {
  x ^= x >> 32;
  x *= 0xd6e8feb86659fd93ULL;
  x ^= x >> 32;
  x *= 0xd6e8feb86659fd93ULL;
  x ^= x >> 32;
  return x;
}

[[nodiscard]] constexpr uint64_t splitmix(uint64_t x) noexcept {
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x ^= (x >> 31);
  return x;
}

[[nodiscard]] constexpr uint64_t splitmix(uint64_t v1, uint64_t v2) noexcept {
  uint64_t x = v1 ^ (v2 + 0x9e3779b97f4a7c15ULL);
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x = x ^ (x >> 31);
  return x;
}

// --- murmur3 (x86, 32 бита) для байтовых последовательностей ---

namespace detail {
constexpr uint32_t rotl32(const uint32_t x, const int8_t r) noexcept {
  return (x << r) | (x >> (32 - r));
}

constexpr uint8_t byte_value(const char value) noexcept {
  return static_cast<uint8_t>(value);
}

constexpr uint8_t byte_value(const std::byte value) noexcept {
  return std::to_integer<uint8_t>(value);
}

template <class Bytes>
constexpr uint32_t get_block(const Bytes& key, const uint32_t index) noexcept {
  return (uint32_t(byte_value(key[index * sizeof(uint32_t) + 0])) << CHAR_BIT * 0) |
         (uint32_t(byte_value(key[index * sizeof(uint32_t) + 1])) << CHAR_BIT * 1) |
         (uint32_t(byte_value(key[index * sizeof(uint32_t) + 2])) << CHAR_BIT * 2) |
         (uint32_t(byte_value(key[index * sizeof(uint32_t) + 3])) << CHAR_BIT * 3);
}

template <class Bytes>
constexpr uint32_t get_tail(const Bytes& key, const uint32_t index) noexcept {
  return uint32_t(byte_value(key[index]));
}

template <class Bytes>
constexpr uint32_t murmur_hash3_x86_32(const Bytes& key, const uint32_t seed = 0) noexcept {
  const uint32_t len = uint32_t(key.size());
  const uint32_t nblocks = len / sizeof(uint32_t);

  uint32_t h1 = seed;

  const uint32_t c1 = 0xcc9e2d51U;
  const uint32_t c2 = 0x1b873593U;

  // body
  for (uint32_t i = 0; i < nblocks; ++i) {
    uint32_t k1 = get_block(key, i);

    k1 *= c1;
    k1 = rotl32(k1, 15);
    k1 *= c2;

    h1 ^= k1;
    h1 = rotl32(h1, 13);
    h1 = h1 * 5 + 0xe6546b64U;
  }

  // tail
  const uint32_t last_block_offset = nblocks * sizeof(uint32_t);
  uint32_t k1 = 0;

  switch (len & 3) {
    case 3: k1 ^= get_tail(key, last_block_offset + 2) << CHAR_BIT * 2; [[fallthrough]];
    case 2: k1 ^= get_tail(key, last_block_offset + 1) << CHAR_BIT * 1; [[fallthrough]];
    case 1:
      k1 ^= get_tail(key, last_block_offset + 0) << CHAR_BIT * 0;
      k1 *= c1;
      k1 = rotl32(k1, 15);
      k1 *= c2;
      h1 ^= k1;
  }

  // finalization
  h1 ^= len;
  h1 = fmix32(h1);

  return h1;
}
} // namespace detail

constexpr uint32_t default_murmur32_seed = 0x9747b28cU;
constexpr uint32_t murmur_hash3_32(const std::string_view& in_str, const uint32_t seed = default_murmur32_seed) noexcept {
  return detail::murmur_hash3_x86_32(in_str, seed);
}

constexpr uint32_t murmur_hash3_32(
  const std::span<const std::byte> bytes,
  const uint32_t seed = default_murmur32_seed) noexcept {
  return detail::murmur_hash3_x86_32(bytes, seed);
}

// --- хеш строки, совпадающий с libstdc++ ---

// Байт в байт `std::hash<std::string>` из libstdc++ на 64-битной платформе (_Hash_bytes: MurmurHash2
// по 8 байт, зерно 0xc70f6907), записанный явно. Стандарт алгоритм НЕ фиксирует: у MSVC это FNV-1a,
// и зерно шага originator, выведенное из имени через std::hash, на Windows было другим — земля
// frontier_online уходила под воду целиком. Существующие миры и эталоны посчитаны этим алгоритмом,
// поэтому он сохранён, а не заменён на string_hash: замена переписала бы каждый мир.
[[nodiscard]] constexpr uint64_t libstdcxx_string_hash(const std::string_view text) noexcept {
  constexpr uint64_t mul = (uint64_t(0xc6a4a793u) << 32) + uint64_t(0x5bd1e995u);
  constexpr auto shift_mix = [](const uint64_t v) { return v ^ (v >> 47); };
  // Слово читается little-endian, как unaligned_load на x86-64 и AArch64.
  const auto load = [&](const size_t at, const size_t count) {
    uint64_t result = 0;
    for (size_t i = count; i-- > 0;) result = (result << 8) + uint64_t(static_cast<unsigned char>(text[at + i]));
    return result;
  };

  const size_t len = text.size();
  const size_t aligned = len & ~size_t(7);
  uint64_t hash = uint64_t(0xc70f6907u) ^ (uint64_t(len) * mul);
  for (size_t at = 0; at != aligned; at += 8) {
    hash ^= shift_mix(load(at, 8) * mul) * mul;
    hash *= mul;
  }
  if ((len & 7) != 0) {
    hash ^= load(aligned, len & 7);
    hash *= mul;
  }
  hash = shift_mix(hash) * mul;
  return shift_mix(hash);
}

} // namespace utils
} // namespace devils_engine

#endif
