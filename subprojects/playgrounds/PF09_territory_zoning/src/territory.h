#ifndef DEVILS_ENGINE_PF09_TERRITORY_H
#define DEVILS_ENGINE_PF09_TERRITORY_H

// Иерархия территорий PF09: источник истины для всех остальных срезов.
//
// Модуль полностью аналитический и не хранит растров. `resolve(point)` зависит только от конфига: одна и
// та же точка всегда даёт одну и ту же цепочку узлов, в каком бы порядке её ни спрашивали. Это и делает
// возможной главную проверку площадки — запечённый тексель обязан совпасть с тем, что здесь посчитано
// для центра этого текселя.
//
// Разбиение строится вложенной сеткой, а не диаграммой Вороного. Причина не в простоте, а в инварианте:
// у ячейки яруса `t` родитель получается целочисленным делением её координат, поэтому строгое вложение и
// единственность родителя выполняются ПО ПОСТРОЕНИЮ, а не проверкой постфактум. Прямоугольность ячеек
// снимается двумя приёмами, которые вложение не трогают:
//
//   - доменный варп КООРДИНАТЫ ЗАПРОСА. Он общий для всех ярусов, поэтому смещает потомка вместе с
//     родителем и не может разорвать вложение. Октава на ярус: длина волны и амплитуда пропорциональны
//     размеру ячейки этого яруса, значит границы извилисты на каждом масштабе одинаково заметно.
//   - слияние соседних ячеек ВНУТРИ ОДНОГО РОДИТЕЛЯ. Даёт неравные площади и неровное число потомков.
//     Слияние строго одношаговое: поглощённая ячейка не может быть целью поглощения, поэтому цепочек не
//     возникает и представитель находится за один шаг.
//
// Верх дерева (`world`..`district`) материализован таблицей: у этих узлов есть титул и владелец. Низ
// (`locale`, `parcel`) не хранится вообще — узлов там 23 миллиона, и они выводятся арифметикой. Внешнему
// коду разница не видна: интерфейс у обеих половин один.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <glm/ext/vector_int2_sized.hpp>
#include <glm/vec2.hpp>

namespace devils_engine::pf09 {

// Ярусы записаны по ГЛУБИНЕ ОТ КОРНЯ, а не снизу вверх: тогда «ярус не глубже» — это `<=`, и сравнения в
// шейдере и в C++ читаются одинаково. `world` — технический корень, а не титул.
enum class tier : uint32_t {
  world,
  realm,
  duchy,
  barony,
  district,
  locale,
  parcel,
  count
};

constexpr size_t tier_count = size_t(tier::count);
constexpr tier leaf_tier = tier::parcel;

// Предел линейного дробления одного яруса: доли родителя считаются в стековый буфер этого размера.
constexpr uint32_t max_split = 64;

std::string_view tier_name(const tier value) noexcept;

// Идентификатор территории: ярус в старших битах, индекс ячейки в младших. Ярус внутри id, а не рядом с
// ним, потому что id ходит по одному в вершинных данных, в SSBO и в текселе, и разъехавшаяся пара
// (id, ярус) — это молчаливая порча, которую нечем поймать.
using zone_id = uint32_t;

constexpr uint32_t zone_tier_shift = 29;
constexpr uint32_t zone_index_mask = (1u << zone_tier_shift) - 1u;
constexpr zone_id invalid_zone = 0xffffffffu;

[[nodiscard]] constexpr tier tier_of(const zone_id id) noexcept {
  return tier(id >> zone_tier_shift);
}

[[nodiscard]] constexpr uint32_t index_of(const zone_id id) noexcept {
  return id & zone_index_mask;
}

[[nodiscard]] constexpr zone_id make_zone(const tier value, const uint32_t index) noexcept {
  return (uint32_t(value) << zone_tier_shift) | (index & zone_index_mask);
}

// Полный адрес точки: по узлу на каждый ярус, от корня к листу.
using address = std::array<zone_id, tier_count>;

struct layout_config {
  double world_span_m = 1024000.0;
  uint64_t seed = 0x5eed0909ull;

  // Линейное дробление родителя на этом ярусе: ячейка родителя делится на `split * split` частей.
  // У корня оно равно единице по определению.
  std::array<uint32_t, tier_count> split{1, 2, 3, 4, 5, 8, 5};

  // Доля размера ячейки, на которую варп смещает координату запроса. Складки начинаются между `0.10` и
  // `0.12` — измерено проверкой якобиана, а не выбрано на глаз. Значение по умолчанию держит запас:
  // сложенный варп разрывает территорию на два несвязных куска в разных концах карты.
  double warp_strength = 0.08;

  // Разброс ширин полос, на которые родитель делит себя. Ноль даёт равномерную сетку, то есть
  // миллиметровку; разброс нужен не для красоты, а чтобы статистика направлений и длин границ была
  // похожа на настоящую — на ней будут меряться SDF границ и читаемость ярусов.
  //
  // Значение держится маленьким, потому что разброс КОМПАУНДИТСЯ по ярусам: при `0.45` соседние локали
  // отличались линейно больше чем на порядок, и утверждение «ярус — это масштаб» переставало быть верным.
  // А на нём держится весь zone LOD: если территория одного яруса может быть и километровой, и
  // стометровой, то по высоте камеры уже нельзя решить, какой ярус сейчас разрешим.
  double split_jitter = 0.20;

  // Вероятность того, что ячейка поглощается соседом внутри своего родителя.
  double merge_chance = 0.30;

