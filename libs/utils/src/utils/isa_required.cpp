// Собирается С ФЛАГАМИ АРХИТЕКТУРЫ движка: только константа из предопределённых макросов, кода нет.
#include <cstdint>

#include "isa_guard.h"

namespace isa = devils_engine::utils::isa;

// Объявление со связыванием C отдельно: const в области пространства имён иначе внутренний.
extern "C" {
extern const uint32_t devils_isa_required_features;
}

const uint32_t devils_isa_required_features = 0u
#if defined(__SSE4_2__)
  | isa::sse4_2
#endif
#if defined(__POPCNT__)
  | isa::popcnt
#endif
#if defined(__AVX__)
  | isa::avx
#endif
#if defined(__AVX2__)
  | isa::avx2
#endif
#if defined(__FMA__)
  | isa::fma
#endif
#if defined(__F16C__)
  | isa::f16c
#endif
#if defined(__BMI__)
  | isa::bmi
#endif
#if defined(__BMI2__)
  | isa::bmi2
#endif
#if defined(__LZCNT__)
  | isa::lzcnt
#endif
#if defined(__AVX512F__)
  | isa::avx512f
#endif
#if defined(_MSC_VER) && !defined(__clang__) && defined(__AVX2__)
  // MSVC под /arch:AVX2 вправе генерировать FMA3, не объявляя __FMA__. FMA3 есть у каждого
  // процессора с AVX2, так что требование никого лишнего не отсекает.
  | isa::fma
#endif
  ;
