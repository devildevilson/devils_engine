#ifndef DEVILS_ENGINE_PAINTER_ATLAS_LAYOUT_H
#define DEVILS_ENGINE_PAINTER_ATLAS_LAYOUT_H

#include <algorithm>
#include <cstdint>
#include <span>

namespace devils_engine::painter {

// Размещение квадратных регионов в одном atlas-изображении. Это механизм: он ничего не знает про
// каскады, spot lights и качество — только про размеры и вместимость. Политику (сколько регионов и
// каких размеров просить) задаёт потребитель.
//
// Зачем контракт: пока раскладка была захардкожена как 2x2, число каскадов физически нельзя было
// изменить, а шейдер знал раскладку сам (tile = vec2(index & 1, index >> 1)). Здесь размещение
// становится ДАННЫМИ: потребитель кладёт viewport/scissor в команду региона, а uv-трансформ — в
// запись региона, и шейдер больше не знает, как атлас устроен.
struct atlas_region {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t size = 0; // сторона в текселях
};

// uv-трансформ региона: local_uv в [0,1] переводится в atlas_uv как local * scale + offset.
struct atlas_uv_transform {
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
};

// Детерминированная укладка first-fit по сетке: регионы размещаются по убыванию размера (при равенстве
// — по индексу запроса), для каждого ищется первая свободная позиция при обходе строк сверху вниз.
// Шаг сетки — НОД всех размеров и размеров атласа, поэтому для степеней двойки укладка точная и не
// теряет место над мелкими регионами (полочная укладка теряла). Порядок результата совпадает с
// порядком запроса, а равные регионы дают ту же раскладку, что и прежняя жёсткая сетка.
// Возвращает false, если запрошенное не вмещается: сообщать об этом громко — дело вызывающей стороны,
// потому что только она знает, что именно просила.
inline bool allocate_atlas_regions(
  const uint32_t atlas_width,
  const uint32_t atlas_height,
  const std::span<const uint32_t> sizes,
  const std::span<atlas_region> out) noexcept {
  constexpr size_t max_regions = 32;
  constexpr size_t max_cells = 128;

  if (out.size() < sizes.size() || sizes.size() > max_regions || sizes.empty()) {
    return false;
  }
  if (atlas_width == 0 || atlas_height == 0) {
    return false;
  }

  const auto gcd = [](uint32_t a, uint32_t b) noexcept {
    while (b != 0) {
      const uint32_t t = a % b;
      a = b;
      b = t;
    }
    return a;
  };

  uint32_t step = gcd(atlas_width, atlas_height);
  for (const uint32_t size : sizes) {
    if (size == 0 || size > atlas_width || size > atlas_height) {
      return false;
    }
    step = gcd(step, size);
  }
  if (step == 0) {
    return false;
  }

  const uint32_t cells_x = atlas_width / step;
  const uint32_t cells_y = atlas_height / step;
  if (cells_x > max_cells || cells_y > max_cells) {
    return false; // сетка крупнее бюджета: осознанный предел, а не молчаливое усечение
  }

  bool occupied[max_cells][max_cells]{};

  uint32_t order[max_regions]{};
  for (size_t i = 0; i < sizes.size(); ++i) {
    order[i] = uint32_t(i);
  }
  std::stable_sort(order, order + sizes.size(), [&](const uint32_t a, const uint32_t b) {
    return sizes[a] > sizes[b];
  });

  for (size_t i = 0; i < sizes.size(); ++i) {
    const uint32_t index = order[i];
    const uint32_t span = sizes[index] / step;
    bool placed = false;
    for (uint32_t y = 0; y + span <= cells_y && !placed; ++y) {
      for (uint32_t x = 0; x + span <= cells_x && !placed; ++x) {
        bool free_here = true;
        for (uint32_t dy = 0; dy < span && free_here; ++dy) {
          for (uint32_t dx = 0; dx < span; ++dx) {
            if (occupied[y + dy][x + dx]) {
              free_here = false;
              break;
            }
          }
        }
        if (!free_here) {
          continue;
        }
        for (uint32_t dy = 0; dy < span; ++dy) {
          for (uint32_t dx = 0; dx < span; ++dx) {
            occupied[y + dy][x + dx] = true;
          }
        }
        out[index] = atlas_region{x * step, y * step, sizes[index]};
        placed = true;
      }
    }
    if (!placed) {
      return false;
    }
  }
  return true;
}

inline atlas_uv_transform atlas_region_uv(
  const atlas_region& region,
  const uint32_t atlas_width,
  const uint32_t atlas_height) noexcept {
  const float width = float(atlas_width == 0 ? 1 : atlas_width);
  const float height = float(atlas_height == 0 ? 1 : atlas_height);
  return atlas_uv_transform{
    float(region.size) / width,
    float(region.size) / height,
    float(region.x) / width,
    float(region.y) / height};
}

// Доля площади атласа под размещёнными регионами: единственная честная метрика «влезло ли с запасом».
inline float atlas_occupancy(
  const std::span<const atlas_region> regions,
  const uint32_t atlas_width,
  const uint32_t atlas_height) noexcept {
  if (atlas_width == 0 || atlas_height == 0) {
    return 0.0f;
  }
  double used = 0.0;
  for (const auto& region : regions) {
    used += double(region.size) * double(region.size);
  }
  return float(used / (double(atlas_width) * double(atlas_height)));
}

} // namespace devils_engine::painter

#endif
