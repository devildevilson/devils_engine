#ifndef DEVILS_ENGINE_GN06_VIEWER_DETAIL_PAGE_H
#define DEVILS_ENGINE_GN06_VIEWER_DETAIL_PAGE_H

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace gn06 {

struct viewer_detail_page {
  std::vector<std::string> lines;
  size_t index = 0;
  size_t count = 1;
};

// Первая пара строк — общая сводка плана, она остаётся на каждой странице. Остальные
// свойства показываются без потерь; последняя строка длинного списка отведена навигации.
// Результат владеет строками. Предел задаёт панель, он должен быть не меньше четырёх.
viewer_detail_page make_viewer_detail_page(const std::span<const std::string> lines,
                                          const size_t requested_page, const size_t line_limit);

} // namespace gn06

#endif
