#ifndef DEVILS_ENGINE_UTILS_KD_TREE_H
#define DEVILS_ENGINE_UTILS_KD_TREE_H

#include <array>
#include <vector>
#include <cstddef>
#include <utility>
#include <algorithm>

namespace devils_engine {
namespace utils {
// kd_tree — статическое k-d дерево над точками для запросов ближайшего/в радиусе.
// Назначение — пространственный акселератор уровня utils (своих оптимизаторов в проекте
// нет, как btDbvt в Bullet): перестраивается раз за кадр, отвечает на запросы с прунингом
// по дистанции. payload-агностичен (T — что угодно), позиция задаётся при insert.
//
// Реюз арены: clear() сохраняет capacity, insert копит точки, build() партиционирует ИХ
// ЖЕ на месте через nth_element (без аллокаций между кадрами).
//
// ВАЖНО про nearest с предикатом: поиск ОГРАНИЧЕН радиусом. Без радиуса предикат-NN, не
// нашедший кандидата (напр. «нет никого крупнее»), деградирует до полного обхода (best=∞ ⇒
// нет прунинга). Радиус и прунит, и задаёт модель ограниченного восприятия.
//
// Детерминизм: при РАВНЫХ дистанциях побеждает первый найденный (зависит от структуры
// дерева). Порядок партиционирования nth_element не стабилен для равных ключей ⇒ для
// строгого детерминизма позже (fixed-point) добавить тотальный тай-брейк по id в предикат
// или пост-обработку. Сейчас точные совпадения дистанций между разными точками редки.
template <typename T, typename Scalar = float, int Dim = 2>
class kd_tree {
public:
  using scalar = Scalar;
  using point = std::array<scalar, Dim>;

  struct node {
    point pos;
    T payload;
  };

  void reserve(const size_t n) { nodes_.reserve(n); }
  void clear() noexcept { nodes_.clear(); }          // сохраняет capacity (арена)
  size_t size() const noexcept { return nodes_.size(); }
  bool empty() const noexcept { return nodes_.empty(); }

  void insert(const point& p, const T& payload) { nodes_.push_back(node{ p, payload }); }
  void insert(const point& p, T&& payload) { nodes_.push_back(node{ p, std::move(payload) }); }

  // Балансирующее построение: медианное разбиение по чередующимся осям, на месте.
  void build() {
    if (!nodes_.empty()) build_rec(0, nodes_.size(), 0);
  }

  // Ближайший узел в пределах max_radius, чей payload проходит pred. nullptr — нет такого.
  // pred: (const T&) -> bool. max_radius ОБЯЗАТЕЛЕН (см. шапку).
  template <typename Pred>
  const node* nearest(const point& q, const scalar max_radius, Pred&& pred) const {
    if (nodes_.empty()) return nullptr;
    const node* best = nullptr;
    scalar best_d2 = max_radius * max_radius;
    nearest_rec(0, nodes_.size(), 0, q, pred, best, best_d2);
    return best;
  }

  // Два ближайших по двум предикатам за ОДИН обход с общим прунингом — вдвое дешевле
  // двух раздельных nearest(). Возвращает пару (по pa, по pb); любой может быть nullptr.
  // Дальняя ветка отсекается, только если она дальше ОБОИХ текущих лучших.
  template <typename PredA, typename PredB>
  std::pair<const node*, const node*>
  nearest2(const point& q, const scalar max_radius, PredA&& pa, PredB&& pb) const {
    const node* ba = nullptr;
    const node* bb = nullptr;
    scalar da = max_radius * max_radius;
    scalar db = da;
    if (!nodes_.empty()) nearest2_rec(0, nodes_.size(), 0, q, pa, pb, ba, da, bb, db);
    return { ba, bb };
  }

  // Обойти все узлы в радиусе r, чей payload проходит pred: visit(const node&).
  template <typename Pred, typename Visit>
  void radius(const point& q, const scalar r, Pred&& pred, Visit&& visit) const {
    if (nodes_.empty()) return;
    radius_rec(0, nodes_.size(), 0, q, r * r, pred, visit);
  }

private:
  static scalar dist2(const point& a, const point& b) noexcept {
    scalar s = 0;
    for (int i = 0; i < Dim; ++i) { const scalar d = a[i] - b[i]; s += d * d; }
    return s;
  }

