#include "config.h"

#include <utility>

#include <devils_engine/utils/core.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

std::string make_project_path(std::string path) {
  if (path.empty()) return utils::project_folder();
  if (path.front() == '/') return path;
  return utils::project_folder() + path;
}

std::string make_project_folder_path(std::string path) {
  path = make_project_path(std::move(path));
  if (!path.empty() && path.back() != '/') path.push_back('/');
  return path;
}

}
}
