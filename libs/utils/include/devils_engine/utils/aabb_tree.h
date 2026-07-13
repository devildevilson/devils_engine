#ifndef DEVILS_ENGINE_UTILS_AABB_TREE_H
#define DEVILS_ENGINE_UTILS_AABB_TREE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "geometry.h"

namespace devils_engine {
namespace utils {
// aabb_tree — ДИНАМИЧЕСКИЙ BVH над РАСШИРЕННЫМИ объектами (у каждого свой AABB), в стиле
// Box2D b2DynamicTree / Bullet btDbvt. В отличие от kd_tree/*_grid (точки), хранит тела и
// поддерживает ИНКРЕМЕНТАЛЬНЫЕ add/remove/update по стабильному хендлу без полной
// перестройки: insert ищет лучшего соседа по SAH и рефитит предков, remove схлопывает
// родителя, update = detach+attach. Балансировка вращениями (AVL-подобная) держит высоту.
//
// Тот же query(shape, visit)-контракт (см. geometry.h). ОТЛИЧИЕ: прунинг узла И тест листа —
// overlaps(shape, box) (объект — коробка). Здесь ray ОСМЫСЛЕН (пересечение с телами).
// Лист хранит ТОЧНЫЙ AABB объекта (без «жирного» запаса) → query точен.
//
// Хендл bvh_handle стабилен при insert/remove/update соседей; rebuild() (bulk-переоптимизация)
// ИНВАЛИДИРУЕТ хендлы. Vec/Dim/UpAxis — как в kd_tree.
struct bvh_handle {
  uint32_t index = std::numeric_limits<uint32_t>::max();
  uint32_t gen = 0;
  bool valid() const noexcept {
    return index != std::numeric_limits<uint32_t>::max();
  }
};

template <typename T, typename Vec = std::array<float, 2>, int Dim = 2, int UpAxis = 1>
class aabb_tree {
public:
  using scalar = geom::scalar_of<Vec>;
  using point = Vec;
  using aabb = geom::aabb<Vec, Dim>;

  size_t size() const noexcept {
    return leaf_count_;
  }
  bool empty() const noexcept {
    return leaf_count_ == 0;
  }
  void reserve(const size_t n) {
    nodes_.reserve(n * 2);
  }
  void clear() noexcept {
    nodes_.clear();
    free_.clear();
    root_ = null;
    leaf_count_ = 0;
  }

  bool alive(const bvh_handle h) const noexcept {
    return h.valid() && h.index < nodes_.size() && nodes_[h.index].height != free_mark && nodes_[h.index].gen == h.gen;
  }

  bvh_handle insert(const aabb& box, const T& payload) {
    return insert_impl(box, payload);
  }
  bvh_handle insert(const aabb& box, T&& payload) {
    return insert_impl(box, std::move(payload));
  }

  void remove(const bvh_handle h) {
    if (!alive(h) || !is_leaf(h.index)) {
      return;
    }
    detach_leaf(h.index);
    free_node(h.index);
    --leaf_count_;
  }
  // Переместить/изменить AABB объекта; хендл СОХРАНЯЕТСЯ. detach + attach.
  void update(const bvh_handle h, const aabb& box) {
    if (!alive(h) || !is_leaf(h.index)) {
      return;
    }
    detach_leaf(h.index);
    nodes_[h.index].box = box;
    attach_leaf(h.index);
  }

  template <typename Shape, typename Visit>
  void query(const Shape& shape, Visit&& visit) const {
    if (root_ == null) {
      return;
    }
    int32_t stack[64];
    int sp = 0;
    stack[sp++] = root_;
    while (sp > 0) {
      const int32_t idx = stack[--sp];
      const node& n = nodes_[idx];
      if (!geom::test_overlaps<UpAxis>(shape, n.box)) {
        continue;
      }
      if (is_leaf(idx)) {
        visit(n.payload);
        continue;
      }
      // защита от переполнения стека (высота ограничена балансировкой; 64 → ~огромное дерево)
      if (sp + 2 <= int(sizeof(stack) / sizeof(stack[0]))) {
        stack[sp++] = n.child1;
        stack[sp++] = n.child2;
      } else {
        query(shape, visit, n.child1);
        query(shape, visit, n.child2);
      } // редкий фолбэк на рекурсию
    }
  }

