#ifndef DEVILS_ENGINE_UTILS_GRID_H
#define DEVILS_ENGINE_UTILS_GRID_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <gtl/phmap.hpp>

#include "geometry.h"

namespace devils_engine {
namespace utils {
// grid.h — пространственные СЕТКИ над точками с ИНКРЕМЕНТАЛЬНЫМИ add/remove/update и
// единым query(shape, visit)-контрактом (общим с kd_tree/aabb_tree, см. geometry.h).
// Две реализации:
//   dense_grid — плотная сетка с ОГРАНИЧЕННЫМИ границами; индекс ячейки вычисляется
//                арифметически (cy*width+cx), без хеша; кэш-дружелюбно.
//   hash_grid  — разрежённая сетка (хеш по целочисл. координатам ячейки); БЕЗГРАНИЧНЫЙ
//                мир, любые/отрицательные координаты.
//
// Общий механизм (detail::grid_pool): ЕДИНЫЙ массив-пул записей (без per-cell
// std::vector) с free-list (стабильные индексы, дыры переиспользуются) и ДВУСВЯЗНЫМИ
// цепочками на ячейку → insert/remove/update = O(1). Хендл grid_handle{index,gen}
// стабилен при любых add/remove соседей (gen ловит устаревший хендл на переиспользованный
// слот). compact() уплотняет пул в порядке ячеек (кэш) — но ИНВАЛИДИРУЕТ хендлы (bulk-op).
//
// Vec/Dim/UpAxis — как в kd_tree.

// Хендл записи: стабилен, пока запись жива; переживает add/remove соседей.
struct grid_handle {
  uint32_t index = std::numeric_limits<uint32_t>::max();
  uint32_t gen = 0;
  bool valid() const noexcept {
    return index != std::numeric_limits<uint32_t>::max();
  }
};

namespace detail {
// Пул записей + помощники цепочек. Владеет позициями/пейлоадами; цепочки-головы держит
// наследник (dense: массив; hash: map). Индексы записей стабильны (free-list, без сдвигов).
template <typename T, typename Vec, int Dim>
class grid_pool {
public:
  using point = Vec;
  struct entry {
    point pos;
    T payload;
    int32_t next;
    int32_t prev;
    uint32_t gen;
  };

  size_t size() const noexcept {
    return live_;
  }
  bool empty() const noexcept {
    return live_ == 0;
  }
  const entry& get(const uint32_t i) const noexcept {
    return entries_[i];
  }
  // Валиден ли хендл (жив и не устарел на переиспользованном слоте).
  bool alive(const grid_handle h) const noexcept {
    return h.valid() && h.index < entries_.size() && entries_[h.index].gen == h.gen && free_pos(h.index) == false;
  }

protected:
  void clear_pool() noexcept {
    entries_.clear();
    free_.clear();
    free_flag_.clear();
    live_ = 0;
  }
  void reserve_pool(const size_t n) {
    entries_.reserve(n);
    free_flag_.reserve(n);
  }

  // Выделить слот под запись (переиспользует дырку или растит массив). Возвращает индекс;
  // gen слота уже актуален (бампался при dealloc). Вызывающий линкует его в цепочку.
  uint32_t alloc(const point& p, T payload_in) {
    uint32_t idx;
    if (!free_.empty()) {
      idx = free_.back();
      free_.pop_back();
      free_flag_[idx] = 0;
      entries_[idx].pos = p;
      entries_[idx].payload = std::move(payload_in);
    } else {
      idx = uint32_t(entries_.size());
      entries_.push_back(entry{p, std::move(payload_in), -1, -1, 0});
      free_flag_.push_back(0);
    }
    entries_[idx].next = -1;
    entries_[idx].prev = -1;
    ++live_;
    return idx;
  }
  void dealloc(const uint32_t idx) noexcept {
    ++entries_[idx].gen;
    free_flag_[idx] = 1;
    free_.push_back(idx);
    --live_;
  }
  grid_handle handle_of(const uint32_t idx) const noexcept {
    return grid_handle{idx, entries_[idx].gen};
  }

