#ifndef DEVILS_ENGINE_UTILS_PRNG_H
#define DEVILS_ENGINE_UTILS_PRNG_H

#include <cstddef>
#include <cstdint>

// реализация этих алгоритмов: http://prng.di.unimi.it/
// возможно мне потребуется джампинг оттуда

namespace devils_engine {
  namespace utils {
    double prng_normalize(const uint64_t value);  // или сделать через темлейт
    float prng_normalizef(const uint32_t value);

    // используем xoshiro
    // можно было бы сделать через шаблоны, но мне лень
    // ХОТЯ БЫ ОДНО ЧИСЛО ДОЛЖНО БЫТЬ НЕ 0
    uint64_t mix(const uint64_t v1, const uint64_t v2);
    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3);
    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4);
    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5);
    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5, const uint64_t v6);
    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5, const uint64_t v6, const uint64_t v7);
    uint64_t mix(const uint64_t v1, const uint64_t v2, const uint64_t v3, const uint64_t v4, const uint64_t v5, const uint64_t v6, const uint64_t v7, const uint64_t v8);

    struct mulberry32 {  // используется для инициализации
      static const size_t state_size = 1;
      struct state { 
        using outer = mulberry32;
        uint32_t s[state_size];
      };
      static state init(const uint32_t seed);
      static state next(state s);
      static uint32_t value(const state &s);
    };

    struct splitmix64 {  // можно использовать для инициализации стейтов других генераторов
      static const size_t state_size = 1;
      struct state { 
        using outer = splitmix64;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xorshift64 {
      static const size_t state_size = 1;
      struct state { 
        using outer = xorshift64;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoroshiro128plus {
      static const size_t state_size = 2;
      struct state { 
        using outer = xoroshiro128plus;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoroshiro128plusplus {
      static const size_t state_size = 2;
      struct state { 
        using outer = xoroshiro128plusplus;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoroshiro128starstar {
      static const size_t state_size = 2;
      struct state { 
        using outer = xoroshiro128starstar;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoshiro256plus {
      static const size_t state_size = 4;
      struct state { 
        using outer = xoshiro256plus;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoshiro256plusplus {
      static const size_t state_size = 4;
      struct state { 
        using outer = xoshiro256plusplus;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoshiro256starstar {
      static const size_t state_size = 4;
      struct state { 
        using outer = xoshiro256starstar;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoshiro512plus {
      static const size_t state_size = 8;
      struct state { 
        using outer = xoshiro512plus;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoshiro512plusplus {
      static const size_t state_size = 8;
      struct state { 
        using outer = xoshiro512plusplus;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoshiro512starstar {
      static const size_t state_size = 8;
      struct state { 
        using outer = xoshiro512starstar;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoroshiro1024star {
      static const size_t state_size = 16;
      struct state { 
        using outer = xoroshiro1024star;
        int32_t p; 
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoroshiro1024plusplus {
      static const size_t state_size = 16;
      struct state { 
        using outer = xoroshiro1024plusplus;
        int32_t p; 
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct xoroshiro1024starstar {
      static const size_t state_size = 16;
      struct state { 
        using outer = xoroshiro1024starstar;
        int32_t p; 
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    // вариант oneseq имеет плохие показатели для генератора (судя по форумам и
    // прочей информации в сети) unique варианты наверное вообще работать не будут в
    // текущем интерфейсе, хотя если сделать const& mcg вариант не определен для тех
    // функций которые я выбрал (rxs_m_xs и xsl_rr_rr) вообще pcg не выглядит каким
    // то уж слишком хорошим

    struct pcg_rxs_m_xs64unique {
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_rxs_m_xs64unique;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(const state &s);
      static uint64_t value(const state &s);
    };

    struct pcg_rxs_m_xs64setseq {
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_rxs_m_xs64setseq;
        uint64_t s[state_size]; 
        uint64_t inc; 
      };
      static state init(const uint64_t seed, const uint64_t initseq);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct pcg_xsl_rr_rr64unique {
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_xsl_rr_rr64unique;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(const state &s);
      static uint64_t value(const state &s);
    };

    struct pcg_xsl_rr_rr64setseq {
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_xsl_rr_rr64setseq;
        uint64_t s[state_size]; 
        uint64_t inc; 
      };
      static state init(const uint64_t seed, const uint64_t initseq);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    // 32 бита, иной интерфейс, зато очень большой период
    struct cmwc {
      static const size_t cycle = 4096;       // as Marsaglia recommends
      static const size_t c_max = 809430660;  // as Marsaglia recommends
      struct state {
        using outer = cmwc;
        uint32_t Q[cycle];
        uint32_t c;
        uint32_t i;
      };

      static void init(state &s, const uint32_t seed);
      static uint32_t value(state &s);
    };

#ifndef _WIN32
    // по идее 128 версия медленее чем 64, но имеет больший период
    struct pcg_rxs_m_xs128unique {
      using uint128_t = __uint128_t;
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_rxs_m_xs128unique;
        uint128_t s[state_size]; 
      };
      static state init(const uint128_t seed);
      static state next(const state &s);
      static uint128_t value(const state &s);
    };

    struct pcg_rxs_m_xs128setseq {
      using uint128_t = __uint128_t;
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_rxs_m_xs128setseq;
        uint128_t s[state_size]; 
        uint128_t inc; 
      };
      static state init(const uint128_t seed, const uint128_t initseq);
      static state next(state s);
      static uint128_t value(const state &s);
    };

    struct pcg_xsl_rr_rr128unique {
      using uint128_t = __uint128_t;
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_xsl_rr_rr128unique;
        uint128_t s[state_size]; 
      };
      static state init(const uint128_t seed);
      static state next(const state &ss);
      static uint128_t value(const state &s);
    };

    struct pcg_xsl_rr_rr128setseq {
      using uint128_t = __uint128_t;
      static const size_t state_size = 1;
      struct state { 
        using outer = pcg_xsl_rr_rr128setseq;
        uint128_t s[state_size]; 
        uint128_t inc; 
      };
      static state init(const uint128_t seed, const uint128_t initseq);
      static state next(state s);
      static uint128_t value(const state &s);
    };

    // используют __uint128_t, по идее скорость ниже, не уверен на счет
    // переносимости на windows не будет работать по всей видимости
    struct cmwc128 {
      using uint128_t = __uint128_t;
      static const size_t state_size = 2;
      struct state { 
        using outer = cmwc128;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };

    struct cmwc256 {
      using uint128_t = __uint128_t;
      static const size_t state_size = 4;
      struct state { 
        using outer = cmwc256;
        uint64_t s[state_size]; 
      };
      static state init(const uint64_t seed);
      static state next(state s);
      static uint64_t value(const state &s);
    };
#endif  // !_WIN32
  }  // namespace utils
}  // namespace devils_engine

#endif