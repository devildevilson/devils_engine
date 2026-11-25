#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace devils_engine::utils {
// windows, unix, macos
std::string app_path() noexcept;
std::string project_folder() noexcept;
std::string cache_folder() noexcept;

// Процессор для диагностики (cpuinfo). Кэши — суммарный объём уровня по всем ядрам.
struct cpu_report {
  std::string name;
  std::string vendor;
  std::string isa;          // расширения, которые различает движок, — что есть у процессора
  std::string required_isa; // под что собран код движка (DEVILS_ENGINE_ARCH)
  std::vector<uint32_t> cluster_cores; // ядра по кластерам: у гибридных (P/E) кластеров несколько
  uint64_t l1d_bytes = 0;
  uint64_t l2_bytes = 0;
  uint64_t l3_bytes = 0;
  uint32_t packages = 0;
  uint32_t cores = 0;
  uint32_t logical_processors = 0;
};

// Пустой name — cpuinfo не смог опросить процессор.
cpu_report query_cpu() noexcept;
// Одна строка для лога: имя, ядра/потоки, кластеры, кэши, расширения.
std::string describe(const cpu_report& cpu);

} // namespace devils_engine::utils
