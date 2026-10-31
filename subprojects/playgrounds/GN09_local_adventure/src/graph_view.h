#pragma once

#include <string>

#include "adventure.h"

namespace gn09 {

// Самодостаточная HTML-страница с графом окна в 3D: узлы, рёбра, залы, их петли,
// ниши и домены существ. Инструмент осмотра; ничего не проверяет.
bool write_graph_html(const adventure& world, const std::string& path);

} // namespace gn09
