// Собирается БЕЗ флагов архитектуры движка: исполняется раньше любого кода, собранного под AVX.
// Устройство и причины — isa_guard.h.
#include "isa_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cpuinfo.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#  define DEVILS_ISA_GUARD_X86 1
#else
#  define DEVILS_ISA_GUARD_X86 0
#endif

extern "C" {
extern const uint32_t devils_isa_required_features;
}

namespace devils_engine::utils::isa {

namespace {

struct named_feature {
  feature bit;
  const char* name;
};

constexpr named_feature feature_names[] = {
  {sse4_2, "sse4.2"}, {popcnt, "popcnt"}, {avx, "avx"},   {avx2, "avx2"},   {fma, "fma"},
  {f16c, "f16c"},     {bmi, "bmi"},       {bmi2, "bmi2"}, {lzcnt, "lzcnt"}, {avx512f, "avx512f"},
};

// Только после cpuinfo_initialize.
uint32_t detect() noexcept {
  uint32_t out = 0;
#if DEVILS_ISA_GUARD_X86
  // cpuinfo проверяет и CPUID, и XCR0: AVX без поддержки ОС для его регистров — всё равно отказ.
  if (cpuinfo_has_x86_sse4_2()) out |= sse4_2;
  if (cpuinfo_has_x86_popcnt()) out |= popcnt;
  if (cpuinfo_has_x86_avx()) out |= avx;
  if (cpuinfo_has_x86_avx2()) out |= avx2;
  if (cpuinfo_has_x86_fma3()) out |= fma;
  if (cpuinfo_has_x86_f16c()) out |= f16c;
  if (cpuinfo_has_x86_bmi()) out |= bmi;
  if (cpuinfo_has_x86_bmi2()) out |= bmi2;
  if (cpuinfo_has_x86_lzcnt()) out |= lzcnt;
  if (cpuinfo_has_x86_avx512f()) out |= avx512f;
#endif
  return out;
}

// Без std::string: сообщение пишется до инициализации стандартной библиотеки C++.
void append_names(char* buffer, const size_t size, const uint32_t features) noexcept {
  size_t used = std::strlen(buffer);
  for (const auto& entry : feature_names) {
    if ((features & entry.bit) == 0) {
      continue;
    }
    const int written = std::snprintf(buffer + used, size - used, "%s%s", used == 0 || buffer[used - 1] == ' ' ? "" : " ", entry.name);
    if (written < 0 || size_t(written) >= size - used) {
      return;
    }
    used += size_t(written);
  }
}

void run_guard() noexcept {
  const uint32_t need = devils_isa_required_features;
  if (need == 0) {
    return;
  }
  const char* test = std::getenv("DEVILS_ENGINE_ISA_GUARD_TEST");
  const bool pretend_missing = test != nullptr && std::strcmp(test, "missing") == 0;

  uint32_t have = 0;
  if (!pretend_missing) {
    if (!cpuinfo_initialize()) {
      std::fputs("devils_engine: cannot query the CPU, the instruction set check is skipped\n", stderr);
      return;
    }
    have = detect();
  }
  const uint32_t missing = need & ~have;
  if (missing == 0) {
    return;
  }

  char message[512] = "This build of devils_engine needs CPU instructions this processor lacks: ";
  append_names(message, sizeof(message), missing);
  const size_t used = std::strlen(message);
  std::snprintf(message + used, sizeof(message) - used,
                ".\nThe build requires: ");
  append_names(message, sizeof(message), need);
  const size_t tail = std::strlen(message);
  std::snprintf(message + tail, sizeof(message) - tail, ".\nUse the baseline package.\n");
  std::fputs(message, stderr);
  std::fflush(stderr);
#ifdef _WIN32
  // Игру запускают без консоли — stderr никто не увидит. В тесте окна нет: оно повесило бы CI.
  if (!pretend_missing) {
    MessageBoxA(nullptr, message, "devils_engine", MB_OK | MB_ICONERROR);
  }
#endif
  std::_Exit(3);
}

} // namespace

uint32_t required() noexcept {
  return devils_isa_required_features;
}

uint32_t available() noexcept {
  return cpuinfo_initialize() ? detect() : 0u;
}

std::string names(const uint32_t features) {
  char buffer[256] = "";
  append_names(buffer, sizeof(buffer), features);
  return buffer;
}

} // namespace devils_engine::utils::isa

// Точка входа раньше статических инициализаторов. Символ-якорь держит этот объектный файл в
// каждом исполняемом файле: на него ссылаются опции линковки devils_utils.
#if defined(_MSC_VER)
static void __cdecl devils_isa_guard_entry() {
  devils_engine::utils::isa::run_guard();
}
#  pragma section(".CRT$XCT", read)
extern "C" {
extern void(__cdecl* const devils_isa_guard_anchor)();
}
// .CRT$XCT идёт до .CRT$XCU, где лежат обычные инициализаторы C++.
__declspec(allocate(".CRT$XCT")) void(__cdecl* const devils_isa_guard_anchor)() = devils_isa_guard_entry;
#else
extern "C" [[gnu::used]] void devils_isa_guard_anchor() {}
// 101 — наивысший приоритет, доступный программе (0–100 зарезервированы реализацией).
[[gnu::constructor(101)]] static void devils_isa_guard_entry() {
  devils_engine::utils::isa::run_guard();
}
#endif
