#include "config.h"

#include <utility>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/fileio.h>

#include <tavl/deserialize.h>

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

app_config load_app_config(const std::string& path) {
  app_config cfg;
  if (!file_io::exists(path)) {
    utils::warn("Could not find app config '{}', using defaults", path);
    return cfg;
  }

  const auto content = file_io::read(path);
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  tavl::deserialize(parser, ctx, cfg);

  if (!ctx.diagnostics.empty()) {
    utils::warn("App config '{}' produced {} tavl diagnostics", path, ctx.diagnostics.size());
    for (const auto& d : ctx.diagnostics) {
      utils::warn("  tavl diagnostic {}, field '{}'", static_cast<size_t>(d.error.type), d.field);
    }
  }

  return cfg;
}

}
}