  void build_rec(const size_t lo, const size_t hi, const int axis) {
    if (hi - lo <= 1) return;
    const size_t mid = lo + (hi - lo) / 2;
    std::nth_element(nodes_.begin() + lo, nodes_.begin() + mid, nodes_.begin() + hi,
      [axis](const node& a, const node& b) { return a.pos[axis] < b.pos[axis]; });
    const int next = (axis + 1) % Dim;
    build_rec(lo, mid, next);
    build_rec(mid + 1, hi, next);
  }

  template <typename Pred>
  void nearest_rec(const size_t lo, const size_t hi, const int axis, const point& q,
                   Pred& pred, const node*& best, scalar& best_d2) const {
    if (hi <= lo) return;
    const size_t mid = lo + (hi - lo) / 2;
    const node& n = nodes_[mid];

    if (pred(n.payload)) {
      const scalar d2 = dist2(n.pos, q);
      if (d2 < best_d2) { best_d2 = d2; best = &n; }
    }

    const scalar diff = q[axis] - n.pos[axis];
    const int next = (axis + 1) % Dim;
    if (diff < scalar(0)) {                              // ближняя сторона — слева
      nearest_rec(lo, mid, next, q, pred, best, best_d2);
      if (diff * diff < best_d2) nearest_rec(mid + 1, hi, next, q, pred, best, best_d2);
    } else {
      nearest_rec(mid + 1, hi, next, q, pred, best, best_d2);
      if (diff * diff < best_d2) nearest_rec(lo, mid, next, q, pred, best, best_d2);
    }
  }

  template <typename PredA, typename PredB>
  void nearest2_rec(const size_t lo, const size_t hi, const int axis, const point& q,
                    PredA& pa, PredB& pb, const node*& ba, scalar& da,
                    const node*& bb, scalar& db) const {
    if (hi <= lo) return;
    const size_t mid = lo + (hi - lo) / 2;
    const node& n = nodes_[mid];

    scalar d2 = scalar(-1); // считаем дистанцию максимум один раз на узел
    if (pa(n.payload)) { d2 = dist2(n.pos, q); if (d2 < da) { da = d2; ba = &n; } }
    if (pb(n.payload)) { if (d2 < scalar(0)) d2 = dist2(n.pos, q); if (d2 < db) { db = d2; bb = &n; } }

    const scalar diff = q[axis] - n.pos[axis];
    const int next = (axis + 1) % Dim;
    if (diff < scalar(0)) {
      nearest2_rec(lo, mid, next, q, pa, pb, ba, da, bb, db);
      if (diff * diff < (da > db ? da : db)) nearest2_rec(mid + 1, hi, next, q, pa, pb, ba, da, bb, db);
    } else {
      nearest2_rec(mid + 1, hi, next, q, pa, pb, ba, da, bb, db);
      if (diff * diff < (da > db ? da : db)) nearest2_rec(lo, mid, next, q, pa, pb, ba, da, bb, db);
    }
  }

  template <typename Pred, typename Visit>
  void radius_rec(const size_t lo, const size_t hi, const int axis, const point& q,
                  const scalar r2, Pred& pred, Visit& visit) const {
    if (hi <= lo) return;
    const size_t mid = lo + (hi - lo) / 2;
    const node& n = nodes_[mid];

    if (pred(n.payload) && dist2(n.pos, q) <= r2) visit(n);

    const scalar diff = q[axis] - n.pos[axis];
    const int next = (axis + 1) % Dim;
    if (diff < scalar(0)) {
      radius_rec(lo, mid, next, q, r2, pred, visit);
      if (diff * diff <= r2) radius_rec(mid + 1, hi, next, q, r2, pred, visit);
    } else {
      radius_rec(mid + 1, hi, next, q, r2, pred, visit);
      if (diff * diff <= r2) radius_rec(lo, mid, next, q, r2, pred, visit);
    }
  }

  std::vector<node> nodes_; // переиспользуемая арена точек (= неявное дерево после build)
};

}
}

#endif
