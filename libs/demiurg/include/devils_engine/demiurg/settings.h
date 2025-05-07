#ifndef DEVILS_ENGINE_DEMIURG_SETTINGS_H
#define DEVILS_ENGINE_DEMIURG_SETTINGS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include "utils/named_serializer.h"
#include "utils/fileio.h"
#include "utils/core.h"

namespace devils_engine {
namespace demiurg {

template <typename T>
class settings : public T {
public:
  template <typename... Args>
  settings(Args&&... args) noexcept : T(std::forward<Args>(args)...), path(utils::project_folder() + "settings.json") {}

  void set_path(std::string path) {
    this->path = std::move(path);
    reload();
  }

  void reload() {
    if (!file_io::exists(path)) {
      // ничего не делаем?
      return;
    }
    const auto cont = file_io::read(path);
    T& ref = *this;
    ref = utils::from_json(cont);
  }

  void sync() {
    const T& ref = *this;
    // запишем в хорошем формате
    const auto cont = utils::to_json<glz::opts{.prettify = true, .indentation_width = 2}>(ref);
    file_io::write(cont, path);
  }
private:
  std::string path;
};
}
}

#endif