  void link_front(int32_t& head, const uint32_t idx) noexcept {
    entries_[idx].prev = -1;
    entries_[idx].next = head;
    if (head >= 0) {
      entries_[head].prev = int32_t(idx);
    }
    head = int32_t(idx);
  }
  void unlink(int32_t& head, const uint32_t idx) noexcept {
    const int32_t p = entries_[idx].prev, n = entries_[idx].next;
    if (p >= 0) {
      entries_[p].next = n;
    } else {
      head = n;
    }
    if (n >= 0) {
      entries_[n].prev = p;
    }
  }
  template <int UpAxis, typename Shape, typename Visit>
  void visit_chain(const int32_t head, const Shape& shape, Visit& visit) const {
    for (int32_t i = head; i >= 0; i = entries_[i].next) {
      if (geom::test_contains<UpAxis>(shape, entries_[i].pos)) {
        visit(entries_[i].payload);
      }
    }
  }

  bool free_pos(const uint32_t idx) const noexcept {
    return idx < free_flag_.size() && free_flag_[idx] != 0;
  }

  std::vector<entry> entries_;     // единый пул (дыры помечены free_flag_)
  std::vector<uint32_t> free_;     // свободные слоты
  std::vector<uint8_t> free_flag_; // 1 = слот свободен
  size_t live_ = 0;
};
} // namespace detail

// ------------------------------------------------------------------ dense_grid
template <typename T, typename Vec = std::array<float, 2>, int Dim = 2, int UpAxis = 1>
class dense_grid : public detail::grid_pool<T, Vec, Dim> {
  using base = detail::grid_pool<T, Vec, Dim>;

public:
  using scalar = geom::scalar_of<Vec>;
  using point = Vec;
  using aabb = geom::aabb<Vec, Dim>;
  using cell_coord = std::array<int32_t, Dim>;

  // origin — мировой угол сетки (min); dims — число ячеек по каждой оси; cell_size — размер
  // ячейки. Точки вне сетки клампятся в краевые ячейки (краевые ячейки открыты до ±inf,
  // так что запросы их не теряют; но объекты, регулярно выходящие за границы, лучше в hash_grid).
  dense_grid(const point& origin, const cell_coord& dims, const scalar cell_size)
    : origin_(origin), dims_(dims), cell_size_(cell_size) {
    size_t n = 1;
    for (int i = 0; i < Dim; ++i) {
      n *= size_t(dims_[i] > 0 ? dims_[i] : 1);
    }
    heads_.assign(n, -1);
  }

  scalar cell_size() const noexcept {
    return cell_size_;
  }
  void clear() noexcept {
    base::clear_pool();
    std::fill(heads_.begin(), heads_.end(), int32_t(-1));
  }
  void reserve(const size_t n) {
    base::reserve_pool(n);
  }

  grid_handle insert(const point& p, const T& payload) {
    return insert_impl(p, payload);
  }
  grid_handle insert(const point& p, T&& payload) {
    return insert_impl(p, std::move(payload));
  }

  void remove(const grid_handle h) {
    if (!base::alive(h)) {
      return;
    }
    base::unlink(heads_[lin(cell_of(base::get(h.index).pos))], h.index);
    base::dealloc(h.index);
  }
  // Переместить запись; при смене ячейки перелинковывает. O(1).
  void update(const grid_handle h, const point& p) {
    if (!base::alive(h)) {
      return;
    }
    const cell_coord oc = cell_of(base::get(h.index).pos);
    const cell_coord nc = cell_of(p);
    base::entries_[h.index].pos = p;
    if (oc != nc) {
      base::unlink(heads_[lin(oc)], h.index);
      base::link_front(heads_[lin(nc)], h.index);
    }
  }

  template <typename Shape, typename Visit>
  void query(const Shape& shape, Visit&& visit) const {
    if (base::empty()) {
      return;
    }
    const aabb qb = geom::query_bounds<UpAxis>(shape);
    cell_coord lo, hi;
    for (int i = 0; i < Dim; ++i) {
      lo[i] = clamp_cell(qb.min[i], i);
      hi[i] = clamp_cell(qb.max[i], i);
    }
    cell_coord cur{};
    iterate_box(lo, hi, 0, cur, shape, visit);
  }

private:
  template <typename U>
  grid_handle insert_impl(const point& p, U&& payload) {
    const uint32_t idx = base::alloc(p, std::forward<U>(payload));
    base::link_front(heads_[lin(cell_of(p))], idx);
    return base::handle_of(idx);
  }