  // До какого яруса включительно узлы материализуются таблицей с владельцем и титулом.
  tier authored_depth = tier::district;

  // Сколько династий раздаётся владельцам и с какой вероятностью узел наследует владельца родителя.
  // Наследование — не украшение: без него карта владений превращается в шум и по ней нечем проверить,
  // что подсветка яруса и окраска по владельцу — разные вещи.
  uint32_t dynasty_count = 24;
  double owner_inherit_chance = 0.75;
};

// Узел материализованной части дерева.
struct authored_node {
  zone_id id = invalid_zone;
  zone_id parent = invalid_zone;
  uint32_t owner = 0;         // индекс династии
  uint32_t child_count = 0;   // сколько ячеек следующего яруса реально принадлежит узлу
  uint32_t cell_count = 1;    // сколько ячеек сетки слилось в этот узел
};

class territory {
public:
  explicit territory(const layout_config& config);

  const layout_config& config() const noexcept { return config_; }

  // НОМИНАЛЬНАЯ сторона ячейки яруса в метрах: разброс долей и варп делают настоящие размеры неравными,
  // поэтому это характерный масштаб яруса, а не размер конкретной территории.
  double tier_span_m(const tier value) const noexcept { return tier_span_[size_t(value)]; }
  uint32_t grid_dim(const tier value) const noexcept { return grid_dim_[size_t(value)]; }

  // Сколько ячеек сетки на ярусе всего, и сколько из них остались самостоятельными узлами после слияния.
  uint64_t cell_count(const tier value) const noexcept { return uint64_t(grid_dim_[size_t(value)]) * grid_dim_[size_t(value)]; }

  // Точное число узлов яруса. Для материализованных ярусов оно посчитано при построении; для procedural
  // считается полным обходом сетки по требованию и запоминается. Обход парцелл — это 23 миллиона хешей,
  // то есть доли секунды: цифра нужна отчёту, и получать её оценкой по выборке было бы враньём в
  // документе, который ровно этой цифрой и обосновывает всю конструкцию.
  uint64_t node_count(const tier value) const;

  // Разрешение точки. `resolve` возвращает лист, `resolve_chain` — весь адрес.
  address resolve_chain(const glm::dvec2& point_m) const;
  zone_id resolve(const glm::dvec2& point_m, const tier value = leaf_tier) const;

  zone_id parent_of(const zone_id id) const;
  zone_id ancestor_at(const zone_id id, const tier value) const;

  // Владелец узла. Для procedural-ярусов владельцем считается владелец ближайшего authored-предка:
  // участок не имеет собственного титула и не может иметь собственного хозяина.
  uint32_t owner_of(const zone_id id) const;

  const std::vector<authored_node>& authored() const noexcept { return authored_; }
  const authored_node* find_authored(const zone_id id) const;

  // Хеш конфига и всей материализованной таблицы. Регресс-константа среза 1.
  uint64_t fingerprint() const noexcept { return fingerprint_; }

  // Доменный варп координаты запроса — публичен, потому что проверка на складки должна щупать ровно ту
  // функцию, которой пользуется `resolve`, а не её копию.
  glm::dvec2 warp(const glm::dvec2& point_m) const;

  // Мировая точка внутри узла. Нужна всему, что ПРИВЯЗЫВАЕТСЯ к территории, а не спрашивает её: город
  // стоит в конкретном месте, и это место надо уметь назвать. Считается спуском от корня по тем же
  // полосам, что и разрешение, а затем обращением варпа итерацией — прямой формулы у варпа нет, но он
  // почти тождественен, и неподвижная точка находится за считанные шаги. Контракт простой и проверяемый:
  // `resolve(node_centre_m(n)) == n`.
  glm::dvec2 node_centre_m(const zone_id id) const;

private:
  template <typename scalar>
  scalar split_weight(const glm::i64vec2 parent_cell, const tier value, const uint32_t axis, const uint32_t index) const;
  template <typename scalar>
  uint32_t subdivide(const glm::i64vec2 parent_cell, const tier value, const uint32_t axis, scalar& local) const;
  template <typename scalar>
  void cells_impl(const glm::dvec2& point_m, std::span<glm::i64vec2> out) const;
  void cells_of(const glm::dvec2& point_m, std::span<glm::i64vec2> out) const;
  glm::i64vec2 representative(const glm::i64vec2 cell, const tier value) const;
  bool absorbed(const glm::i64vec2 cell, const tier value) const;
  uint32_t flat_index(const glm::i64vec2 cell, const tier value) const;
  glm::i64vec2 unflatten(const uint32_t index, const tier value) const;

  void build_authored();

  layout_config config_;
  std::array<uint32_t, tier_count> grid_dim_{};
  std::array<double, tier_count> tier_span_{};
  mutable std::array<uint64_t, tier_count> node_count_{};
  mutable std::array<bool, tier_count> node_count_ready_{};
  std::array<uint32_t, tier_count> authored_offset_{}; // начало яруса в `authored_slots_`
  uint64_t merge_threshold_ = 0;                       // `merge_chance`, переведённая в целочисленный порог
  uint64_t inherit_threshold_ = 0;

  // Слот на КАЖДУЮ ячейку сетки материализованных ярусов, включая поглощённые: индекс ячейки — это и есть
  // индекс слота, поэтому поиск узла остаётся арифметикой без хеш-таблицы. Поглощённая ячейка хранит
  // индекс своего представителя.
  std::vector<uint32_t> authored_slots_;
  std::vector<authored_node> authored_;
  uint64_t fingerprint_ = 0;
};

} // namespace devils_engine::pf09

#endif
