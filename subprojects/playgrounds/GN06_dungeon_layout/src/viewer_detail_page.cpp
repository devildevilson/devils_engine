#include "viewer_detail_page.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace gn06 {

viewer_detail_page make_viewer_detail_page(const std::span<const std::string> lines,
                                          const size_t requested_page, const size_t line_limit) {
  constexpr size_t summary_lines = 2;
  if (line_limit <= summary_lines + 1) {
    throw std::invalid_argument("GN06 detail page needs at least four lines");
  }
  viewer_detail_page result;
  if (lines.size() <= line_limit) {
    result.lines.assign(lines.begin(), lines.end());
    return result;
  }

  const auto page_capacity = line_limit - summary_lines - 1;
  const auto properties = lines.size() - summary_lines;
  result.count = 1 + (properties - 1) / page_capacity;
  result.index = std::min(requested_page, result.count - 1);
  const auto begin = summary_lines + result.index * page_capacity;
  const auto end = begin + std::min(page_capacity, lines.size() - begin);
  result.lines.reserve(line_limit);
  result.lines.insert(result.lines.end(), lines.begin(), lines.begin() + summary_lines);
  result.lines.insert(result.lines.end(), lines.begin() + begin, lines.begin() + end);
  result.lines.push_back(std::format("properties {}/{} | PgUp/PgDn | lines {}-{} of {}",
    result.index + 1, result.count, begin - summary_lines + 1, end - summary_lines, properties));
  return result;
}

} // namespace gn06