  // Bulk-переоптимизация: собрать листья и переинсертить (лучше качество после многих
  // update). ИНВАЛИДИРУЕТ все хендлы.
  void rebuild() {
    std::vector<std::pair<aabb, T>> leaves;
    leaves.reserve(leaf_count_);
    for (const auto& n : nodes_) {
      if (n.height != free_mark && n.child1 == null) {
        leaves.push_back({n.box, n.payload});
      }
    }
    clear();
    for (auto& l : leaves) {
      insert_impl(l.first, std::move(l.second));
    }
  }

private:
  static constexpr int32_t null = -1;
  static constexpr int32_t free_mark = -2; // height == free_mark ⇒ узел в свободном списке
  // Поля по убыванию размера. Для листа: child1==null, payload валиден. Для внутреннего:
  // payload не используется. Для свободного: height==free_mark, parent = следующий свободный.
  struct node {
    aabb box;
    T payload;
    int32_t parent;
    int32_t child1;
    int32_t child2;
    int32_t height;
    uint32_t gen;
  };

  bool is_leaf(const int32_t i) const noexcept {
    return nodes_[i].child1 == null;
  }

  int32_t alloc_node() {
    if (!free_.empty()) {
      const int32_t i = free_.back();
      free_.pop_back();
      return i;
    }
    const int32_t i = int32_t(nodes_.size());
    nodes_.push_back(node{aabb::empty(), T{}, null, null, null, 0, 0});
    return i;
  }
  void free_node(const int32_t i) noexcept {
    ++nodes_[i].gen;
    nodes_[i].height = free_mark;
    nodes_[i].parent = null;
    free_.push_back(i);
  }

  template <typename U>
  bvh_handle insert_impl(const aabb& box, U&& payload) {
    const int32_t leaf = alloc_node();
    nodes_[leaf].box = box;
    nodes_[leaf].payload = std::forward<U>(payload);
    nodes_[leaf].child1 = null;
    nodes_[leaf].child2 = null;
    nodes_[leaf].height = 0;
    attach_leaf(leaf);
    ++leaf_count_;
    return bvh_handle{uint32_t(leaf), nodes_[leaf].gen};
  }

  // Вставить уже выделенный лист (box заполнен) — поиск лучшего соседа по SAH + рефит вверх.
  void attach_leaf(const int32_t leaf) {
    if (root_ == null) {
      root_ = leaf;
      nodes_[leaf].parent = null;
      return;
    }

    const aabb leaf_box = nodes_[leaf].box;
    int32_t index = root_;
    while (!is_leaf(index)) {
      const int32_t c1 = nodes_[index].child1, c2 = nodes_[index].child2;
      const scalar area = nodes_[index].box.surface();
      const aabb combined = geom::merge(nodes_[index].box, leaf_box);
      const scalar combined_area = combined.surface();
      const scalar cost = scalar(2) * combined_area;             // цена нового родителя здесь
      const scalar inherit = scalar(2) * (combined_area - area); // доп. цена спуска
      const scalar cost1 = descent_cost(c1, leaf_box, inherit);
      const scalar cost2 = descent_cost(c2, leaf_box, inherit);
      if (cost < cost1 && cost < cost2) {
        break;
      }
      index = cost1 < cost2 ? c1 : c2;
    }
    const int32_t sibling = index;

    // Новый родитель на место sibling.
    const int32_t old_parent = nodes_[sibling].parent;
    const int32_t new_parent = alloc_node();
    nodes_[new_parent].parent = old_parent;
    nodes_[new_parent].box = geom::merge(leaf_box, nodes_[sibling].box);
    nodes_[new_parent].height = nodes_[sibling].height + 1;
    nodes_[new_parent].child1 = sibling;
    nodes_[new_parent].child2 = leaf;
    nodes_[sibling].parent = new_parent;
    nodes_[leaf].parent = new_parent;
    if (old_parent != null) {
      if (nodes_[old_parent].child1 == sibling) {
        nodes_[old_parent].child1 = new_parent;
      } else {
        nodes_[old_parent].child2 = new_parent;
      }
    } else {
      root_ = new_parent;
    }

    refit_from(nodes_[leaf].parent);
  }

  // Отцепить лист (не освобождая узел): схлопнуть его родителя, рефит вверх.
  void detach_leaf(const int32_t leaf) {
    if (leaf == root_) {
      root_ = null;
      nodes_[leaf].parent = null;
      return;
    }
    const int32_t parent = nodes_[leaf].parent;
    const int32_t grand = nodes_[parent].parent;
    const int32_t sibling = nodes_[parent].child1 == leaf ? nodes_[parent].child2 : nodes_[parent].child1;
    if (grand != null) {
      if (nodes_[grand].child1 == parent) {
        nodes_[grand].child1 = sibling;
      } else {
        nodes_[grand].child2 = sibling;
      }
      nodes_[sibling].parent = grand;
      free_node(parent);
      refit_from(grand);
    } else {
      root_ = sibling;
      nodes_[sibling].parent = null;
      free_node(parent);
    }
    nodes_[leaf].parent = null;
  }

