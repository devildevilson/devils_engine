#include "devils_engine/originator/motif_geometry.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <utility>

// Реестр не доверяет даже собственной процедуре: один валидатор защищает потребителя
// от незаконных частей. Пределы координат удерживают cross/SAT в безопасной int64-арифметике.

namespace devils_engine::originator {
namespace {

constexpr int32_t coordinate_limit = 1'048'576;

std::string validate_geometry(const motif_geometry_result& result) {
  if (result.pieces.empty() || result.pieces.size() > 256) {
    return "geometry needs 1..256 convex pieces";
  }
  for (size_t piece = 0; piece < result.pieces.size(); ++piece) {
    const auto& outline = result.pieces[piece];
    if (outline.size() < 3 || outline.size() > 256) {
      return std::format("piece {} needs 3..256 vertices", piece);
    }
    for (const auto point : outline) {
      if (std::abs(int64_t(point.x)) > coordinate_limit || std::abs(int64_t(point.y)) > coordinate_limit) {
        return std::format("piece {} exceeds coordinate limits", piece);
      }
    }
    // Все остальные вершины слева от каждого направленного ребра. Это проверяет
    // именно выпуклый CCW-контур, а не только знаки соседних поворотов у звезды.
    int64_t area2 = 0;
    for (size_t i = 0; i < outline.size(); ++i) {
      const auto a = outline[i], b = outline[(i + 1) % outline.size()];
      if (std::find(outline.begin(), outline.begin() + i, a) != outline.begin() + i) {
        return std::format("piece {} repeats a vertex", piece);
      }
      if (a == b) { return std::format("piece {} has a zero edge", piece); }
      area2 += int64_t(a.x) * b.y - int64_t(a.y) * b.x;
      for (const auto c : outline) {
        if ((int64_t(b.x) - a.x) * (int64_t(c.y) - a.y) -
            (int64_t(b.y) - a.y) * (int64_t(c.x) - a.x) < 0) {
          return std::format("piece {} is not convex CCW", piece);
        }
      }
    }
    if (area2 <= 0) { return std::format("piece {} has no positive area", piece); }
    for (size_t prior = 0; prior < piece; ++prior) {
      if (convex_interiors_overlap(outline, result.pieces[prior])) {
        return std::format("pieces {} and {} overlap", prior, piece);
      }
    }
  }
  std::vector<bool> reached(result.pieces.size(), false);
  std::vector<size_t> queue{0};
  reached[0] = true;
  for (size_t read = 0; read < queue.size(); ++read) {
    for (size_t next = 0; next < result.pieces.size(); ++next) {
      if (!reached[next] && share_motif_boundary(result.pieces[queue[read]], result.pieces[next])) {
        reached[next] = true;
        queue.push_back(next);
      }
    }
  }
  if (queue.size() != result.pieces.size()) { return "geometry pieces are disconnected"; }
  return {};
}

motif_geometry_result rectangle(const motif_geometry_input& input) {
  if (!input.parameters.empty()) { return {{}, "rectangle accepts no extra parameters"}; }
  const auto w = int32_t(input.width), h = int32_t(input.height);
  return {{{{0, 0}, {w, 0}, {w, h}, {0, h}}}, {}};
}

motif_geometry_result rounded_east(const motif_geometry_input& input) {
  if (!input.parameters.empty()) { return {{}, "rounded_east accepts no extra parameters"}; }
  const auto w = int32_t(input.width), h = int32_t(input.height);
  if (w < 8 || h < 8) { return {{}, "rounded_east needs at least 8x8"}; }
  const auto shoulder = std::clamp(w / 6, 2, 4);
  return {{{{0, 0}, {w - shoulder, 0}, {w, h / 2 - 1},
    {w, (h + 1) / 2 + 1}, {w - shoulder, h}, {0, h}}}, {}};
}

motif_geometry_result diagonal(const motif_geometry_input& input) {
  if (!input.parameters.empty()) { return {{}, "diagonal accepts no extra parameters"}; }
  const auto w = int32_t(input.width), h = int32_t(input.height);
  if (w < 7 || h < 7) { return {{}, "diagonal needs at least 7x7"}; }
  const auto band = std::min({3, w / 3, h / 3});
  return {{{{0, 0}, {band, 0}, {w, h - band}, {w, h}, {w - band, h}, {0, band}}}, {}};
}

} // namespace

void motif_geometry_registry::add(std::string name, const motif_geometry_builder builder) {
  if (name.empty() || !builder || contains(name)) {
    throw std::invalid_argument(std::format("motif geometry: invalid or duplicate operation '{}'", name));
  }
  operations_.push_back({std::move(name), builder});
}

bool motif_geometry_registry::contains(const std::string_view name) const noexcept {
  return std::any_of(operations_.begin(), operations_.end(),
    [&](const operation& operation) { return operation.name == name; });
}

motif_geometry_result motif_geometry_registry::build(const std::string_view name,
                                                     const motif_geometry_input& input) const {
  const auto found = std::find_if(operations_.begin(), operations_.end(),
    [&](const operation& operation) { return operation.name == name; });
  if (found == operations_.end()) {
    return {{}, std::format("motif geometry: unknown operation '{}'", name)};
  }
  if (input.width == 0 || input.height == 0 || input.width > 4096 || input.height > 4096) {
    return {{}, std::format("motif geometry '{}': dimensions must be in 1..4096", name)};
  }
  if (input.parameters.size() > 256) {
    return {{}, std::format("motif geometry '{}': too many parameters", name)};
  }
  for (size_t i = 0; i < input.parameters.size(); ++i) {
    if (input.parameters[i].name.empty() ||
        std::any_of(input.parameters.begin(), input.parameters.begin() + i,
          [&](const auto& prior) { return prior.name == input.parameters[i].name; })) {
      return {{}, std::format("motif geometry '{}': invalid/duplicate parameter", name)};
    }
  }
  auto result = found->builder(input);
  if (result.valid()) { result.refusal = validate_geometry(result); }
  if (!result.valid()) {
    result.refusal = std::format("motif geometry '{}': {}", name, result.refusal);
    result.pieces.clear();
  }
  return result;
}

void add_standard_motif_geometry(motif_geometry_registry& registry) {
  registry.add("rectangle", rectangle);
  registry.add("rounded_east", rounded_east);
  registry.add("diagonal", diagonal);
}

} // namespace devils_engine::originator
