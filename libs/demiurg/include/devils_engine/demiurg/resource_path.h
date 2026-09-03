#ifndef DEVILS_ENGINE_DEMIURG_RESOURCE_PATH_H
#define DEVILS_ENGINE_DEMIURG_RESOURCE_PATH_H

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// Правила АДРЕСАЦИИ ресурса, отделённые от того, кто эту адресацию использует.
//
// У demiurg-ресурса нет расширения и нет относительного пути: id — это путь от корня модуля без
// расширения, потому что расширение это способ хранения, а не имя вещи (одна и та же карта высот
// может приехать png или exr, и ссылка на неё не должна от этого меняться), а относительность
// сломалась бы при первом же переопределении файла модом.
//
// Значит между тем, что ЧЕЛОВЕК пишет в конфиге, и тем, чем адресуется ресурс, нужен один перевод —
// и он обязан быть один на весь движок, иначе lua, генератор и render config разойдутся в том, что
// значит `../common/values.tavl`. Функции ниже и есть этот перевод; они не трогают реестр, поэтому
// живут заголовком без зависимостей.

namespace devils_engine {
namespace demiurg {

// Путь к «папке», в которой лежит ресурс: id без последнего сегмента.
inline std::string resource_parent_path(const std::string_view id) {
  const size_t slash = id.rfind('/');
  if (slash == std::string_view::npos) {
    return {};
  }
  return std::string(id.substr(0, slash));
}

// Абсолютный id ресурса из того, что написано в конфиге или скрипте.
//
// current_module — id ТОГО ресурса, внутри которого написана ссылка: относительный путь считается от
// его папки. Относительным считается только путь, начинающийся с точки (`./x`, `../x`); всё
// остальное — уже абсолютный путь от корня модуля, поэтому обычная ссылка не зависит от того, где
// лежит ссылающийся файл.
//
// По дороге снимается расширение и сворачиваются `.`/`..`, а хвост `:name` (суб-ресурс документа,
// разложенного по `//---`) отделяется и приклеивается обратно нетронутым.
inline std::string absolute_resource_path(const std::string_view current_module, std::string_view path) {
  while (!path.empty() && (path.front() == ' ' || path.front() == '\t' || path.front() == '\n' || path.front() == '\r')) {
    path.remove_prefix(1);
  }
  while (!path.empty() && (path.back() == ' ' || path.back() == '\t' || path.back() == '\n' || path.back() == '\r')) {
    path.remove_suffix(1);
  }
  if (path.empty()) {
    return {};
  }

  std::string p(path);
  std::replace(p.begin(), p.end(), '\\', '/');

  std::string selector;
  const size_t colon = p.rfind(':');
  if (colon != std::string::npos) {
    selector = p.substr(colon);
    p.resize(colon);
  }

  const bool explicit_root = !p.empty() && p.front() == '/';
  while (!p.empty() && p.front() == '/') {
    p.erase(p.begin());
  }

  if (!explicit_root && p.starts_with(".")) {
    const std::string parent = resource_parent_path(current_module);
    if (!parent.empty()) {
      p = parent + "/" + p;
    }
  }

  const size_t slash = p.rfind('/');
  const size_t dot = p.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    p.resize(dot);
  }

  std::vector<std::string_view> segments;
  size_t pos = 0;
  while (pos <= p.size()) {
    size_t end = p.find('/', pos);
    if (end == std::string::npos) {
      end = p.size();
    }
    std::string_view segment(p.data() + pos, end - pos);
    pos = end + 1;

    if (segment.empty() || segment == ".") {
      continue;
    }
    if (segment == "..") {
      if (segments.empty()) {
        return {};
      }
      segments.pop_back();
      continue;
    }
    segments.push_back(segment);
  }

  std::string out;
  for (const auto segment : segments) {
    if (!out.empty()) {
      out += '/';
    }
    out += segment;
  }

  if (out.empty()) {
    return {};
  }
  out += selector;
  return out;
}

} // namespace demiurg
} // namespace devils_engine

#endif