  cell_coord cell_of(const point& p) const noexcept {
    cell_coord c{};
    for (int i = 0; i < Dim; ++i) {
      int32_t v = int32_t(std::floor((p[i] - origin_[i]) / cell_size_));
      c[i] = std::clamp(v, int32_t(0), dims_[i] - 1);
    }
    return c;
  }
  int32_t clamp_cell(const scalar v, const int i) const noexcept {
    if (std::isinf(v)) {
      return v < scalar(0) ? 0 : dims_[i] - 1;
    }
    double c = std::floor((double(v) - double(origin_[i])) / double(cell_size_));
    return int32_t(std::clamp(c, 0.0, double(dims_[i] - 1)));
  }
  size_t lin(const cell_coord& c) const noexcept {
    size_t idx = 0, stride = 1;
    for (int i = 0; i < Dim; ++i) {
      idx += size_t(c[i]) * stride;
      stride *= size_t(dims_[i]);
    }
    return idx;
  }
  // Границы ячейки; краевые ячейки открыты наружу до ±inf (поглощают клампнутые объекты,
  // чтобы прунинг их не отбросил).
  aabb cell_bounds(const cell_coord& c) const noexcept {
    const scalar inf = std::numeric_limits<scalar>::infinity();
    aabb b{};
    for (int i = 0; i < Dim; ++i) {
      b.min[i] = (c[i] == 0) ? -inf : origin_[i] + scalar(c[i]) * cell_size_;
      b.max[i] = (c[i] == dims_[i] - 1) ? inf : origin_[i] + scalar(c[i] + 1) * cell_size_;
    }
    return b;
  }

  template <typename Shape, typename Visit>
  void iterate_box(const cell_coord& lo, const cell_coord& hi, const int axis, cell_coord& cur,
                   const Shape& shape, Visit& visit) const {
    if (axis == Dim) {
      const int32_t head = heads_[lin(cur)];
      if (head >= 0 && geom::test_overlaps<UpAxis>(shape, cell_bounds(cur))) {
        base::template visit_chain<UpAxis>(head, shape, visit);
      }
      return;
    }
    for (cur[axis] = lo[axis]; cur[axis] <= hi[axis]; ++cur[axis]) {
      iterate_box(lo, hi, axis + 1, cur, shape, visit);
    }
  }

  point origin_;
  cell_coord dims_;
  scalar cell_size_;
  std::vector<int32_t> heads_; // голова цепочки на ячейку (линейный индекс), -1 = пусто
};

// ------------------------------------------------------------------ hash_grid
template <typename T, typename Vec = std::array<float, 2>, int Dim = 2, int UpAxis = 1>
class hash_grid : public detail::grid_pool<T, Vec, Dim> {
  using base = detail::grid_pool<T, Vec, Dim>;

public:
  using scalar = geom::scalar_of<Vec>;
  using point = Vec;
  using aabb = geom::aabb<Vec, Dim>;
  using cell_coord = std::array<int32_t, Dim>;

  explicit hash_grid(const scalar cell_size = scalar(1)) : cell_size_(cell_size) {}

  scalar cell_size() const noexcept {
    return cell_size_;
  }
  void set_cell_size(const scalar s) {
    if (!base::empty()) {
      clear();
    }
    cell_size_ = s;
  }
  void clear() noexcept {
    base::clear_pool();
    heads_.clear();
    occ_min_ = cell_coord{};
    occ_max_ = cell_coord{};
  }
  void reserve(const size_t n) {
    base::reserve_pool(n);
  }

  grid_handle insert(const point& p, const T& payload) {
    return insert_impl(p, payload);
  }
  grid_handle insert(const point& p, T&& payload) {
    return insert_impl(p, std::move(payload));
  }

  void remove(const grid_handle h) {
    if (!base::alive(h)) {
      return;
    }
    unlink_from_cell(cell_of(base::get(h.index).pos), h.index);
    base::dealloc(h.index);
  }
  void update(const grid_handle h, const point& p) {
    if (!base::alive(h)) {
      return;
    }
    const cell_coord oc = cell_of(base::get(h.index).pos);
    const cell_coord nc = cell_of(p);
    base::entries_[h.index].pos = p;
    if (oc != nc) {
      unlink_from_cell(oc, h.index);
      link_to_cell(nc, h.index);
    }
  }

