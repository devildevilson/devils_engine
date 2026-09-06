#include "devils_engine/utils/process_memory.h"

#include <cstdio>
#include <cstring>

#if defined(__linux__)
#  include <cstdlib>
#elif defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#endif

// Чтение пика памяти у системы. Способ у каждой свой, и общего нет: Linux отдаёт его строкой в
// `/proc/self/status`, Windows — структурой, macOS — портом задачи. Отсюда и правило «ноль означает
// не измеряется»: там, где ни одного способа нет, подставить нечего.
//
// Linux читается ФАЙЛОМ, а не через `getrusage`: `ru_maxrss` там в килобайтах, но у BSD в байтах, и
// одно и то же поле означало бы разное на разных системах — ровно тот класс расхождения, который
// проект ловит везде.

namespace devils_engine {
namespace utils {

namespace {
#if defined(__linux__)
// Значение строки `/proc/self/status` вида `VmHWM:  123456 kB`. Килобайты там означают 1024 байта:
// ядро печатает страницы, переведённые в KiB.
size_t status_field_bytes(const char* name) noexcept {
  std::FILE* file = std::fopen("/proc/self/status", "r");
  if (file == nullptr) {
    return 0;
  }

  const size_t length = std::strlen(name);
  size_t result = 0;
  char line[256];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (std::strncmp(line, name, length) != 0) {
      continue;
    }
    unsigned long long kilobytes = 0;
    if (std::sscanf(line + length, " %llu", &kilobytes) == 1) {
      result = size_t(kilobytes) * 1024;
    }
    break;
  }

  std::fclose(file);
  return result;
}
#endif
} // namespace

size_t peak_resident_bytes() noexcept {
#if defined(__linux__)
  return status_field_bytes("VmHWM:");
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return 0;
  }
  return size_t(counters.PeakWorkingSetSize);
#else
  // macOS отдаёт текущий размер, но не пик, поэтому пика здесь нет вовсе — и об этом говорит ноль.
  return 0;
#endif
}

size_t current_resident_bytes() noexcept {
#if defined(__linux__)
  return status_field_bytes("VmRSS:");
#elif defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return 0;
  }
  return size_t(counters.WorkingSetSize);
#elif defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return size_t(info.resident_size);
#else
  return 0;
#endif
}

} // namespace utils
} // namespace devils_engine
