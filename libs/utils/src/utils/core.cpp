#include "core.h"

#include <cstdlib>
#include <filesystem>

#include "utf/utf.hpp"

#ifdef _WIN32
#include <windows.h>    //GetModuleFileNameW
#else
#include <limits.h>
#include <unistd.h>     //readlink
#endif

#define DEVILS_ENGINE_AESTHETICS_IMPLEMENTATION
#include "aesthetics/world.h"

namespace fs = std::filesystem;

namespace devils_engine {
  namespace utils {
    std::string_view make_sane_file_name(const std::string_view &str) {
      const size_t slash1_index = str.rfind("/");
      if (slash1_index == std::string_view::npos) return str;

      const auto str2 = str.substr(0, slash1_index);
      const size_t slash2_index = str2.rfind("/");
      return slash2_index == std::string_view::npos ? str.substr(slash1_index+1) : str.substr(slash2_index+1);
    }

    void assert_failed_detail(
      const std::string_view &cond_str,
      const std::string_view &file_name,
      const std::string_view &func_name,
      const size_t line
    ) {
      spdlog::error("{}:{}: {}: Assertion `{}` failed", make_sane_file_name(file_name), line, func_name, cond_str);
      throw std::runtime_error("Assertion failed");
    }

    void assert_failed_detail(
      const std::string_view &cond_str,
      const std::string_view &file_name,
      const std::string_view &func_name,
      const size_t line,
      const std::string_view &comm
    ) {
      spdlog::error("{}:{}: {}: Assertion `{}` failed", make_sane_file_name(file_name), line, func_name, cond_str);
      spdlog::info(comm);
      throw std::runtime_error("Assertion failed");
    }

    tracer::tracer(std::source_location loc) noexcept : l(std::move(loc)) {
      spdlog::log(spdlog::level::trace, "in  {}:{} `{}`", make_sane_file_name(l.file_name()), l.line(), l.function_name());
    }

    tracer::~tracer() noexcept {
      spdlog::log(spdlog::level::trace, "out {}:{} `{}`", make_sane_file_name(l.file_name()), l.line(), l.function_name());
    }

    std::string cast(const std::wstring &str) noexcept {
      const size_t s = wcstombs(nullptr, str.c_str(), 0);
      if (s == SIZE_MAX) return std::string();
      std::string ret(s, '\0');
      wcstombs(ret.data(), str.c_str(), ret.size());
      return ret;
    }

    std::string cast(const std::u16string_view &str) noexcept { return utf::as_str8(str); }
    std::string cast(const std::u32string_view &str) noexcept { return utf::as_str8(str); }

    std::wstring cast(const std::string &str) noexcept {
      const size_t s = mbstowcs(nullptr, str.c_str(), 0);
      if (s == SIZE_MAX) return std::wstring();
      std::wstring ret(s, '\0');
      mbstowcs(ret.data(), str.c_str(), ret.size());
      return ret;
    }

    std::u16string cast16(const std::string_view& str) noexcept { return utf::as_u16(str); }
    std::u32string cast32(const std::string_view& str) noexcept { return utf::as_u32(str); }

    std::string app_path() noexcept {
#ifdef _WIN32
      wchar_t path[MAX_PATH] = { 0 };
      GetModuleFileNameW(NULL, path, MAX_PATH);
      const size_t s = wcstombs(nullptr, path, 0);
      if (s == SIZE_MAX) return std::string();
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
      return std::string(bin_dir.substr(0, bin_dir.rfind('/')+1));
    }

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
        crc = _mm_crc32_u32(crc, chunk);
        i += sizeof(uint32_t);
      }

      // 1 байт
      for (; i < len; i++) {
        crc = _mm_crc32_u8(crc, data[i]);
      }

      return static_cast<uint32_t>(crc ^ 0xffffffffu); // финальный XOR
    }

    uint32_t crc32c(const std::span<const uint8_t>& data) noexcept { return crc32c(data.data(), data.size()); }
    uint32_t crc32c(const std::span<const char>& data) noexcept { return crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size()); }
    uint32_t crc32c(const std::span<uint8_t>& data) noexcept { return crc32c(data.data(), data.size()); }
    uint32_t crc32c(const std::span<char>& data) noexcept { return crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size()); }
    uint32_t crc32c(const std::string_view& data) noexcept { return crc32c(reinterpret_cast<const uint8_t*>(data.data()), data.size()); }
  }
}
