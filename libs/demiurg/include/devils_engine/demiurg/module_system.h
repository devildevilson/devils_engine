#ifndef DEVILS_ENGINE_DEMIURG_MODULE_SYSTEM_H
#define DEVILS_ENGINE_DEMIURG_MODULE_SYSTEM_H

// Owns the ordered set of resource modules and their override lookup context.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "resource_manifest.h"

namespace devils_engine {
namespace demiurg {
class resource_system;
class module_interface;

class module_system {
public:
  static constexpr uint32_t fingerprint_format_version = 1;

  enum class module_kind : uint8_t {
    directory,
    archive
  };

  struct list_entry {
    std::string path;
    std::string hash;
    std::string file_date;
  };

  struct loaded_module {
    std::string id;
    std::string source;
    std::string fingerprint;
    module_kind kind = module_kind::directory;
    uint32_t priority = 0;
  };

  explicit module_system(std::string path);
  ~module_system() noexcept;

  std::string path() const;
  std::string_view modules_list() const;
  void set_modules_list(std::string modules_list);

  std::vector<list_entry> load_list(const std::string_view& list_name) const;
  void load_modules(std::vector<list_entry> paths);
  void load_default_modules();
  void open_modules();
  void close_modules();
  void discover_resources(std::vector<resource_candidate>& out);

  std::span<const loaded_module> loaded_modules() const noexcept;
  std::string_view fingerprint() const noexcept;

private:
  void discover_resources_impl(std::vector<resource_candidate>& out);
  void rebuild_fingerprint();

  std::string _path;
  std::string modules_list_name;
  std::vector<std::unique_ptr<module_interface>> modules;
  std::vector<loaded_module> loaded_module_list;
  std::string modules_fingerprint;
};
} // namespace demiurg
} // namespace devils_engine

#endif
