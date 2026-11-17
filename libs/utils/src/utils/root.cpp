#include "devils_engine/utils/root.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace devils_engine {
namespace utils {

std::string_view to_string(const path_refusal refusal) noexcept {
  switch (refusal) {
    case path_refusal::absolute: return "path is absolute; only paths relative to the root are allowed";
    case path_refusal::escapes_root: return "path leaves the root through '..'";
    case path_refusal::invalid: return "path contains ':' or a null character";
    case path_refusal::root_not_absolute: return "root is not an absolute path";
    case path_refusal::root_not_directory: return "root is not an existing directory";
  }
  return "unknown path refusal";
}

std::expected<root, path_refusal> root::open(const std::string_view base) {
  const std::filesystem::path path(base);
  if (!path.is_absolute()) {
    return std::unexpected(path_refusal::root_not_absolute);
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    return std::unexpected(path_refusal::root_not_directory);
  }
  std::string normal = path.lexically_normal().generic_string();
  if (normal.empty() || normal.back() != '/') {
    normal.push_back('/');
  }
  return root(std::move(normal));
}

std::expected<std::string, path_refusal> root::resolve(const std::string_view relative) const {
  std::string p(relative);
  std::replace(p.begin(), p.end(), '\\', '/');

  if (!p.empty() && p.front() == '/') {
    return std::unexpected(path_refusal::absolute);  // '/x', '\x', '//server', '\\server'
  }
  const bool drive = p.size() >= 2 && p[1] == ':' &&
                     ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z'));
  if (drive) {
    return std::unexpected(path_refusal::absolute);  // 'C:\x' and the drive-relative 'C:x'
  }
  if (p.find(':') != std::string::npos || p.find('\0') != std::string::npos) {
    return std::unexpected(path_refusal::invalid);
  }

  std::vector<std::string_view> segments;
  size_t pos = 0;
  while (pos <= p.size()) {
    size_t end = p.find('/', pos);
    if (end == std::string::npos) {
      end = p.size();
    }
    const std::string_view segment(p.data() + pos, end - pos);
    pos = end + 1;
    if (segment.empty() || segment == ".") {
      continue;
    }
    if (segment == "..") {
      if (segments.empty()) {
        return std::unexpected(path_refusal::escapes_root);
      }
      segments.pop_back();
      continue;
    }
    segments.push_back(segment);
  }

  std::string out = base_;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i != 0) {
      out.push_back('/');
    }
    out += segments[i];
  }
  if (!segments.empty() && !p.empty() && p.back() == '/') {
    out.push_back('/');
  }
  return out;
}

} // namespace utils
} // namespace devils_engine
