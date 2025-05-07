#ifndef DEVILS_ENGINE_DEMIURG_MODULES_LISTING_H
#define DEVILS_ENGINE_DEMIURG_MODULES_LISTING_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
namespace fs = std::filesystem;

namespace devils_engine {
namespace demiurg {
  // нужно как то понять какой лист выбрал игрок в качестве текущего
  // сам по себе для листа нужно будет посчитать хеш
  // этот лист нужно будет сохранить в игровой сейв
class modules_listing {
public:
  struct module_entry {
    std::string path;
    std::string name;
    std::string hash;
    size_t workshop_id;
    size_t timestamp;
    std::string file_date;
    std::string description;
    // ресурс картинок
  };

  struct list_entry {
    std::string path;
    std::string hash;
    std::string file_date;
  };

  struct view {
    std::string path;
    std::string hash;
    const module_entry* ptr;
  };

  struct list {
    std::string name;
    std::vector<view> entries;
  };

  // рут должен совпадать с рутом из системы
  modules_listing(std::string root) noexcept;
  ~modules_listing() noexcept;

  // заново загрузим файлы в папке + листы
  void clear();
  void reload();

  void save_list(const std::string &name, const std::vector<view> &entries) const;
  void save_list(const std::string &name, const std::vector<std::string> &paths) const;
  list load_list(const std::string &name) const;
  const std::vector<list> & available_lists() const;
  std::vector<std::string> available_lists_names() const;
  const std::vector<std::unique_ptr<module_entry>> & entries() const;
 private:
  std::string root_path;

  std::vector<std::unique_ptr<module_entry>> entries_arr;
  std::vector<list> lists;

  void load_list(const std::string &path);
  void load_mod(const fs::directory_entry &entry);
};
}
}

#endif