  scalar descent_cost(const int32_t child, const aabb& leaf_box, const scalar inherit) const noexcept {
    const aabb merged = geom::merge(leaf_box, nodes_[child].box);
    if (is_leaf(child)) {
      return merged.surface() + inherit;
    }
    return (merged.surface() - nodes_[child].box.surface()) + inherit; // прирост площади
  }

  void refit_from(int32_t index) {
    while (index != null) {
      index = balance(index);
      const int32_t c1 = nodes_[index].child1, c2 = nodes_[index].child2;
      nodes_[index].height = 1 + std::max(nodes_[c1].height, nodes_[c2].height);
      nodes_[index].box = geom::merge(nodes_[c1].box, nodes_[c2].box);
      index = nodes_[index].parent;
    }
  }

  // AVL-подобная балансировка поддерева iA вращением (Box2D). Возвращает новый корень поддерева.
  int32_t balance(const int32_t iA) {
    if (is_leaf(iA) || nodes_[iA].height < 2) {
      return iA;
    }
    const int32_t iB = nodes_[iA].child1, iC = nodes_[iA].child2;
    const int32_t bal = nodes_[iC].height - nodes_[iB].height;

    if (bal > 1) {
      return rotate_up(iA, iC, iB, /*C is child2*/ true); // поднять C
    }
    if (bal < -1) {
      return rotate_up(iA, iB, iC, /*B is child1*/ false); // поднять B
    }
    return iA;
  }

  // Обобщённое вращение: поднять iPivot (тяжёлого ребёнка A) над A; iOther — лёгкий ребёнок A.
  // pivot_is_child2 — был ли pivot вторым ребёнком A (иначе первым).
  int32_t rotate_up(const int32_t iA, const int32_t iP, const int32_t iOther, const bool pivot_is_child2) {
    const int32_t iF = nodes_[iP].child1, iG = nodes_[iP].child2;
    // P встаёт на место A.
    nodes_[iP].parent = nodes_[iA].parent;
    (pivot_is_child2 ? nodes_[iP].child1 : nodes_[iP].child2) = iA;
    nodes_[iA].parent = iP;
    const int32_t old = nodes_[iP].parent; // = старый родитель A
    if (old != null) {
      if (nodes_[old].child1 == iA) {
        nodes_[old].child1 = iP;
      } else {
        nodes_[old].child2 = iP;
      }
    } else {
      root_ = iP;
    }

    // Из двух детей pivot тяжёлый остаётся у pivot, лёгкий уходит к A.
    const bool f_heavier = nodes_[iF].height > nodes_[iG].height;
    const int32_t heavy = f_heavier ? iF : iG;
    const int32_t light = f_heavier ? iG : iF;
    (pivot_is_child2 ? nodes_[iP].child2 : nodes_[iP].child1) = heavy;
    (pivot_is_child2 ? nodes_[iA].child2 : nodes_[iA].child1) = light;
    nodes_[light].parent = iA;

    nodes_[iA].box = geom::merge(nodes_[iOther].box, nodes_[light].box);
    nodes_[iP].box = geom::merge(nodes_[iA].box, nodes_[heavy].box);
    nodes_[iA].height = 1 + std::max(nodes_[iOther].height, nodes_[light].height);
    nodes_[iP].height = 1 + std::max(nodes_[iA].height, nodes_[heavy].height);
    return iP;
  }

  // Рекурсивный фолбэк query (используется лишь при переполнении явного стека).
  template <typename Shape, typename Visit>
  void query(const Shape& shape, Visit& visit, const int32_t idx) const {
    const node& n = nodes_[idx];
    if (!geom::test_overlaps<UpAxis>(shape, n.box)) {
      return;
    }
    if (is_leaf(idx)) {
      visit(n.payload);
      return;
    }
    query(shape, visit, n.child1);
    query(shape, visit, n.child2);
  }

  std::vector<node> nodes_;
  std::vector<int32_t> free_;
  int32_t root_ = null;
  size_t leaf_count_ = 0;
};

} // namespace utils
} // namespace devils_engine

#endif
