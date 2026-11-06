#ifndef DEVILS_ENGINE_DEMIURG_MODULE_PROFILE_H
#define DEVILS_ENGINE_DEMIURG_MODULE_PROFILE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace devils_engine::demiurg {

// Один формат ordered module profile для runtime и каталога установленных модулей.
// Путь относителен корню modules; hash/file_date могут отсутствовать при authoring.
struct module_profile_entry {
  std::string path;
  std::string hash;
  std::string file_date;
};
struct module_profile {
  uint32_t version = 1;
  std::vector<module_profile_entry> modules;
};

std::filesystem::path module_profile_path(const std::filesystem::path& root, std::string_view name);
module_profile read_module_profile(const std::filesystem::path& path);
void write_module_profile(const std::filesystem::path& path, const module_profile& profile);

} // namespace devils_engine::demiurg
#endif
