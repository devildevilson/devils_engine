#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <queue>
#include <utility>
#include <vector>

#include "devils_engine/originator/motif_layout.h"
#include "devils_engine/utils/shared.h"

// СБОРКА АВТОРСКИХ МОТИВОВ. Программа задаёт дерево пристыковок, но не координаты. Сборщик
// выбирает размеры, сторону и место вдоль стены целочисленно; ограниченное число попыток позволяет
// отклонить тесную программу с причиной. Проходы записываются отдельно от прямоугольников, чтобы
// обход проверял геометрию, а не доверял только связям программы. Случайность привязана к номеру
// попытки, правила и выбора: добавление независимой фоновой работы не меняет план.

namespace devils_engine::originator {
namespace {

uint32_t random_word(const uint64_t seed, const uint32_t attempt, const uint32_t rule,
                     const uint32_t choice) noexcept {
  const auto lo = uint32_t(seed);
  const auto hi = uint32_t(seed >> 32);
  return utils::shared::prng2(utils::shared::prng2(lo ^ attempt, hi ^ rule), choice + 0x9e3779b9u);
}

bool contains(const motif_rect& rect, const int32_t x, const int32_t y) noexcept {
  return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

bool near_rect(const motif_rect& rect, const int32_t x, const int32_t y) noexcept {
  return x >= rect.x - 1 && y >= rect.y - 1 && x <= rect.x + rect.w && y <= rect.y + rect.h;
}

bool too_close(const motif_rect& a, const motif_rect& b) noexcept {
  return a.x < b.x + b.w + 1 && b.x < a.x + a.w + 1 &&
         a.y < b.y + b.h + 1 && b.y < a.y + a.h + 1;
}

bool share_edge(const motif_rect& a, const motif_rect& b) noexcept {
  const auto x_overlap = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
  const auto y_overlap = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
  return (x_overlap > 0 && y_overlap == 0) || (y_overlap > 0 && x_overlap == 0);
}

bool fits(const motif_layout& layout, const motif_rect& rect, const int32_t door_x,
          const int32_t door_y, const uint32_t parent, const bool direct_join) noexcept {
  if (rect.x < 2 || rect.y < 1 || rect.x + rect.w > layout.width - 1 ||
      rect.y + rect.h > layout.height - 1) return false;

  for (uint32_t i = 0; i < layout.instances.size(); ++i) {
    const auto& item = layout.instances[i];
    if (i == parent && direct_join) {
      if (!share_edge(rect, item.rect)) return false;
    } else if (too_close(rect, item.rect)) return false;
  }
  for (const auto& passage : layout.passages) {
    if (near_rect(rect, passage.x, passage.y)) return false;
    if (!direct_join && passage.x == door_x && passage.y == door_y) return false;
  }
  if (direct_join) return true;
  for (uint32_t i = 0; i < layout.instances.size(); ++i) {
    if (i != parent && near_rect(layout.instances[i].rect, door_x, door_y)) return false;
  }
  return door_x > 1 && door_x < layout.width - 1 && door_y > 0 && door_y < layout.height - 1;
}

struct candidate {
  motif_rect rect;
  int32_t door_x = 0;
  int32_t door_y = 0;
};

candidate position_at(const motif_rect& parent, const int32_t w, const int32_t h,
                      const uint32_t side, const int32_t offset, const uint32_t door_choice,
                      const motif_attach_align passage_align, const bool direct_join,
                      const uint32_t passage_inset) noexcept {
  candidate result;
  auto& r = result.rect;
  r.w = w;
  r.h = h;
  const auto gap = direct_join ? 0 : 1;
  switch (side) {
    case 0: r = {parent.x + parent.w + gap, offset, w, h}; break;
    case 1: r = {parent.x - w - gap, offset, w, h}; break;
    case 2: r = {offset, parent.y + parent.h + gap, w, h}; break;
    default: r = {offset, parent.y - h - gap, w, h}; break;
  }
  if (side < 2) {
    const auto start = std::max(parent.y, r.y);
    const auto length = std::min(parent.y + parent.h, r.y + r.h) - start;
    const auto inset = std::min(passage_inset, uint32_t(length - 1) / 2);
    const auto available = uint32_t(length) - 2 * inset;
    result.door_x = side == 0 ? parent.x + parent.w : parent.x - 1;
    const auto chosen = passage_align == motif_attach_align::start ? inset :
      passage_align == motif_attach_align::end ? uint32_t(length - 1) - inset :
      passage_align == motif_attach_align::centre ? uint32_t(length / 2) :
      inset + door_choice % available;
    result.door_y = start + int32_t(chosen);
  } else {
    const auto start = std::max(parent.x, r.x);
    const auto length = std::min(parent.x + parent.w, r.x + r.w) - start;
    const auto inset = std::min(passage_inset, uint32_t(length - 1) / 2);
    const auto available = uint32_t(length) - 2 * inset;
    const auto chosen = passage_align == motif_attach_align::start ? inset :
      passage_align == motif_attach_align::end ? uint32_t(length - 1) - inset :
      passage_align == motif_attach_align::centre ? uint32_t(length / 2) :
      inset + door_choice % available;
    result.door_x = start + int32_t(chosen);
    result.door_y = side == 2 ? parent.y + parent.h : parent.y - 1;
  }
  return result;
}

bool place_rule(motif_layout& layout, const motif_rule& rule, const uint32_t rule_index,
                const uint32_t parent_index, const uint64_t seed, const uint32_t attempt) {
  const auto& parent = layout.instances[parent_index].rect;
  const auto w = int32_t(rule.min_width + random_word(seed, attempt, rule_index, 1) %
                        (rule.max_width - rule.min_width + 1));
  const auto h = int32_t(rule.min_height + random_word(seed, attempt, rule_index, 2) %
                        (rule.max_height - rule.min_height + 1));
  const auto first_side = random_word(seed, attempt, rule_index, 3) % 4;

  for (uint32_t turn = 0; turn < 4; ++turn) {
    const auto side = (first_side + turn) % 4;
    if ((rule.sides & (1u << side)) == 0) continue;
    const auto low = side < 2 ? parent.y - h + 1 : parent.x - w + 1;
    const auto high = side < 2 ? parent.y + parent.h - 1 : parent.x + parent.w - 1;
    const auto count = uint32_t(high - low + 1);
    const auto target = rule.align == motif_attach_align::start ?
      (side < 2 ? parent.y : parent.x) :
      rule.align == motif_attach_align::end ?
      (side < 2 ? parent.y + parent.h - h : parent.x + parent.w - w) :
      (side < 2 ? parent.y + (parent.h - h) / 2 : parent.x + (parent.w - w) / 2);
    const auto first = rule.align == motif_attach_align::random ?
      random_word(seed, attempt, rule_index, 4 + side) % count :
      uint32_t(std::clamp(target - low, 0, high - low));
    for (uint32_t step = 0; step < (rule.strict_align ? 1u : count); ++step) {
      const auto offset = low + int32_t(rule.align == motif_attach_align::end ?
        (first + count - step) % count : (first + step) % count);
      const auto door_choice = random_word(seed, attempt, rule_index, 8 + side * 4096 + step);
      const auto placed = position_at(parent, w, h, side, offset, door_choice,
                                      rule.passage_align, rule.direct_join, rule.passage_inset);
      if (!fits(layout, placed.rect, placed.door_x, placed.door_y, parent_index, rule.direct_join)) continue;
      const auto index = uint32_t(layout.instances.size());
      layout.instances.push_back({placed.rect, rule.motif, rule.location, rule_index,
                                  parent_index, rule.direct_join});
      if (!rule.direct_join)
        layout.passages.push_back({parent_index, index, placed.door_x, placed.door_y});
      return true;
    }
  }
  return false;
}

std::string check_rules(const std::span<const motif_rule> rules, const int32_t width,
                        const int32_t height, const int32_t entry_y,
                        const std::span<const uint8_t> exposure, const uint32_t attempts) {
  if (rules.empty()) return "the motif program is empty";
  if (width < 8 || height < 8 || int64_t(width) * height > 16'777'216)
    return "the footprint is outside the supported 8..16777216-unit area";
  if (entry_y < 1 || entry_y >= height - 1) return "the entrance is outside the interior edge";
  if (attempts == 0) return "the attempt budget is zero";
  if (!exposure.empty() && exposure.size() != size_t(width) * size_t(height))
    return "the surface exposure mask does not match the footprint";
  if (rules[0].parent != no_motif_parent) return "rule 0 must be the root";
  if (rules[0].chance_percent != 100) return "the root must be mandatory";
  if (rules[0].direct_join) return "the root cannot directly join a parent";

  for (uint32_t i = 0; i < rules.size(); ++i) {
    const auto& rule = rules[i];
    if (rule.min_width < 3 || rule.min_height < 3 || rule.max_width < rule.min_width ||
        rule.max_height < rule.min_height || rule.max_width > uint32_t(width) ||
        rule.max_height > uint32_t(height))
      return std::format("rule {} has an invalid size range", i);
    if (rule.chance_percent > 100) return std::format("rule {} has a chance above 100%", i);
    if (rule.align > motif_attach_align::end || rule.passage_align > motif_attach_align::end)
      return std::format("rule {} has an unknown edge or passage alignment", i);
    if (rule.strict_align && rule.align == motif_attach_align::random)
      return std::format("rule {} cannot strictly align to a random offset", i);
    if (i == 0) continue;
    if (rule.parent >= i) return std::format("rule {} must name an earlier parent", i);
    if (rule.sides == 0 || (rule.sides & 0xf0u) != 0)
      return std::format("rule {} has no valid attachment side", i);
    if (rules[rule.parent].chance_percent != 100)
      return std::format("rule {} depends on an optional parent", i);
  }
  return {};
}

} // namespace

motif_layout assemble_motifs(const std::span<const motif_rule> rules, const int32_t width,
                             const int32_t height, const int32_t entry_y, const uint64_t seed,
                             const std::span<const uint8_t> surface_exposure,
                             const uint32_t max_attempts) {
  motif_layout result;
  result.width = width;
  result.height = height;
  result.entry_y = entry_y;
  result.refusal = check_rules(rules, width, height, entry_y, surface_exposure, max_attempts);
  if (!result.valid()) return result;

  uint32_t last_failed = 0;
  std::string last_validation_failure;
  for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
    last_validation_failure.clear();
    motif_layout trial;
    trial.width = width;
    trial.height = height;
    trial.entry_y = entry_y;
    trial.attempts = attempt + 1;
    const auto& root = rules[0];
    const auto w = int32_t(root.min_width + random_word(seed, attempt, 0, 1) %
                          (root.max_width - root.min_width + 1));
    const auto h = int32_t(root.min_height + random_word(seed, attempt, 0, 2) %
                          (root.max_height - root.min_height + 1));
    const auto y = std::clamp(entry_y - h / 2, 1, std::max(1, height - h - 1));
    if (2 + w > width - 1 || y + h > height - 1 || entry_y < y || entry_y >= y + h) {
      last_failed = 0;
      continue;
    }
    trial.instances.push_back({{2, y, w, h}, root.motif, root.location, 0, no_motif_parent});
    trial.passages.push_back({no_motif_parent, 0, 1, entry_y});

    std::vector<uint32_t> rule_to_instance(rules.size(), no_motif_parent);
    rule_to_instance[0] = 0;
    bool complete = true;
    for (uint32_t i = 1; i < rules.size(); ++i) {
      const auto& rule = rules[i];
      if (random_word(seed, attempt, i, 0) % 100 >= rule.chance_percent) continue;
      if (!place_rule(trial, rule, i, rule_to_instance[rule.parent], seed, attempt)) {
        last_failed = i;
        complete = false;
        break;
      }
      rule_to_instance[i] = uint32_t(trial.instances.size() - 1);
    }
    if (!complete) continue;

    for (uint32_t i = 0; i < trial.instances.size(); ++i) {
      const auto& rect = trial.instances[i].rect;
      for (int32_t y0 = rect.y; y0 < rect.y + rect.h; ++y0) {
        for (int32_t x0 = rect.x; x0 < rect.x + rect.w; ++x0) {
          if (!surface_exposure.empty() && surface_exposure[size_t(y0) * width + x0] != 0)
            trial.exposures.push_back({i, x0, y0});
        }
      }
    }
    for (const auto& passage : trial.passages) {
      if (passage.a == no_motif_parent) continue;
      if (!surface_exposure.empty() && surface_exposure[size_t(passage.y) * width + passage.x] != 0)
        trial.exposures.push_back({passage.b, passage.x, passage.y});
    }
    trial.refusal = validate_motif_layout(trial);
    if (trial.valid()) return trial;
    last_failed = uint32_t(rules.size());
    last_validation_failure = std::move(trial.refusal);
  }
  result.attempts = max_attempts;
  result.refusal = last_validation_failure.empty() ?
    std::format("could not place motif rule {} in {} attempts", last_failed, max_attempts) :
    std::format("candidate failed validation in {} attempts: {}", max_attempts, last_validation_failure);
  return result;
}

std::string validate_motif_layout(const motif_layout& layout) {
  if (!layout.refusal.empty()) return layout.refusal;
  if (layout.width < 3 || layout.height < 3 || layout.instances.empty()) return "the layout is empty";
  if (int64_t(layout.width) * layout.height > 16'777'216)
    return "the layout exceeds the supported validation area";
  const auto width = size_t(layout.width);
  const auto height = size_t(layout.height);
  std::vector<uint32_t> owner(width * height, 0);
  for (uint32_t i = 0; i < layout.instances.size(); ++i) {
    const auto& rect = layout.instances[i].rect;
    if (rect.w <= 0 || rect.h <= 0 || rect.x < 1 || rect.y < 1 ||
        rect.x + rect.w >= layout.width || rect.y + rect.h >= layout.height)
      return std::format("motif {} lies outside the footprint", i);
    for (int32_t y = rect.y; y < rect.y + rect.h; ++y) {
      for (int32_t x = rect.x; x < rect.x + rect.w; ++x) {
        auto& cell = owner[size_t(y) * width + x];
        if (cell != 0) return std::format("motifs {} and {} overlap", cell - 1, i);
        cell = i + 1;
      }
    }
  }
  for (uint32_t i = 1; i < layout.instances.size(); ++i) {
    const auto& item = layout.instances[i];
    if (item.parent >= i) return "a motif names a missing or later parent";
    if (item.direct_join && !share_edge(item.rect, layout.instances[item.parent].rect))
      return "a direct motif join does not share a parent edge";
    if (!item.direct_join && std::none_of(layout.passages.begin(), layout.passages.end(),
          [&](const motif_passage& passage) { return passage.a == item.parent && passage.b == i; }))
      return "a motif has no declared passage to its parent";
  }
  if (layout.entry_y < 0 || layout.entry_y >= layout.height) return "the entrance is outside the footprint";
  owner[size_t(layout.entry_y) * width] = uint32_t(layout.instances.size() + 1);
  for (const auto& passage : layout.passages) {
    if (passage.b >= layout.instances.size() ||
        (passage.a != no_motif_parent && passage.a >= layout.instances.size()))
      return "a passage names a missing motif";
    if (passage.a != no_motif_parent && layout.instances[passage.b].direct_join)
      return "a direct motif join also declares a passage";
    if (passage.x < 0 || passage.y < 0 || passage.x >= layout.width || passage.y >= layout.height)
      return "a passage lies outside the footprint";
    const auto index = size_t(passage.y) * width + size_t(passage.x);
    if (owner[index] != 0) return "a passage overwrites a motif or another passage";
    std::array<uint32_t, 4> neighbours{};
    size_t found = 0;
    for (const auto& [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
      const auto x = passage.x + dx;
      const auto y = passage.y + dy;
      if (x >= 0 && y >= 0 && x < layout.width && y < layout.height &&
          owner[size_t(y) * width + size_t(x)] != 0)
        neighbours[found++] = owner[size_t(y) * width + size_t(x)];
    }
    const auto expected_a = passage.a == no_motif_parent ? uint32_t(layout.instances.size() + 1) : passage.a + 1;
    if (found != 2 || std::find(neighbours.begin(), neighbours.begin() + found, expected_a) == neighbours.begin() + found ||
        std::find(neighbours.begin(), neighbours.begin() + found, passage.b + 1) == neighbours.begin() + found)
      return "a passage does not join exactly its declared two sides";
    owner[index] = uint32_t(layout.instances.size() + 2);
  }
  for (const auto& exposure : layout.exposures) {
    if (exposure.instance >= layout.instances.size())
      return "a surface exposure does not belong to its declared motif";
    if (contains(layout.instances[exposure.instance].rect, exposure.x, exposure.y)) continue;
    const auto passage = std::find_if(layout.passages.begin(), layout.passages.end(), [&](const auto& item) {
      return item.a != no_motif_parent && item.b == exposure.instance &&
             item.x == exposure.x && item.y == exposure.y;
    });
    if (passage == layout.passages.end())
      return "a surface exposure does not belong to its declared motif";
  }

  std::vector<uint8_t> seen(owner.size(), 0);
  std::queue<size_t> pending;
  const auto start = size_t(layout.entry_y) * width;
  pending.push(start);
  seen[start] = 1;
  while (!pending.empty()) {
    const auto index = pending.front();
    pending.pop();
    const auto x = int32_t(index % width);
    const auto y = int32_t(index / width);
    for (const auto& [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
      const auto nx = x + dx;
      const auto ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= layout.width || ny >= layout.height) continue;
      const auto next = size_t(ny) * width + size_t(nx);
      if (owner[next] == 0 || seen[next] != 0) continue;
      seen[next] = 1;
      pending.push(next);
    }
  }
  for (size_t i = 0; i < owner.size(); ++i) {
    if (owner[i] != 0 && seen[i] == 0) return "a floor measure cannot be visited from the entrance";
  }
  return {};
}

} // namespace devils_engine::originator