  template <typename Shape, typename Visit>
  void query(const Shape& shape, Visit&& visit) const {
    if (base::empty()) {
      return;
    }
    const aabb qb = geom::query_bounds<UpAxis>(shape);
    cell_coord lo, hi;
    for (int i = 0; i < Dim; ++i) {
      const int32_t l = clamp_axis(qb.min[i], i, true);
      const int32_t h = clamp_axis(qb.max[i], i, false);
      if (l > h) {
        return;
      }
      lo[i] = l;
      hi[i] = h;
    }
    double box_cells = 1.0;
    for (int i = 0; i < Dim; ++i) {
      box_cells *= double(hi[i] - lo[i] + 1);
    }
    if (box_cells > double(heads_.size())) { // перебор занятых ячеек дешевле
      for (const auto& [c, head] : heads_) {
        bool inside = true;
        for (int i = 0; i < Dim; ++i) {
          if (c[i] < lo[i] || c[i] > hi[i]) {
            inside = false;
            break;
          }
        }
        if (inside && geom::test_overlaps<UpAxis>(shape, cell_bounds(c))) {
          base::template visit_chain<UpAxis>(head, shape, visit);
        }
      }
    } else {
      cell_coord cur{};
      iterate_box(lo, hi, 0, cur, shape, visit);
    }
  }

private:
  template <typename U>
  grid_handle insert_impl(const point& p, U&& payload) {
    const uint32_t idx = base::alloc(p, std::forward<U>(payload));
    const cell_coord c = cell_of(p);
    link_to_cell(c, idx);
    if (base::size() == 1) {
      occ_min_ = c;
      occ_max_ = c;
    } else {
      for (int i = 0; i < Dim; ++i) {
        occ_min_[i] = std::min(occ_min_[i], c[i]);
        occ_max_[i] = std::max(occ_max_[i], c[i]);
      }
    }
    return base::handle_of(idx);
  }

  cell_coord cell_of(const point& p) const noexcept {
    cell_coord c{};
    for (int i = 0; i < Dim; ++i) {
      c[i] = int32_t(std::floor(p[i] / cell_size_));
    }
    return c;
  }
  aabb cell_bounds(const cell_coord& c) const noexcept {
    aabb b{};
    for (int i = 0; i < Dim; ++i) {
      b.min[i] = scalar(c[i]) * cell_size_;
      b.max[i] = scalar(c[i] + 1) * cell_size_;
    }
    return b;
  }
  int32_t clamp_axis(const scalar v, const int i, const bool is_min) const noexcept {
    if (std::isinf(v)) {
      return is_min ? occ_min_[i] : occ_max_[i];
    }
    double c = std::floor(double(v) / double(cell_size_));
    c = std::clamp(c, double(occ_min_[i]), double(occ_max_[i]));
    return int32_t(c);
  }
  void link_to_cell(const cell_coord& c, const uint32_t idx) {
    // try_emplace инициализирует голову НОВОЙ ячейки в -1 (operator[] дал бы 0 = валидный индекс!).
    auto [it, inserted] = heads_.try_emplace(c, int32_t(-1));
    static_cast<void>(inserted);
    base::link_front(it->second, idx);
  }
  void unlink_from_cell(const cell_coord& c, const uint32_t idx) {
    const auto it = heads_.find(c);
    if (it == heads_.end()) {
      return;
    }
    base::unlink(it->second, idx);
    if (it->second < 0) {
      heads_.erase(it); // ячейка опустела
    }
  }

  template <typename Shape, typename Visit>
  void iterate_box(const cell_coord& lo, const cell_coord& hi, const int axis, cell_coord& cur,
                   const Shape& shape, Visit& visit) const {
    if (axis == Dim) {
      const auto it = heads_.find(cur);
      if (it != heads_.end() && geom::test_overlaps<UpAxis>(shape, cell_bounds(cur))) {
        base::template visit_chain<UpAxis>(it->second, shape, visit);
      }
      return;
    }
    for (cur[axis] = lo[axis]; cur[axis] <= hi[axis]; ++cur[axis]) {
      iterate_box(lo, hi, axis + 1, cur, shape, visit);
    }
  }

  struct coord_hash {
    size_t operator()(const cell_coord& c) const noexcept {
      size_t h = 1469598103934665603ull;
      for (int i = 0; i < Dim; ++i) {
        h ^= size_t(uint32_t(c[i]));
        h *= 1099511628211ull;
      }
      return h;
    }
  };

  scalar cell_size_;
  cell_coord occ_min_{}, occ_max_{};
  gtl::flat_hash_map<cell_coord, int32_t, coord_hash> heads_;
};

} // namespace utils
} // namespace devils_engine

#endif
