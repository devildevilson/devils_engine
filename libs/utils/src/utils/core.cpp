#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#  include <windows.h> //GetModuleFileNameW
#else
#  include <cpuid.h>
#  include <immintrin.h>
#  include <limits.h>
#  include <unistd.h> //readlink
#endif

#include <cpuinfo.h>

#include "core.h"
#include "utf.hpp"

namespace fs = std::filesystem;

namespace devils_engine {
namespace utils {
tracer::tracer(const std::source_location loc) noexcept : location(loc) {
  spdlog::log(spdlog::level::trace, "in  {}:{} `{}`", make_sane_file_name(location.file_name()), location.line(), location.function_name());
}

tracer::~tracer() noexcept {
  spdlog::log(spdlog::level::trace, "out {}:{} `{}`", make_sane_file_name(location.file_name()), location.line(), location.function_name());
}

std::string cast(const std::wstring& str) noexcept {
  const size_t s = wcstombs(nullptr, str.c_str(), 0);
  if (s == SIZE_MAX) {
    return std::string();
  }
  std::string ret(s, '\0');
  wcstombs(ret.data(), str.c_str(), ret.size());
  return ret;
}

std::string cast(const std::u16string_view& str) noexcept {
  return utf::as_str8(str);
}
std::string cast(const std::u32string_view& str) noexcept {
  return utf::as_str8(str);
}

std::wstring cast(const std::string& str) noexcept {
  const size_t s = mbstowcs(nullptr, str.c_str(), 0);
  if (s == SIZE_MAX) {
    return std::wstring();
  }
  std::wstring ret(s, '\0');
  mbstowcs(ret.data(), str.c_str(), ret.size());
  return ret;
}

std::u16string cast16(const std::string_view& str) noexcept {
  return utf::as_u16(str);
}
std::u32string cast32(const std::string_view& str) noexcept {
  return utf::as_u32(str);
}

std::string app_path() noexcept {
#ifdef _WIN32
  wchar_t path[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  const size_t s = wcstombs(nullptr, path, 0);
  if (s == SIZE_MAX) {
    return std::string();
  }
  std::string ret(s, '\0');
  wcstombs(ret.data(), path, ret.size());
  return fs::path(ret).generic_string();
#else
  char result[PATH_MAX];
  ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
  return std::string(result, (count > 0) ? count : 0);
#endif
}

std::string project_folder() noexcept {
  const auto str = app_path();
  // по идее уберем название файла + уберем папку bin
  const size_t first_slash = str.rfind('/');
  const auto bin_dir = std::string_view(str).substr(0, first_slash);
  return std::string(bin_dir.substr(0, bin_dir.rfind('/') + 1));
}

std::string cache_folder() noexcept {
  return project_folder() + "cache/";
}

std::string get_cpu_name() noexcept {
  std::string proc_name;
  cpuinfo_initialize();

  const auto ptr = cpuinfo_get_current_processor();
  if (ptr == nullptr) {
    const uint32_t count = cpuinfo_get_packages_count();
    if (count > 0) {
      const auto pack = cpuinfo_get_package(0);
      proc_name = pack->name;
    }
  } else {
    proc_name = ptr->package->name;
  }

  cpuinfo_deinitialize();
  return proc_name;
}

// CRC32C по Кастаньоли. Инструкция SSE4.2 и программный проход дают ОДНИ И ТЕ ЖЕ
// биты: полином определён стандартом, а `_mm_crc32_u64` над memcpy-загрузкой
// обрабатывает байты в том же порядке, что побайтовый проход на little-endian.
// Поэтому здесь не «быстрая и медленная версии с разным результатом», а одна
// величина двумя способами — иначе смена базовой архитектуры сдвигала бы любое
// значение, выведенное из этой функции.
//
// Ветка нужна потому, что интринсики CRC32 требуют SSE4.2, а сборка с
// DEVILS_ENGINE_ARCH=OFF его не включает: безусловный вызов ломал сборку под
// машину без AVX целиком.
#if defined(__SSE4_2__) || defined(_MSC_VER)
uint32_t crc32c(const uint8_t* data, const size_t len) noexcept {
  uint64_t crc = 0xffffffffu;
  size_t i = 0;

  // 8 байт
  for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
    uint64_t chunk = 0;
    memcpy(&chunk, data + i, sizeof(uint64_t));
    crc = _mm_crc32_u64(crc, chunk);
  }

  // 4 байт
  if (i + sizeof(uint32_t) <= len) {
    uint32_t chunk = 0;
    std::memcpy(&chunk, data + i, sizeof(uint32_t));
    crc = _mm_crc32_u32(static_cast<uint32_t>(crc), chunk);
    i += sizeof(uint32_t);
  }

  // 1 байт
  for (; i < len; ++i) {
    crc = _mm_crc32_u8(static_cast<uint32_t>(crc), data[i]);
  }

  return static_cast<uint32_t>(crc ^ 0xffffffffu); // финальный XOR
}
#else
uint32_t crc32c(const uint8_t* data, const size_t len) noexcept {
  // Отражённый полином Кастаньоли: 0x1edc6f41 в обратном порядке битов.
  constexpr uint32_t reflected_polynomial = 0x82f63b78u;
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (reflected_polynomial & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xffffffffu;
}
#endif

uint32_t crc32c(const std::span<const uint8_t>& data) noexcept {
  return crc32c(data.data(), data.size());
}
uint32_t crc32c(const std::span<const char>& data) noexcept {
  return crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
uint32_t crc32c(const std::span<uint8_t>& data) noexcept {
  return crc32c(data.data(), data.size());
}
uint32_t crc32c(const std::span<char>& data) noexcept {
  return crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
uint32_t crc32c(const std::string_view& data) noexcept {
  return crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
} // namespace utils
} // namespace devils_engine
