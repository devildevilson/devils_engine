#include "prng.h"

#include "type_traits.h"

// надеюсь я все правильно записал и у меня не будет глупых проблем со случацными числами

namespace devils_engine {
  namespace utils {
    double prng_normalize(const uint64_t value) noexcept {
      union { uint64_t i; double d; } u;
      u.i = (UINT64_C(0x3FF) << 52) | (value >> 12);
      return u.d - 1.0;
    }
    
    float prng_normalizef(const uint32_t value) noexcept {
      union { uint32_t i; float f; } u;
      const uint32_t float_mask = 0x7f << 23;
      u.i = float_mask | (value >> 9);
      return u.f - 1.0f;
    }
    
    template <typename... Args>
    static uint64_t mix_impl(Args&&... args) noexcept {
      const uint64_t arr[] = { std::forward<Args>(args)... };
      const size_t size = sizeof(arr) / sizeof(arr[0]);
      const auto input = std::string_view(reinterpret_cast<const char*>(arr), sizeof(uint64_t) * size);
      return utils::murmur_hash64A(input, splitmix(arr[0]));
    }

    uint64_t mix(const uint64_t v1) noexcept {
      return wyhash64(v1);
    }

    uint64_t mix(const uint64_t v1, const uint64_t v2) noexcept {
      return mix_splitmix(v1, v2);
    }

    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3) noexcept {
      return mix_impl(v1, v2, v3);
    }

    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4) noexcept {
      return mix_impl(v1, v2, v3, v4);
    }

    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5) noexcept {
      return mix_impl(v1, v2, v3, v4, v5);
    }

    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5, const uint64_t v6) noexcept {
      return mix_impl(v1, v2, v3, v4, v5, v6);
    }

    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5, const uint64_t v6, const uint64_t v7) noexcept {
      return mix_impl(v1, v2, v3, v4, v5, v6, v7);
    }

    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5, const uint64_t v6, const uint64_t v7, const uint64_t v8) noexcept {
      return mix_impl(v1, v2, v3, v4, v5, v6, v7, v8);
    }

    uint64_t mix_hash(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4) noexcept {
      const uint64_t arr[] = { v1, v2, v3, v4 };
      const size_t size = sizeof(arr) / sizeof(arr[0]);
      const auto input = std::string_view(reinterpret_cast<const char*>(arr), sizeof(uint64_t) * size);
      return utils::murmur_hash64A(input, splitmix(v1));
    }

    uint64_t mix_splitmix(const uint64_t v1, const uint64_t v2) noexcept {
      return splitmix(v1, v2);
    }

    uint64_t mix_splitmix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4) noexcept {
      return mix_splitmix(mix_splitmix(v1, v2), mix_splitmix(v3, v4));
    }

    uint64_t mix_xoshiro1(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4) noexcept {
      xoshiro256starstar::state s{ { mix(v1), mix(v2), mix(v3), mix(v4) } };
      return xoshiro256starstar::value(xoshiro256starstar::next(s));
    }

    constexpr size_t mix_count = 10;
    uint64_t mix_xoshiro2(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4) noexcept {
      xoshiro256starstar::state s{ { v1, v2, v3, v4 } };
      for (size_t i = 0; i < mix_count; ++i) {
        s = xoshiro256starstar::next(s);
      }
      return xoshiro256starstar::value(s);
    }

    uint64_t mix_mulxor(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4) noexcept {
      uint64_t x = (v1 ^ (v2 << 1)) * 0x9e3779b97f4a7c15ULL;
      x ^= (v3 ^ (v4 << 3)) * 0xbf58476d1ce4e5b9ULL;
      x ^= x >> 33;
      x *= 0xff51afd7ed558ccdULL;
      return x ^ (x >> 29);
    }
    
    static inline uint64_t rotl(const uint64_t x, int k) {
      return (x << k) | (x >> (64 - k));
    }
    
    const size_t mulberry32::state_size;
    mulberry32::state mulberry32::init(const uint32_t seed) {
      return next(mulberry32::state{{seed}});
    }
      
    mulberry32::state mulberry32::next(state s) {
      return mulberry32::state{ { s.s[0] + 0x6D2B79F5 } };
    }
      
    uint32_t mulberry32::value(const state &s) {
      uint32_t z = s.s[0];
      z = (z ^ (z >> 15)) * (z | 1);
      z ^= z + (z ^ (z >> 7)) * (z | 61);
      return z ^ (z >> 14);
    }
    
    const size_t splitmix64::state_size;
    splitmix64::state splitmix64::init(const uint64_t seed) {
      return next(splitmix64::state{ { seed } });
    }
      
    splitmix64::state splitmix64::next(state s) {
      return splitmix64::state{ { s.s[0] + 0x9e3779b97f4a7c15 } };
    }
      
    uint64_t splitmix64::value(const state &s) {
      uint64_t z = s.s[0];
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
      z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
      return z ^ (z >> 31);
    }

    template <typename T>
    typename T::state typed_init(const uint64_t seed) {
      typename T::state new_state;
      const size_t state_size = T::state_size;
      splitmix64::state splitmix_states[state_size];
      splitmix_states[0] = splitmix64::next(splitmix64::state{ { seed } });
      for (size_t i = 1; i < state_size; ++i) splitmix_states[i] = splitmix64::next(splitmix_states[i-1]);
      for (size_t i = 0; i < state_size; ++i) new_state.s[i] = splitmix64::value(splitmix_states[i]);
      return new_state;
    }

    const size_t xorshift64::state_size;
    xorshift64::state xorshift64::init(const uint64_t seed) {
      return typed_init<xorshift64>(seed);
    }
      
    xorshift64::state xorshift64::next(state s) {
      s.s[0] ^= s.s[0] << 13;
      s.s[0] ^= s.s[0] >> 7;
      s.s[0] ^= s.s[0] << 17;
      return s;
    }
      
    uint64_t xorshift64::value(const state &s) {
      return s.s[0];
    }
    
    const size_t xoroshiro128plus::state_size;
    xoroshiro128plus::state xoroshiro128plus::init(const uint64_t seed) {
      return typed_init<xoroshiro128plus>(seed);
    }
      
    xoroshiro128plus::state xoroshiro128plus::next(state s) {
      const uint64_t s0 = s.s[0];
      uint64_t s1 = s.s[1];
      s1 ^= s0;
      s.s[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16); // a, b
      s.s[1] = rotl(s1, 37); // c
      return s;
    }
      
    uint64_t xoroshiro128plus::value(const state &s) {
      return s.s[0] + s.s[1];
    }
    
    const size_t xoroshiro128plusplus::state_size;
    xoroshiro128plusplus::state xoroshiro128plusplus::init(const uint64_t seed) {
      return typed_init<xoroshiro128plusplus>(seed);
    }
      
    xoroshiro128plusplus::state xoroshiro128plusplus::next(state s) {
      const uint64_t s0 = s.s[0];
      uint64_t s1 = s.s[1];
      s1 ^= s0;
      s.s[0] = rotl(s0, 49) ^ s1 ^ (s1 << 21); // a, b
      s.s[1] = rotl(s1, 28); // c
      return s;
    }
      
    uint64_t xoroshiro128plusplus::value(const state &s) {
      return rotl(s.s[0] + s.s[1], 17) + s.s[0];
    }
    
    const size_t xoroshiro128starstar::state_size;
    xoroshiro128starstar::state xoroshiro128starstar::init(const uint64_t seed) {
      return typed_init<xoroshiro128starstar>(seed);
    }
      
    xoroshiro128starstar::state xoroshiro128starstar::next(state s) {
      const uint64_t s0 = s.s[0];
      uint64_t s1 = s.s[1];
      s1 ^= s0;
      s.s[0] = rotl(s0, 24) ^ s1 ^ (s1 << 16); // a, b
      s.s[1] = rotl(s1, 37); // c
      return s;
    }
      
    uint64_t xoroshiro128starstar::value(const state &s) {
      return rotl(s.s[0] * 5, 7) * 9;
    }
    
    const size_t xoshiro256plus::state_size;
    xoshiro256plus::state xoshiro256plus::init(const uint64_t seed) {
      return typed_init<xoshiro256plus>(seed);
    }
      
    xoshiro256plus::state xoshiro256plus::next(state s) {
      const uint64_t t = s.s[1] << 17;
      s.s[2] ^= s.s[0];
      s.s[3] ^= s.s[1];
      s.s[1] ^= s.s[2];
      s.s[0] ^= s.s[3];
      s.s[2] ^= t;
      s.s[3] = rotl(s.s[3], 45);
      return s;
    }
      
    uint64_t xoshiro256plus::value(const state &s) {
      return s.s[0] + s.s[3];
    }
    
    const size_t xoshiro256plusplus::state_size;
    xoshiro256plusplus::state xoshiro256plusplus::init(const uint64_t seed) {
      return typed_init<xoshiro256plusplus>(seed);
    }
      
    xoshiro256plusplus::state xoshiro256plusplus::next(state s) {
      const uint64_t t = s.s[1] << 17;
      s.s[2] ^= s.s[0];
      s.s[3] ^= s.s[1];
      s.s[1] ^= s.s[2];
      s.s[0] ^= s.s[3];
      s.s[2] ^= t;
      s.s[3] = rotl(s.s[3], 45);
      return s;
    }
      
    uint64_t xoshiro256plusplus::value(const state &s) {
      return rotl(s.s[0] + s.s[3], 23) + s.s[0];
    }
    
    const size_t xoshiro256starstar::state_size;
    xoshiro256starstar::state xoshiro256starstar::init(const uint64_t seed) {
      return typed_init<xoshiro256starstar>(seed);
    }
      
    xoshiro256starstar::state xoshiro256starstar::next(state s) {
      const uint64_t t = s.s[1] << 17;
      s.s[2] ^= s.s[0];
      s.s[3] ^= s.s[1];
      s.s[1] ^= s.s[2];
      s.s[0] ^= s.s[3];
      s.s[2] ^= t;
      s.s[3] = rotl(s.s[3], 45);
      return s;
    }
      
    uint64_t xoshiro256starstar::value(const state &s) {
      return rotl(s.s[1] * 5, 7) * 9;
    }
    
    const size_t xoshiro512plus::state_size;
    xoshiro512plus::state xoshiro512plus::init(const uint64_t seed) {
      return typed_init<xoshiro512plus>(seed);
    }
      
    xoshiro512plus::state xoshiro512plus::next(state s) {
      const uint64_t t = s.s[1] << 11;
      s.s[2] ^= s.s[0];
      s.s[5] ^= s.s[1];
      s.s[1] ^= s.s[2];
      s.s[7] ^= s.s[3];
      s.s[3] ^= s.s[4];
      s.s[4] ^= s.s[5];
      s.s[0] ^= s.s[6];
      s.s[6] ^= s.s[7];
      s.s[6] ^= t;
      s.s[7] = rotl(s.s[7], 21);
      return s;
    }
      
    uint64_t xoshiro512plus::value(const state &s) {
      return s.s[0] + s.s[2];
    }
    
    const size_t xoshiro512plusplus::state_size;
    xoshiro512plusplus::state xoshiro512plusplus::init(const uint64_t seed) {
      return typed_init<xoshiro512plusplus>(seed);
    }
      
    xoshiro512plusplus::state xoshiro512plusplus::next(state s) {
      const uint64_t t = s.s[1] << 11;
      s.s[2] ^= s.s[0];
      s.s[5] ^= s.s[1];
      s.s[1] ^= s.s[2];
      s.s[7] ^= s.s[3];
      s.s[3] ^= s.s[4];
      s.s[4] ^= s.s[5];
      s.s[0] ^= s.s[6];
      s.s[6] ^= s.s[7];
      s.s[6] ^= t;
      s.s[7] = rotl(s.s[7], 21);
      return s;
    }
      
    uint64_t xoshiro512plusplus::value(const state &s) {
      return rotl(s.s[0] + s.s[2], 17) + s.s[2];
    }

    const size_t xoshiro512starstar::state_size;
    xoshiro512starstar::state xoshiro512starstar::init(const uint64_t seed) {
      return typed_init<xoshiro512starstar>(seed);
    }
      
    xoshiro512starstar::state xoshiro512starstar::next(state s) {
      const uint64_t t = s.s[1] << 11;
      s.s[2] ^= s.s[0];
      s.s[5] ^= s.s[1];
      s.s[1] ^= s.s[2];
      s.s[7] ^= s.s[3];
      s.s[3] ^= s.s[4];
      s.s[4] ^= s.s[5];
      s.s[0] ^= s.s[6];
      s.s[6] ^= s.s[7];
      s.s[6] ^= t;
      s.s[7] = rotl(s.s[7], 21);
      return s;
    }
      
    uint64_t xoshiro512starstar::value(const state &s) {
      return rotl(s.s[1] * 5, 7) * 9;
    }

    template <typename T>
    typename T::state typed_init_with_p(const uint64_t seed) {
      typename T::state new_state;
      splitmix64::state splitmix_states[T::state_size];
      splitmix_states[0] = splitmix64::next(splitmix64::state{ { seed } });
      for (size_t i = 1; i < T::state_size; ++i) splitmix_states[i] = splitmix64::next(splitmix_states[i-1]);
      for (size_t i = 0; i < T::state_size; ++i) new_state.s[i] = splitmix64::value(splitmix_states[i]);
      new_state.p = splitmix64::value(splitmix64::next(splitmix_states[T::state_size-1])) % T::state_size;
      return new_state;
    }
    
    const size_t xoroshiro1024star::state_size;
    xoroshiro1024star::state xoroshiro1024star::init(const uint64_t seed) {
      return typed_init_with_p<xoroshiro1024star>(seed);
    }
      
    xoroshiro1024star::state xoroshiro1024star::next(state s) {
      const int32_t q = s.p;
      s.p = (s.p + 1) & 15;
      const uint64_t s0 = s.s[s.p];
      uint64_t s15 = s.s[q];

      s15 ^= s0;
      s.s[q] = rotl(s0, 25) ^ s15 ^ (s15 << 27);
      s.s[s.p] = rotl(s15, 36);

      return s;
    }
      
    uint64_t xoroshiro1024star::value(const state &s) {
      const uint64_t s0 = s.s[s.p];
      return s0 * 0x9e3779b97f4a7c13;
    }
    
    const size_t xoroshiro1024plusplus::state_size;
    xoroshiro1024plusplus::state xoroshiro1024plusplus::init(const uint64_t seed) {
      return typed_init_with_p<xoroshiro1024plusplus>(seed);
    }
      
    xoroshiro1024plusplus::state xoroshiro1024plusplus::next(state s) {
      const int32_t q = s.p;
      s.p = (s.p + 1) & 15;
      const uint64_t s0 = s.s[s.p];
      uint64_t s15 = s.s[q];

      s15 ^= s0;
      s.s[q] = rotl(s0, 25) ^ s15 ^ (s15 << 27);
      s.s[s.p] = rotl(s15, 36);

      return s;
    }
      
    uint64_t xoroshiro1024plusplus::value(const state &s) {
      const int32_t q = s.p;
      const int32_t p = (s.p + 1) & 15;
      const uint64_t s0 = s.s[p];
      const uint64_t s15 = s.s[q];
      return rotl(s0 + s15, 23) + s15;
    }
    
    const size_t xoroshiro1024starstar::state_size;
    xoroshiro1024starstar::state xoroshiro1024starstar::init(const uint64_t seed) {
      return typed_init_with_p<xoroshiro1024starstar>(seed);
    }
      
    xoroshiro1024starstar::state xoroshiro1024starstar::next(state s) {
      const int32_t q = s.p;
      s.p = (s.p + 1) & 15;
      const uint64_t s0 = s.s[s.p];
      uint64_t s15 = s.s[q];

      s15 ^= s0;
      s.s[q] = rotl(s0, 25) ^ s15 ^ (s15 << 27);
      s.s[s.p] = rotl(s15, 36);

      return s;
    }
      
    uint64_t xoroshiro1024starstar::value(const state &s) {
      const uint64_t s0 = s.s[s.p];
      return rotl(s0 * 5, 7) * 9;
    }
    
    static inline uint32_t pcg_rotr_32(uint32_t value, unsigned int rot) {
      return (value >> rot) | (value << ((- rot) & 31));
    }
    
    static inline uint64_t pcg_rotr_64(uint64_t value, unsigned int rot) {
      return (value >> rot) | (value << ((- rot) & 63));
    }

