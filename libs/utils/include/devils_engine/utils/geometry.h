#ifndef DEVILS_ENGINE_UTILS_GEOMETRY_H
#define DEVILS_ENGINE_UTILS_GEOMETRY_H

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace devils_engine {
namespace utils {
// geometry.h — dependency-free (utils не тянет glm) геометрические примитивы + предикаты
// для пространственных структур (kd_tree / cell_list / aabb_tree).
//
// Вектор/точка — это ШАБЛОННЫЙ параметр Vec (тип математического вектора). В utils по
// умолчанию std::array<float,Dim>, но потребитель подставляет свой тип (в перспективе
// glm::vec4 вне зависимости от 2D/3D). Требования к Vec: default-конструируемость и
// operator[](int) -> (ссылка на) скаляр. Dim = число ЗНАЧИМЫХ осей (может быть МЕНЬШЕ
// числа компонент Vec: vec4 при Dim=2 использует только [0],[1]). Scalar выводится из Vec.
//
// КОНТРАКТ для структур: каждая форма умеет два теста —
//   overlaps(shape, aabb)   — пересекается ли форма с осевой коробкой; это И прунинг
//                             узлов дерева, И тест листа-коробки (aabb_tree);
//   contains(shape, point)  — лежит ли точка внутри формы; тест листа точечных
//                             структур (kd_tree, cell_list).
// Запрос query(shape, visit) в любой структуре строится только на этих двух функциях.
//
// overlaps ОБЯЗАН быть консервативным (никогда не отвергать регион, который МОЖЕТ
// содержать попадание) — иначе структура пропустит объекты. Точный лист-тест
// (contains для точки / overlaps для коробки) отсекает ложные срабатывания прунинга.
namespace geom {

// Скаляр вектора Vec — тип, который отдаёт operator[].
template <typename Vec>
using scalar_of = std::remove_cvref_t<decltype(std::declval<const Vec&>()[0])>;

// ------------------------------------------------------------------ vec-хелперы
// Dim передаётся явно (не выводится из Vec); Vec выводится из аргументов.
template <int Dim, typename Vec>
Vec vsub(const Vec& a, const Vec& b) noexcept {
  Vec r{};
  for (int i = 0; i < Dim; ++i) {
    r[i] = a[i] - b[i];
  }
  return r;
}
template <int Dim, typename Vec>
Vec vadd(const Vec& a, const Vec& b) noexcept {
  Vec r{};
  for (int i = 0; i < Dim; ++i) {
    r[i] = a[i] + b[i];
  }
  return r;
}
template <int Dim, typename Vec>
scalar_of<Vec> vdot(const Vec& a, const Vec& b) noexcept {
  scalar_of<Vec> s = scalar_of<Vec>(0);
  for (int i = 0; i < Dim; ++i) {
    s += a[i] * b[i];
  }
  return s;
}
template <int Dim, typename Vec>
scalar_of<Vec> vlen2(const Vec& a) noexcept {
  return vdot<Dim>(a, a);
}

// Единичный вектор по оси axis (компоненты [0,Dim) занулены явно — Vec может не
// зануляться при {} для типов вроде glm без FORCE_CTOR_INIT).
template <int Dim, typename Vec>
Vec unit_axis(const int axis) noexcept {
  Vec e{};
  for (int i = 0; i < Dim; ++i) {
    e[i] = scalar_of<Vec>(i == axis ? 1 : 0);
  }
  return e;
}

// ------------------------------------------------------------------ примитивы
// AABB — осевая коробка [min, max]. Инвариант: min[i] <= max[i].
template <typename Vec, int Dim>
struct aabb {
  using scalar = scalar_of<Vec>;
  using point = Vec;
  point min, max;

  point center() const noexcept {
    point c{};
    for (int i = 0; i < Dim; ++i) {
      c[i] = (min[i] + max[i]) / scalar(2);
    }
    return c;
  }
  point extent() const noexcept {
    point e{};
    for (int i = 0; i < Dim; ++i) {
      e[i] = max[i] - min[i];
    }
    return e;
  }
  point half() const noexcept {
    point h{};
    for (int i = 0; i < Dim; ++i) {
      h[i] = (max[i] - min[i]) / scalar(2);
    }
    return h;
  }
  // "Площадь поверхности" (D=3) / периметр (D=2) — метрика SAH для aabb_tree.
  scalar surface() const noexcept {
    const point e = extent();
    if constexpr (Dim == 2) {
      return scalar(2) * (e[0] + e[1]);
    } else if constexpr (Dim == 3) {
      return scalar(2) * (e[0] * e[1] + e[1] * e[2] + e[2] * e[0]);
    } else {
      scalar s = scalar(0);
      for (int i = 0; i < Dim; ++i) {
        s += e[i];
      }
      return s;
    }
  }
  scalar volume() const noexcept {
    scalar v = scalar(1);
    for (int i = 0; i < Dim; ++i) {
      v *= (max[i] - min[i]);
    }
    return v;
  }

  static aabb empty() noexcept { // "перевёрнутая" коробка-нейтраль для expand/merge
    aabb b{};
    const scalar hi = std::numeric_limits<scalar>::max();
    for (int i = 0; i < Dim; ++i) {
      b.min[i] = hi;
      b.max[i] = -hi;
    }
    return b;
  }
  static aabb from_center_half(const point& c, const point& h) noexcept {
    aabb b{};
    for (int i = 0; i < Dim; ++i) {
      b.min[i] = c[i] - h[i];
      b.max[i] = c[i] + h[i];
    }
    return b;
  }
  void expand(const point& p) noexcept {
    for (int i = 0; i < Dim; ++i) {
      min[i] = std::min<scalar>(min[i], p[i]);
      max[i] = std::max<scalar>(max[i], p[i]);
    }
  }
  void expand(const aabb& o) noexcept {
    for (int i = 0; i < Dim; ++i) {
      min[i] = std::min<scalar>(min[i], o.min[i]);
      max[i] = std::max<scalar>(max[i], o.max[i]);
    }
  }
};

template <typename Vec, int Dim>
aabb<Vec, Dim> merge(const aabb<Vec, Dim>& a, const aabb<Vec, Dim>& b) noexcept {
  aabb<Vec, Dim> r = a;
  r.expand(b);
  return r;
}

// Sphere — шар (3D) / диск (2D). radius-запрос в 2D сводится сюда.
template <typename Vec, int Dim>
struct sphere {
  using scalar = scalar_of<Vec>;
  using point = Vec;
  point center;
  scalar radius;
};

// up_cylinder — бесконечный вдоль UP-оси цилиндр (radius-запрос в 3D: "вся колонна").
// UP-ось задаётся ШАБЛОНОМ структуры (kd_tree<...,UpAxis>) → предикаты принимают UpAxis
// нетиповым параметром. В 2D бессмыслен (там radius = sphere).
template <typename Vec, int Dim>
struct up_cylinder {
  using scalar = scalar_of<Vec>;
  using point = Vec;
  point center;
  scalar radius;
};

// Ray — луч: начало + НОРМАЛИЗОВАННОЕ направление + дальность (tmax может быть inf).
template <typename Vec, int Dim>
struct ray {
  using scalar = scalar_of<Vec>;
  using point = Vec;
  point origin;
  point dir;
  scalar tmax = std::numeric_limits<scalar>::infinity();
};

// Cylinder — направленный цилиндр с ПЛОСКИМИ торцами (= "луч с радиусом", конечный).
// Внутри ⇔ 0 <= dot(p-origin, dir) <= length И радиальная дистанция <= radius.
template <typename Vec, int Dim>
struct cylinder {
  using scalar = scalar_of<Vec>;
  using point = Vec;
  point origin;
  point dir;
  scalar length;
  scalar radius; // dir нормализован
};

// OBB — ориентированная коробка: центр + полуразмеры + ортонормированные локальные оси
// (axis[i] — направление i-й полуоси в мире). "AABB + ориентация".
template <typename Vec, int Dim>
struct obb {
  using scalar = scalar_of<Vec>;
  using point = Vec;
  point center;
  point half;
  std::array<point, Dim> axis;

  static obb from_aabb(const aabb<Vec, Dim>& b, const std::array<point, Dim>& a) noexcept {
    obb o{};
    o.center = b.center();
    o.half = b.half();
    o.axis = a;
    return o;
  }
};

// ------------------------------------------------------------------ contains(shape, point)
// (точный тест листа для точечных структур)
template <typename Vec, int Dim>
bool contains(const aabb<Vec, Dim>& b, const Vec& p) noexcept {
  for (int i = 0; i < Dim; ++i) {
    if (p[i] < b.min[i] || p[i] > b.max[i]) {
      return false;
    }
  }
  return true;
}
template <typename Vec, int Dim>
bool contains(const sphere<Vec, Dim>& s, const Vec& p) noexcept {
  return vlen2<Dim>(vsub<Dim>(p, s.center)) <= s.radius * s.radius;
}
template <int UpAxis, typename Vec, int Dim>
bool contains(const up_cylinder<Vec, Dim>& c, const Vec& p) noexcept {
  using S = scalar_of<Vec>;
  S d2 = S(0); // дистанция в НЕ-up осях
  for (int i = 0; i < Dim; ++i) {
    if (i == UpAxis) {
      continue;
    }
    const S d = p[i] - c.center[i];
    d2 += d * d;
  }
  return d2 <= c.radius * c.radius;
}
template <typename Vec, int Dim>
bool contains(const cylinder<Vec, Dim>& c, const Vec& p) noexcept {
  using S = scalar_of<Vec>;
  const Vec rel = vsub<Dim>(p, c.origin);
  const S t = vdot<Dim>(rel, c.dir); // проекция на ось
  if (t < S(0) || t > c.length) {
    return false;
  }
  const S perp2 = vlen2<Dim>(rel) - t * t; // квадрат радиальной дистанции
  return perp2 <= c.radius * c.radius;
}
template <typename Vec, int Dim>
bool contains(const obb<Vec, Dim>& o, const Vec& p) noexcept {
  using S = scalar_of<Vec>;
  const Vec rel = vsub<Dim>(p, o.center);
  for (int i = 0; i < Dim; ++i) {
    const S d = vdot<Dim>(rel, o.axis[i]);
    if (d < -o.half[i] || d > o.half[i]) {
      return false;
    }
  }
  return true;
}
// Луч по точке вырожден (мера ноль). Для точечных структур используйте cylinder
// (луч с радиусом). Оставлено ради полноты перегрузок query(); всегда false.
template <typename Vec, int Dim>
bool contains(const ray<Vec, Dim>&, const Vec&) noexcept {
  return false;
}

// ------------------------------------------------------------------ overlaps(shape, aabb)
// (прунинг узлов + тест листа-коробки; ДОЛЖЕН быть консервативным)

// Квадрат дистанции от точки c до коробки b (0 если внутри); skip_axis игнорируется
// (для up_cylinder). Общий помощник для sphere/up_cylinder.
template <int Dim, typename Vec>
scalar_of<Vec> closest_dist2(const aabb<Vec, Dim>& b, const Vec& c, const int skip_axis) noexcept {
  using S = scalar_of<Vec>;
  S d2 = S(0);
  for (int i = 0; i < Dim; ++i) {
    if (i == skip_axis) {
      continue;
    }
    const S v = c[i] < b.min[i] ? b.min[i] - c[i] : (c[i] > b.max[i] ? c[i] - b.max[i] : S(0));
    d2 += v * v;
  }
  return d2;
}

template <typename Vec, int Dim>
bool overlaps(const aabb<Vec, Dim>& a, const aabb<Vec, Dim>& b) noexcept {
  for (int i = 0; i < Dim; ++i) {
    if (a.max[i] < b.min[i] || a.min[i] > b.max[i]) {
      return false;
    }
  }
  return true;
}
template <typename Vec, int Dim>
bool overlaps(const sphere<Vec, Dim>& s, const aabb<Vec, Dim>& b) noexcept {
  return closest_dist2<Dim>(b, s.center, -1) <= s.radius * s.radius; // точный
}
template <int UpAxis, typename Vec, int Dim>
bool overlaps(const up_cylinder<Vec, Dim>& c, const aabb<Vec, Dim>& b) noexcept {
  return closest_dist2<Dim>(b, c.center, UpAxis) <= c.radius * c.radius; // up-ось игнор — точный
}
// Луч × AABB — стандартный slab-тест с учётом tmax (точный).
template <typename Vec, int Dim>
bool overlaps(const ray<Vec, Dim>& r, const aabb<Vec, Dim>& b) noexcept {
  using S = scalar_of<Vec>;
  S t0 = S(0), t1 = r.tmax;
  for (int i = 0; i < Dim; ++i) {
    if (std::abs(r.dir[i]) < std::numeric_limits<S>::epsilon()) {
      if (r.origin[i] < b.min[i] || r.origin[i] > b.max[i]) {
        return false; // параллельно и снаружи
      }
    } else {
      const S inv = S(1) / r.dir[i];
      S tn = (b.min[i] - r.origin[i]) * inv;
      S tf = (b.max[i] - r.origin[i]) * inv;
      if (tn > tf) {
        std::swap(tn, tf);
      }
      t0 = tn > t0 ? tn : t0;
      t1 = tf < t1 ? tf : t1;
      if (t0 > t1) {
        return false;
      }
    }
  }
  return true;
}
// Цилиндр × AABB — КОНСЕРВАТИВНО через bounding-box отрезка, расширенный на radius.
// (Точный cylinder-AABB дорог; лист-тест contains(cylinder,point) точен, over-visit
// безопасен. Уточнение — segment/box distance — можно добавить позже.)
template <typename Vec, int Dim>
bool overlaps(const cylinder<Vec, Dim>& c, const aabb<Vec, Dim>& b) noexcept {
  aabb<Vec, Dim> cb = aabb<Vec, Dim>::empty();
  cb.expand(c.origin);
  Vec tip{};
  for (int i = 0; i < Dim; ++i) {
    tip[i] = c.origin[i] + c.dir[i] * c.length;
  }
  cb.expand(tip);
  for (int i = 0; i < Dim; ++i) {
    cb.min[i] -= c.radius;
    cb.max[i] += c.radius;
  }
  return overlaps(cb, b);
}
// Радиус проекции коробки с полуосями half и осями ax на направление L:
//   sum_i half[i] * |dot(ax[i], L)|.
template <int Dim, typename Vec>
scalar_of<Vec> proj_radius(const Vec& half, const std::array<Vec, Dim>& ax, const Vec& L) noexcept {
  using S = scalar_of<Vec>;
  S r = S(0);
  for (int i = 0; i < Dim; ++i) {
    r += half[i] * std::abs(vdot<Dim>(ax[i], L));
  }
  return r;
}
// OBB × AABB — SAT (точный). AABB трактуется как OBB с единичными осями.
template <typename Vec, int Dim>
bool overlaps(const obb<Vec, Dim>& o, const aabb<Vec, Dim>& b) noexcept {
  static_assert(Dim == 2 || Dim == 3, "OBB overlap реализован только для 2D/3D");
  using S = scalar_of<Vec>;
  const Vec bcenter = b.center();
  const Vec bhalf = b.half();
  std::array<Vec, Dim> baxis{};
  for (int i = 0; i < Dim; ++i) {
    baxis[i] = unit_axis<Dim, Vec>(i);
  }
  const Vec t = vsub<Dim>(bcenter, o.center); // вектор между центрами

  auto separated = [&](const Vec& L) noexcept -> bool {
    const S dist = std::abs(vdot<Dim>(t, L));
    return dist > proj_radius<Dim>(o.half, o.axis, L) + proj_radius<Dim>(bhalf, baxis, L);
  };

  for (int i = 0; i < Dim; ++i) {
    if (separated(o.axis[i])) {
      return false; // грани OBB
    }
  }
  for (int i = 0; i < Dim; ++i) {
    if (separated(baxis[i])) {
      return false; // грани AABB
    }
  }
  if constexpr (Dim == 3) { // + 9 векторных произведений рёбер
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        const Vec& a = o.axis[i];
        const Vec& e = baxis[j];
        Vec L{};
        L[0] = a[1] * e[2] - a[2] * e[1];
        L[1] = a[2] * e[0] - a[0] * e[2];
        L[2] = a[0] * e[1] - a[1] * e[0];
        if (vlen2<3>(L) < std::numeric_limits<S>::epsilon()) {
          continue; // почти параллельны — ось вырождена
        }
        if (separated(L)) {
          return false;
        }
      }
    }
  }
  return true;
}

// ------------------------------------------------------------------ inflate(shape, margin)
// Раздутие формы на margin (сумма Минковского с шаром радиуса margin). Нужно для
// запросов по ТОЧЕЧНЫМ структурам (kd_tree/*_grid), когда у payload есть СВОЙ габарит:
// "тело радиуса r пересекает shape" ⟺ "центр тела внутри inflate(shape, r)". Точно для
// шаро-тел (sphere/aabb растут ровно), КОНСЕРВАТИВНО для cylinder/obb (торцы/углы
// становятся скруглёнными, а мы отдаём охватывающий超-набор). Для разнородных пер-объектных
// габаритов используйте aabb_tree (хранит AABB каждого объекта). Раздувать надо на MAX
// габарит; лишнее добьётся точным тестом в visit.
template <typename Vec, int Dim>
sphere<Vec, Dim> inflate(const sphere<Vec, Dim>& s, const scalar_of<Vec> m) noexcept {
  return sphere<Vec, Dim>{s.center, s.radius + m};
}
template <typename Vec, int Dim>
aabb<Vec, Dim> inflate(const aabb<Vec, Dim>& b, const scalar_of<Vec> m) noexcept {
  aabb<Vec, Dim> r = b;
  for (int i = 0; i < Dim; ++i) {
    r.min[i] -= m;
    r.max[i] += m;
  }
  return r;
}
template <typename Vec, int Dim>
up_cylinder<Vec, Dim> inflate(const up_cylinder<Vec, Dim>& c, const scalar_of<Vec> m) noexcept {
  return up_cylinder<Vec, Dim>{c.center, c.radius + m};
}
// cylinder ⊕ ball = капсула; отдаём охватывающий цилиндр (радиус+m, торцы отодвинуты на m).
template <typename Vec, int Dim>
cylinder<Vec, Dim> inflate(const cylinder<Vec, Dim>& c, const scalar_of<Vec> m) noexcept {
  cylinder<Vec, Dim> r = c;
  for (int i = 0; i < Dim; ++i) {
    r.origin[i] = c.origin[i] - c.dir[i] * m; // сдвиг начала назад
  }
  r.length = c.length + m + m;
  r.radius = c.radius + m;
  return r;
}
// obb ⊕ ball ≈ obb с раздутыми полуосями (углы скруглены → консервативно).
template <typename Vec, int Dim>
obb<Vec, Dim> inflate(const obb<Vec, Dim>& o, const scalar_of<Vec> m) noexcept {
  obb<Vec, Dim> r = o;
  for (int i = 0; i < Dim; ++i) {
    r.half[i] += m;
  }
  return r;
}
// ray ⊕ ball = капсула радиуса m ⇒ ровно cylinder(origin, dir, tmax, m).
template <typename Vec, int Dim>
cylinder<Vec, Dim> inflate(const ray<Vec, Dim>& r, const scalar_of<Vec> m) noexcept {
  return cylinder<Vec, Dim>{r.origin, r.dir, r.tmax, m};
}

// ------------------------------------------------------------------ UpAxis-диспетч
// up_cylinder — единственная форма, чей тест зависит от UP-оси (её задаёт ШАБЛОН
// структуры). Чтобы query(shape,visit) звал contains/overlaps единообразно, структура
// зовёт test_contains<UpAxis>/test_overlaps<UpAxis>: для up_cylinder они подставляют
// UpAxis, для остальных форм — игнорируют его.
template <typename>
struct is_up_cylinder : std::false_type {};
template <typename V, int D>
struct is_up_cylinder<up_cylinder<V, D>> : std::true_type {};

template <int UpAxis, typename Shape, typename Vec>
bool test_contains(const Shape& s, const Vec& p) noexcept {
  if constexpr (is_up_cylinder<Shape>::value) {
    return contains<UpAxis>(s, p);
  } else {
    return contains(s, p);
  }
}
template <int UpAxis, typename Shape, typename Vec, int Dim>
bool test_overlaps(const Shape& s, const aabb<Vec, Dim>& b) noexcept {
  if constexpr (is_up_cylinder<Shape>::value) {
    return overlaps<UpAxis>(s, b);
  } else {
    return overlaps(s, b);
  }
}

// aabb-обёртки форм — для seed-построения / грубого прунинга снаружи структуры.
template <typename Vec, int Dim>
aabb<Vec, Dim> bounds(const sphere<Vec, Dim>& s) noexcept {
  Vec h{};
  for (int i = 0; i < Dim; ++i) {
    h[i] = s.radius;
  }
  return aabb<Vec, Dim>::from_center_half(s.center, h);
}
template <typename Vec, int Dim>
aabb<Vec, Dim> bounds(const cylinder<Vec, Dim>& c) noexcept {
  aabb<Vec, Dim> cb = aabb<Vec, Dim>::empty();
  cb.expand(c.origin);
  Vec tip{};
  for (int i = 0; i < Dim; ++i) {
    tip[i] = c.origin[i] + c.dir[i] * c.length;
  }
  cb.expand(tip);
  for (int i = 0; i < Dim; ++i) {
    cb.min[i] -= c.radius;
    cb.max[i] += c.radius;
  }
  return cb;
}
template <typename Vec, int Dim>
aabb<Vec, Dim> bounds(const obb<Vec, Dim>& o) noexcept {
  Vec h{};
  for (int i = 0; i < Dim; ++i) {
    h[i] = proj_radius<Dim>(o.half, o.axis, unit_axis<Dim, Vec>(i));
  }
  return aabb<Vec, Dim>::from_center_half(o.center, h);
}

// ------------------------------------------------------------------ query_bounds(shape)
// AABB, охватывающая область ИТЕРАЦИИ формы — нужна сеточным структурам (cell_list),
// которым надо перечислить ячейки. Для НЕОГРАНИЧЕННЫХ форм содержит ±inf по нужным
// осям (up_cylinder вдоль up; ray с tmax=inf вдоль направления) — структура клампит
// её к своей занятой области. UpAxis игнорируется всеми формами кроме up_cylinder.
template <int UpAxis, typename Vec, int Dim>
aabb<Vec, Dim> query_bounds(const aabb<Vec, Dim>& b) noexcept {
  return b;
}
template <int UpAxis, typename Vec, int Dim>
aabb<Vec, Dim> query_bounds(const sphere<Vec, Dim>& s) noexcept {
  return bounds(s);
}
template <int UpAxis, typename Vec, int Dim>
aabb<Vec, Dim> query_bounds(const cylinder<Vec, Dim>& c) noexcept {
  return bounds(c);
}
template <int UpAxis, typename Vec, int Dim>
aabb<Vec, Dim> query_bounds(const obb<Vec, Dim>& o) noexcept {
  return bounds(o);
}
template <int UpAxis, typename Vec, int Dim>
aabb<Vec, Dim> query_bounds(const up_cylinder<Vec, Dim>& c) noexcept {
  using S = scalar_of<Vec>;
  const S inf = std::numeric_limits<S>::infinity();
  aabb<Vec, Dim> b{};
  for (int i = 0; i < Dim; ++i) {
    if (i == UpAxis) {
      b.min[i] = -inf;
      b.max[i] = inf;
    } else {
      b.min[i] = c.center[i] - c.radius;
      b.max[i] = c.center[i] + c.radius;
    }
  }
  return b;
}
template <int UpAxis, typename Vec, int Dim>
aabb<Vec, Dim> query_bounds(const ray<Vec, Dim>& r) noexcept {
  using S = scalar_of<Vec>;
  const S inf = std::numeric_limits<S>::infinity();
  const bool unbounded = std::isinf(r.tmax);
  aabb<Vec, Dim> b{};
  for (int i = 0; i < Dim; ++i) {
    const S o = r.origin[i], d = r.dir[i];
    S lo = o, hi = o;
    if (d > S(0)) {
      hi = unbounded ? inf : o + d * r.tmax;
    } else if (d < S(0)) {
      lo = unbounded ? -inf : o + d * r.tmax;
    }
    b.min[i] = lo;
    b.max[i] = hi;
  }
  return b;
}

} // namespace geom
} // namespace utils
} // namespace devils_engine

#endif