//     static inline __uint128_t pcg_rotr_128(__uint128_t value, unsigned int rot) {
//       return (value >> rot) | (value << ((- rot) & 127));
//     }
    
    static inline uint64_t pcg_output_rxs_m_xs_64_64(const uint64_t state) {
      const uint64_t word = ((state >> ((state >> 59u) + 5u)) ^ state) * 12605985483714917081ull;
      return (word >> 43u) ^ word;
    }
    
    static inline uint64_t pcg_output_xsl_rr_rr_64_64(uint64_t state) {
      uint32_t rot1 = (uint32_t)(state >> 59u);
      uint32_t high = (uint32_t)(state >> 32u);
      uint32_t low  = (uint32_t)state;
      uint32_t xored = high ^ low;
      uint32_t newlow  = pcg_rotr_32(xored, rot1);
      uint32_t newhigh = pcg_rotr_32(high, newlow & 31u);
      return (((uint64_t)newhigh) << 32u) | newlow;
    }

    #define PCG_DEFAULT_MULTIPLIER_64 6364136223846793005ULL
    #define PCG_DEFAULT_INCREMENT_64 1442695040888963407ULL
    
    const size_t pcg_rxs_m_xs64unique::state_size;
    pcg_rxs_m_xs64unique::state pcg_rxs_m_xs64unique::init(const uint64_t seed) {
      state s = {0U};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_rxs_m_xs64unique::state pcg_rxs_m_xs64unique::next(const state &s) {
      state new_s = s;
      new_s.s[0] = new_s.s[0] * PCG_DEFAULT_MULTIPLIER_64 + (uint64_t)(((intptr_t)&s) | 1u);
      return new_s;
    }
      
    uint64_t pcg_rxs_m_xs64unique::value(const state &s) {
      return pcg_output_rxs_m_xs_64_64(s.s[0]);
    }
    
    const size_t pcg_rxs_m_xs64setseq::state_size;
    pcg_rxs_m_xs64setseq::state pcg_rxs_m_xs64setseq::init(const uint64_t seed, const uint64_t initseq) {
      state s = {{0U}, (initseq << 1u) | 1u};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_rxs_m_xs64setseq::state pcg_rxs_m_xs64setseq::next(state s) {
      s.s[0] = s.s[0] * PCG_DEFAULT_MULTIPLIER_64 + s.inc;
      return s;
    }
      
    uint64_t pcg_rxs_m_xs64setseq::value(const state &s) {
      return pcg_output_rxs_m_xs_64_64(s.s[0]);
    }
    
    const size_t pcg_xsl_rr_rr64unique::state_size;
    pcg_xsl_rr_rr64unique::state pcg_xsl_rr_rr64unique::init(const uint64_t seed) {
      state s = {0U};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_xsl_rr_rr64unique::state pcg_xsl_rr_rr64unique::next(const state &s) {
      state new_s = s;
      new_s.s[0] = new_s.s[0] * PCG_DEFAULT_MULTIPLIER_64 + (uint64_t)(((intptr_t)&s) | 1u);
      return new_s;
    }
      
    uint64_t pcg_xsl_rr_rr64unique::value(const state &s) {
      return pcg_output_xsl_rr_rr_64_64(s.s[0]);
    }
    
    const size_t pcg_xsl_rr_rr64setseq::state_size;
    pcg_xsl_rr_rr64setseq::state pcg_xsl_rr_rr64setseq::init(const uint64_t seed, const uint64_t initseq) {
      state s = {{0U}, (initseq << 1u) | 1u};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_xsl_rr_rr64setseq::state pcg_xsl_rr_rr64setseq::next(state s) {
      s.s[0] = s.s[0] * PCG_DEFAULT_MULTIPLIER_64 + s.inc;
      return s;
    }
      
    uint64_t pcg_xsl_rr_rr64setseq::value(const state &s) {
      return pcg_output_xsl_rr_rr_64_64(s.s[0]);
    }

    const size_t cmwc::cycle;
    const size_t cmwc::c_max;
    void cmwc::init(state &s, const uint32_t seed) {
      auto rand_state = mulberry32::init({seed});
      for (size_t i = 0; i < cycle; ++i) {
        s.Q[i] = mulberry32::value(rand_state);
        rand_state = mulberry32::next(rand_state);
      }
        
      do {
        s.c = mulberry32::value(rand_state);
        rand_state = mulberry32::next(rand_state);
      } while (s.c >= c_max);
        
      s.i = cycle - 1;
    }
      
    uint32_t cmwc::value(state &s) {
      const uint64_t a = 18782;      // as Marsaglia recommends
      const uint32_t m = 0xfffffffe; // as Marsaglia recommends
      uint64_t t;
      uint32_t x;

      s.i = (s.i + 1) & (cycle - 1);
      t = a * s.Q[s.i] + s.c;
      /* Let c = t / 0xffffffff, x = t mod 0xffffffff */
      s.c = t >> 32;
      x = t + s.c;
      if (x < s.c) {
        x++;
        s.c++;
      }
        
      return s.Q[s.i] = m - x;
    }

#ifndef _WIN32

    #define PCG_128BIT_CONSTANT(high,low) ((((__uint128_t)high) << 64) + low)
    static inline __uint128_t pcg_output_rxs_m_xs_128_128(__uint128_t state) {
      const __uint128_t word = ((state >> ((state >> 122u) + 6u)) ^ state) * (PCG_128BIT_CONSTANT(17766728186571221404ULL, 12605985483714917081ULL));
      /* 327738287884841127335028083622016905945 */
      return (word >> 86u) ^ word; 
    }

    static inline __uint128_t pcg_output_xsl_rr_rr_128_128(__uint128_t state) {
      uint32_t rot1 = (uint32_t)(state >> 122u);
      uint64_t high = (uint64_t)(state >> 64u);
      uint64_t low  = (uint64_t)state;
      uint64_t xored = high ^ low;
      uint64_t newlow  = pcg_rotr_64(xored, rot1);
      uint64_t newhigh = pcg_rotr_64(high, newlow & 63u);
      return (((__uint128_t)newhigh) << 64u) | newlow;
    }
    
    #define PCG_DEFAULT_MULTIPLIER_128 PCG_128BIT_CONSTANT(2549297995355413924ULL,4865540595714422341ULL)
    #define PCG_DEFAULT_INCREMENT_128  PCG_128BIT_CONSTANT(6364136223846793005ULL,1442695040888963407ULL)
    
    pcg_rxs_m_xs128unique::state pcg_rxs_m_xs128unique::init(const uint128_t seed) {
      state s = {0U};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_rxs_m_xs128unique::state pcg_rxs_m_xs128unique::next(const state &s) {
      state new_s = s;
      new_s.s[0] = new_s.s[0] * PCG_DEFAULT_MULTIPLIER_128 + (uint128_t)(((intptr_t)&s) | 1u);
      return new_s;
    }
      
    uint128_t pcg_rxs_m_xs128unique::value(const state &s) {
      return pcg_output_rxs_m_xs_128_128(s.s[0]);
    }
    
    pcg_rxs_m_xs128setseq::state pcg_rxs_m_xs128setseq::init(const uint128_t seed, const uint128_t initseq) {
      state s = {{0U}, (initseq << 1u) | 1u};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_rxs_m_xs128setseq::state pcg_rxs_m_xs128setseq::next(state s) {
      s.s[0] = s.s[0] * PCG_DEFAULT_MULTIPLIER_128 + s.inc;
      return s;
    }
      
    uint128_t pcg_rxs_m_xs128setseq::value(const state &s) {
      return pcg_output_rxs_m_xs_128_128(s.s[0]);
    }
    
    pcg_xsl_rr_rr128unique::state pcg_xsl_rr_rr128unique::init(const uint128_t seed) {
      state s = {0U};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_xsl_rr_rr128unique::state pcg_xsl_rr_rr128unique::next(const state &s) {
      state new_s = s;
      new_s.s[0] = new_s.s[0] * PCG_DEFAULT_MULTIPLIER_128 + (uint128_t)(((intptr_t)&s) | 1u);
      return new_s;
    }
      
    uint128_t pcg_xsl_rr_rr128unique::value(const state &s) {
      return pcg_output_xsl_rr_rr_128_128(s.s[0]);
    }
    
    pcg_xsl_rr_rr128setseq::state pcg_xsl_rr_rr128setseq::init(const uint128_t seed, const uint128_t initseq) {
      state s = {{0U}, (initseq << 1u) | 1u};
      s = next(s);
      s.s[0] += seed;
      return next(s);
    }
      
    pcg_xsl_rr_rr128setseq::state pcg_xsl_rr_rr128setseq::next(state s) {
      s.s[0] = s.s[0] * PCG_DEFAULT_MULTIPLIER_128 + s.inc;
      return s;
    }
      
    uint128_t pcg_xsl_rr_rr128setseq::value(const state &s) {
      return pcg_output_xsl_rr_rr_128_128(s.s[0]);
    }
    
    cmwc128::state cmwc128::init(const uint64_t seed) {
      return typed_init<cmwc128>(seed);
    }
      
    cmwc128::state cmwc128::next(state s) {
      const uint128_t t = 0xff8fa3db04bb588e * (uint128_t)s.s[0] + s.s[1];
      s.s[0] = 0xd81fdde4eba3aae9 * (uint64_t)t;
      s.s[1] = (t + 0xadca32a7 * (uint128_t)s.s[0]) >> 64;
      return s;
    }
      
    uint64_t cmwc128::value(const state &s) {
      return s.s[0];
    }
    
    cmwc256::state cmwc256::init(const uint64_t seed) {
      return typed_init<cmwc256>();
    }
      
    cmwc256::state cmwc256::next(state s) {
      const uint128_t t = 0xff2a4b18846bbee2 * static_cast<uint128_t>(s.s[0]) + s.s[3];
      s.s[0] = s.s[1];
      s.s[1] = s.s[2];
      s.s[2] = 0x94d34db4cd59d099 * static_cast<uint64_t>(t);
      s.s[3] = (t + 0x96e36616f07c57 * static_cast<uint128_t>(s.s[2])) >> 64;
      return s;
    }
      
    uint64_t cmwc256::value(const state &s) {
      return s.s[2];
    }

#endif  // !_WIN32
  }
}