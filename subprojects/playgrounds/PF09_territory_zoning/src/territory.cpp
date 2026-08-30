#include "territory.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <span>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/hash.h"

namespace devils_engine::pf09 {

namespace {

constexpr uint64_t roll_mask = 0xffffull;
constexpr uint64_t roll_range = roll_mask + 1ull;

// Хеш ячейки: тождественен для одной и той же (координата, ярус, seed) и ни на что больше не смотрит.
// Именно поэтому слияние и владельцы не зависят ни от порядка обхода, ни от того, строим мы таблицу или
// отвечаем на одиночный запрос.
[[nodiscard]] uint64_t cell_hash(const glm::i64vec2 cell, const tier value, const uint64_t seed, const uint64_t salt) noexcept {
  uint64_t h = utils::hash_combine(seed ^ salt, uint64_t(cell.x) * 0x9e3779b97f4a7c15ull);
  h = utils::hash_combine(h, uint64_t(cell.y) * 0xc2b2ae3d27d4eb4full);
  h = utils::hash_combine(h, uint64_t(value) + 1ull);
  return utils::splitmix(h);
}

[[nodiscard]] int64_t wrap_index(const int64_t value, const int64_t dim) noexcept {
  const int64_t m = value % dim;
  return m < 0 ? m + dim : m;
}

// Решётка шума заворачивается по `period` узлов. Период октавы равен числу ячеек её яруса на весь мир,
// поэтому варп получается ТОЧНО периодичным с периодом в сторону мира. Это не украшение: индексы ячеек и
// так заворачиваются, и без периодичного шума у квадрата мира появляется шов, за которым часть территорий
// не имеет прообраза — то есть существует в таблице, но её нет на карте.
[[nodiscard]] double lattice_value(const int64_t ix, const int64_t iy, const int64_t period, const uint64_t seed) noexcept {
  const int64_t wx = wrap_index(ix, period);
  const int64_t wy = wrap_index(iy, period);
  const uint64_t h = utils::splitmix(uint64_t(wx) * 0x9e3779b97f4a7c15ull ^ (uint64_t(wy) * 0xc2b2ae3d27d4eb4full), seed);
  return double(h >> 11) * (2.0 / 9007199254740992.0) - 1.0; // [-1, 1)
}

template <typename scalar>
[[nodiscard]] scalar quintic(const scalar t) noexcept {
  return t * t * t * (t * (t * scalar(6) - scalar(15)) + scalar(10));
}

// Обычный value noise на квинтической интерполяции. Квинтика здесь не для гладкости картинки, а ради
// непрерывной первой производной: по якобиану варпа проверяется отсутствие складок, и на кубическом
// сглаживании эта проверка ловила бы изломы сетки, а не настоящие складки.
template <typename scalar>
[[nodiscard]] scalar value_noise(const scalar x, const scalar y, const int64_t period, const uint64_t seed) noexcept {
  const scalar fx = std::floor(x);
  const scalar fy = std::floor(y);
  const int64_t ix = int64_t(fx);
  const int64_t iy = int64_t(fy);
  const scalar tx = quintic<scalar>(x - fx);
  const scalar ty = quintic<scalar>(y - fy);

  const scalar v00 = scalar(lattice_value(ix, iy, period, seed));
  const scalar v10 = scalar(lattice_value(ix + 1, iy, period, seed));
  const scalar v01 = scalar(lattice_value(ix, iy + 1, period, seed));
  const scalar v11 = scalar(lattice_value(ix + 1, iy + 1, period, seed));

  const scalar bottom = v00 + (v10 - v00) * tx;
  const scalar top = v01 + (v11 - v01) * tx;
  return bottom + (top - bottom) * ty;
}

} // namespace

std::string_view tier_name(const tier value) noexcept {
  switch (value) {
    case tier::world: return "world";
    case tier::realm: return "realm";
    case tier::duchy: return "duchy";
    case tier::barony: return "barony";
    case tier::district: return "district";
    case tier::locale: return "locale";
    case tier::parcel: return "parcel";
    default: return "?";
  }
}

territory::territory(const layout_config& config) : config_(config) {
  if (config_.world_span_m <= 0.0) {
    utils::error{}("PF09 layout: world span must be positive, got {}", config_.world_span_m);
  }
  if (config_.split[0] != 1) {
    utils::error{}("PF09 layout: root tier must not subdivide anything, got split {}", config_.split[0]);
  }
  if (size_t(config_.authored_depth) >= tier_count) {
    utils::error{}("PF09 layout: authored depth {} is outside the tier list", uint32_t(config_.authored_depth));
  }

  uint64_t dim = 1;
  for (size_t t = 0; t < tier_count; ++t) {
    if (config_.split[t] == 0) {
      utils::error{}("PF09 layout: tier '{}' subdivides by zero", tier_name(tier(t)));
    }
    if (config_.split[t] > max_split) {
      utils::error{}("PF09 layout: tier '{}' subdivides by {}, above the {} the stack buffer holds",
                     tier_name(tier(t)), config_.split[t], max_split);
    }
    dim *= config_.split[t];

    // Индекс ячейки живёт в 29 битах id вместе с ярусом. Ловим здесь, потому что дальше переполнение
    // проявится не ошибкой, а двумя разными территориями с одинаковым идентификатором.
    if (dim * dim > uint64_t(zone_index_mask)) {
      utils::error{}("PF09 layout: tier '{}' needs {} cells, but a zone_id holds only {}", tier_name(tier(t)), dim * dim,
                     uint64_t(zone_index_mask));
    }

    grid_dim_[t] = uint32_t(dim);
    tier_span_[t] = config_.world_span_m / double(dim);
  }

  merge_threshold_ = uint64_t(std::clamp(config_.merge_chance, 0.0, 1.0) * double(roll_range));
  inherit_threshold_ = uint64_t(std::clamp(config_.owner_inherit_chance, 0.0, 1.0) * double(roll_range));

  build_authored();
}

glm::dvec2 territory::warp(const glm::dvec2& point_m) const {
  // Нулевая амплитуда — это не «октавы с нулевым вкладом», а отсутствие варпа. Ранний выход нужен не ради
  // микрооптимизации, а чтобы `--warp=0` измерял ЦЕНУ варпа, а не считал шум и умножал его на ноль.
  if (config_.warp_strength == 0.0) return point_m;

  auto result = point_m;

  // Октавы берут смещение от ИСХОДНОЙ точки, а не от накопленной: последовательный доменный варп
  // складывается гораздо охотнее, а выигрыша в виде картинки здесь не даёт.
  for (size_t t = size_t(tier::realm); t < tier_count; ++t) {
    const double lambda = tier_span_[t];
    const double amplitude = config_.warp_strength * lambda;
    const double u = point_m.x / lambda;
    const double v = point_m.y / lambda;
    const int64_t period = int64_t(grid_dim_[t]);
    result.x += amplitude * value_noise(u, v, period, utils::splitmix(config_.seed, t * 2ull + 1ull));
    result.y += amplitude * value_noise(u, v, period, utils::splitmix(config_.seed, t * 2ull + 2ull));
  }

  return result;
}

// Доля полосы `index` в разбиении родителя на `count` частей вдоль оси. Доли зависят только от родителя,
// яруса и оси, поэтому одна и та же территория режется одинаково, кем бы её ни спросили.
template <typename scalar>
scalar territory::split_weight(const glm::i64vec2 parent_cell, const tier value, const uint32_t axis,
                               const uint32_t index) const {
  const uint64_t h = cell_hash(parent_cell, value, config_.seed, 0x5911700ull + axis * 977ull + index * 31ull);
  const scalar unit = scalar(h & 0xffffull) / scalar(roll_range);
  return scalar(1) + scalar(config_.split_jitter) * (unit * scalar(2) - scalar(1));
}

// Выбор полосы, в которую попала локальная координата, и пересчёт координаты внутрь этой полосы.
// Ребёнок ищется ВНУТРИ родителя, поэтому уйти из него он не может ни при каком округлении: строгое
// вложение здесь структурное, а не следствие удачной арифметики.
template <typename scalar>
uint32_t territory::subdivide(const glm::i64vec2 parent_cell, const tier value, const uint32_t axis,
                              scalar& local) const {
  const uint32_t count = config_.split[size_t(value)];
  if (count == 1) return 0;

  // Доли считаются ОДИН раз в стековый буфер. Раньше их считали дважды — на сумму и на обход — и это
  // удваивало число хешей на каждый разрешённый тексель. Клипмап печёт миллионы текселей за перестройку
  // уровня, поэтому цена одного `resolve` перестала быть мелочью.
  std::array<scalar, max_split> weights{};
  scalar total = scalar(0);
  for (uint32_t i = 0; i < count; ++i) {
    weights[i] = split_weight<scalar>(parent_cell, value, axis, i);
    total += weights[i];
  }

  scalar cursor = scalar(0);
  for (uint32_t i = 0; i < count; ++i) {
    const scalar width = weights[i] / total;
    if (local < cursor + width || i + 1 == count) {
      local = std::clamp((local - cursor) / width, scalar(0), std::nextafter(scalar(1), scalar(0)));
      return i;
    }
    cursor += width;
  }
  return count - 1;
}

// Ячейки ярусов выводятся спуском ОТ КОРНЯ: на каждом шаге координата уже локальна для родителя, поэтому
// потомок выбирается среди детей именно этого родителя. Это единственная форма, при которой вложение не
// зависит от точности: пересчёт каждого яруса из своей мировой координаты расходится ровно на границе
// ячейки, где `x / span_locale == 480.0` и одновременно `x / span_parcel == 2399.9999999999995`.
bool territory::absorbed(const glm::i64vec2 cell, const tier value) const {
  if (value == tier::world) return false;
  return (cell_hash(cell, value, config_.seed, 0xa11ce5ull) & roll_mask) < merge_threshold_;
}

glm::i64vec2 territory::representative(const glm::i64vec2 cell, const tier value) const {
  if (value == tier::world) return cell;

  const uint64_t h = cell_hash(cell, value, config_.seed, 0xa11ce5ull);
  if ((h & roll_mask) >= merge_threshold_) return cell;

  constexpr int64_t offset_x[4] = {1, -1, 0, 0};
  constexpr int64_t offset_y[4] = {0, 0, 1, -1};
  const uint32_t direction = uint32_t((h >> 16) & 3ull);
  const glm::i64vec2 target{cell.x + offset_x[direction], cell.y + offset_y[direction]};

  const int64_t dim = int64_t(grid_dim_[size_t(value)]);
  if (target.x < 0 || target.y < 0 || target.x >= dim || target.y >= dim) return cell;

  // Поглощение разрешено только внутри одного родителя: иначе у слитого узла оказалось бы два родителя,
  // и всё дерево перестало бы быть деревом.
  const int64_t step = int64_t(config_.split[size_t(value)]);
  if (target.x / step != cell.x / step || target.y / step != cell.y / step) return cell;

  // Цепочек не строим: поглощённая ячейка не может быть целью поглощения. Иначе представителя пришлось бы
  // искать обходом произвольной длины, а он должен находиться за один шаг из любой точки.
  if (absorbed(target, value)) return cell;

  return target;
}

uint32_t territory::flat_index(const glm::i64vec2 cell, const tier value) const {
  const int64_t dim = int64_t(grid_dim_[size_t(value)]);
  return uint32_t(cell.y * dim + cell.x);
}

glm::i64vec2 territory::unflatten(const uint32_t index, const tier value) const {
  const int64_t dim = int64_t(grid_dim_[size_t(value)]);
  return {int64_t(index) % dim, int64_t(index) / dim};
}

template <typename scalar>
void territory::cells_impl(const glm::dvec2& point_m, std::span<glm::i64vec2> out) const {
  // Варп считается в той же точности, что и спуск: смысл замера в том, чтобы получить ровно ту цепочку
  // округлений, которая будет в GLSL, а не смесь из двух точностей.
  scalar wx = scalar(point_m.x);
  scalar wy = scalar(point_m.y);

  if (config_.warp_strength != 0.0) {
    for (size_t t = size_t(tier::realm); t < tier_count; ++t) {
      const scalar lambda = scalar(tier_span_[t]);
      const scalar amplitude = scalar(config_.warp_strength) * lambda;
      const scalar u = scalar(point_m.x) / lambda;
      const scalar v = scalar(point_m.y) / lambda;
      const int64_t period = int64_t(grid_dim_[t]);
      wx += amplitude * value_noise<scalar>(u, v, period, utils::splitmix(config_.seed, t * 2ull + 1ull));
      wy += amplitude * value_noise<scalar>(u, v, period, utils::splitmix(config_.seed, t * 2ull + 2ull));
    }
  }

  const scalar inverse_span = scalar(1) / scalar(config_.world_span_m);
  scalar lx = wx * inverse_span;
  scalar ly = wy * inverse_span;
  lx -= std::floor(lx);
  ly -= std::floor(ly);

  out[0] = {0, 0};
  for (size_t t = 1; t < tier_count; ++t) {
    const auto value = tier(t);
    const int64_t step = int64_t(config_.split[t]);
    const uint32_t ix = subdivide<scalar>(out[t - 1], value, 0, lx);
    const uint32_t iy = subdivide<scalar>(out[t - 1], value, 1, ly);
    out[t] = {out[t - 1].x * step + int64_t(ix), out[t - 1].y * step + int64_t(iy)};
  }
}

void territory::cells_of(const glm::dvec2& point_m, std::span<glm::i64vec2> out) const {
  cells_impl<double>(point_m, out);
}

glm::dvec2 territory::node_centre_m(const zone_id id) const {
  const auto value = tier_of(id);

  std::array<glm::i64vec2, tier_count> cells{};
  cells[size_t(value)] = unflatten(index_of(id), value);
  for (size_t t = size_t(value); t > 0; --t) {
    const int64_t step = int64_t(config_.split[t]);
    cells[t - 1] = {cells[t].x / step, cells[t].y / step};
  }

  double lo_x = 0.0;
  double hi_x = 1.0;
  double lo_y = 0.0;
  double hi_y = 1.0;

  for (size_t t = 1; t <= size_t(value); ++t) {
    const auto child = tier(t);
    const uint32_t count = config_.split[t];
    const int64_t step = int64_t(count);

    const uint32_t index_x = uint32_t(cells[t].x - cells[t - 1].x * step);
    const uint32_t index_y = uint32_t(cells[t].y - cells[t - 1].y * step);

    for (uint32_t axis = 0; axis < 2; ++axis) {
      double total = 0.0;
      std::array<double, max_split> weights{};
      for (uint32_t i = 0; i < count; ++i) {
        weights[i] = split_weight<double>(cells[t - 1], child, axis, i);
        total += weights[i];
      }

      const uint32_t wanted = axis == 0 ? index_x : index_y;
      double cursor = 0.0;
      for (uint32_t i = 0; i < wanted; ++i) {
        cursor += weights[i] / total;
      }
      const double width = weights[wanted] / total;

      if (axis == 0) {
        const double span = hi_x - lo_x;
        hi_x = lo_x + span * (cursor + width);
        lo_x = lo_x + span * cursor;
      } else {
        const double span = hi_y - lo_y;
        hi_y = lo_y + span * (cursor + width);
        lo_y = lo_y + span * cursor;
      }
    }
  }

  const glm::dvec2 target{(lo_x + hi_x) * 0.5 * config_.world_span_m, (lo_y + hi_y) * 0.5 * config_.world_span_m};

  // Обращение варпа неподвижной точкой: `p <- target - (warp(p) - p)`. Сходится, потому что смещение
  // варпа мало по сравнению с собственным масштабом — то же условие, что запрещает складки.
  auto guess = target;
  for (uint32_t step = 0; step < 24; ++step) {
    const auto residual = warp(guess) - target;
    if (std::abs(residual.x) + std::abs(residual.y) < 1.0e-6) break;
    guess -= residual;
  }
  return guess;
}

address territory::resolve_chain(const glm::dvec2& point_m) const {
  std::array<glm::i64vec2, tier_count> cells{};
  cells_of(point_m, cells);

  address out{};
  for (size_t t = 0; t < tier_count; ++t) {
    const auto value = tier(t);
    out[t] = make_zone(value, flat_index(representative(cells[t], value), value));
  }
  return out;
}

zone_id territory::resolve(const glm::dvec2& point_m, const tier value) const {
  std::array<glm::i64vec2, tier_count> cells{};
  cells_of(point_m, cells);

  const size_t index = size_t(value);
  return make_zone(value, flat_index(representative(cells[index], value), value));
}

zone_id territory::parent_of(const zone_id id) const {
  const auto value = tier_of(id);
  if (value == tier::world) return invalid_zone;

  const auto cell = unflatten(index_of(id), value);
  const int64_t step = int64_t(config_.split[size_t(value)]);
  const auto parent_tier = tier(uint32_t(value) - 1);
  const glm::i64vec2 up{cell.x / step, cell.y / step};
  return make_zone(parent_tier, flat_index(representative(up, parent_tier), parent_tier));
}

zone_id territory::ancestor_at(const zone_id id, const tier value) const {
  auto current = id;
  if (uint32_t(value) > uint32_t(tier_of(current))) return invalid_zone;

  while (tier_of(current) != value) {
    current = parent_of(current);
    if (current == invalid_zone) return invalid_zone;
  }
  return current;
}

const authored_node* territory::find_authored(const zone_id id) const {
  const auto value = tier_of(id);
  if (uint32_t(value) > uint32_t(config_.authored_depth)) return nullptr;

  const uint32_t slot = authored_offset_[size_t(value)] + index_of(id);
  return &authored_[authored_slots_[slot]];
}

uint32_t territory::owner_of(const zone_id id) const {
  const auto value = tier_of(id);
  const auto anchor = uint32_t(value) > uint32_t(config_.authored_depth) ? ancestor_at(id, config_.authored_depth) : id;
  const auto* node = find_authored(anchor);
  return node == nullptr ? 0 : node->owner;
}

uint64_t territory::node_count(const tier value) const {
  const size_t index = size_t(value);
  if (node_count_ready_[index]) return node_count_[index];

  const int64_t dim = int64_t(grid_dim_[index]);
  uint64_t count = 0;
  for (int64_t y = 0; y < dim; ++y) {
    for (int64_t x = 0; x < dim; ++x) {
      const glm::i64vec2 cell{x, y};
      if (representative(cell, value) == cell) ++count;
    }
  }

  node_count_[index] = count;
  node_count_ready_[index] = true;
  return count;
}

void territory::build_authored() {
  const size_t depth = size_t(config_.authored_depth);

  uint32_t running = 0;
  for (size_t t = 0; t <= depth; ++t) {
    authored_offset_[t] = running;
    running += uint32_t(cell_count(tier(t)));
  }
  authored_slots_.assign(running, 0);

  // Первый проход создаёт узлы у представителей, второй направляет поглощённые ячейки на их узел. Два
  // прохода нужны потому, что представитель может встретиться позже поглощённой ячейки: порядок обхода
  // не должен влиять на результат.
  for (size_t t = 0; t <= depth; ++t) {
    const auto value = tier(t);
    const int64_t dim = int64_t(grid_dim_[t]);

    for (int64_t y = 0; y < dim; ++y) {
      for (int64_t x = 0; x < dim; ++x) {
        const glm::i64vec2 cell{x, y};
        if (representative(cell, value) != cell) continue;

        const auto id = make_zone(value, flat_index(cell, value));
        authored_slots_[authored_offset_[t] + index_of(id)] = uint32_t(authored_.size());

        auto& node = authored_.emplace_back();
        node.id = id;
        node.parent = parent_of(id);
        node.cell_count = 1;
      }
    }

    for (int64_t y = 0; y < dim; ++y) {
      for (int64_t x = 0; x < dim; ++x) {
        const glm::i64vec2 cell{x, y};
        const auto rep = representative(cell, value);
        if (rep == cell) continue;

        const uint32_t rep_slot = authored_offset_[t] + flat_index(rep, value);
        const uint32_t node_index = authored_slots_[rep_slot];
        authored_slots_[authored_offset_[t] + flat_index(cell, value)] = node_index;
        ++authored_[node_index].cell_count;
      }
    }

  }

  // Владельцы раздаются сверху вниз. Узлы лежат в порядке ярусов, поэтому владелец родителя уже известен
  // к моменту, когда до потомка доходит очередь, и отдельный проход не нужен.
  const uint32_t dynasties = std::max(config_.dynasty_count, 1u);
  for (auto& node : authored_) {
    const auto value = tier_of(node.id);
    const uint64_t roll = cell_hash(unflatten(index_of(node.id), value), value, config_.seed, 0xd0776e72ull);
    const auto* parent = node.parent == invalid_zone ? nullptr : find_authored(node.parent);

    const bool inherits = parent != nullptr && (roll & roll_mask) < inherit_threshold_;
    node.owner = inherits ? parent->owner : uint32_t(utils::splitmix(roll) % dynasties);
  }

  for (const auto& node : authored_) {
    if (node.parent == invalid_zone) continue;
    ++authored_[authored_slots_[authored_offset_[size_t(tier_of(node.parent))] + index_of(node.parent)]].child_count;
  }

  // В отпечаток входит ВЕСЬ конфиг, а не только то, что попало в таблицу. Геометрия (варп, разброс долей)
  // таблицу узлов не меняет, но меняет карту — и регресс-константа, которая этого не замечает, бесполезна.
  uint64_t hash = utils::splitmix(config_.seed);
  hash = utils::hash_combine(hash, std::bit_cast<uint64_t>(config_.world_span_m));
  hash = utils::hash_combine(hash, std::bit_cast<uint64_t>(config_.warp_strength));
  hash = utils::hash_combine(hash, std::bit_cast<uint64_t>(config_.split_jitter));
  hash = utils::hash_combine(hash, std::bit_cast<uint64_t>(config_.merge_chance));
  hash = utils::hash_combine(hash, std::bit_cast<uint64_t>(config_.owner_inherit_chance));
  hash = utils::hash_combine(hash, config_.dynasty_count);
  hash = utils::hash_combine(hash, uint32_t(config_.authored_depth));
  for (size_t t = 0; t < tier_count; ++t) {
    hash = utils::hash_combine(hash, config_.split[t]);
  }
  for (const auto& node : authored_) {
    hash = utils::hash_combine(hash, node.id);
    hash = utils::hash_combine(hash, node.parent);
    hash = utils::hash_combine(hash, node.owner);
    hash = utils::hash_combine(hash, node.cell_count);
  }
  fingerprint_ = utils::splitmix(hash);

  for (size_t t = 0; t <= depth; ++t) {
    uint64_t count = 0;
    for (const auto& node : authored_) {
      if (size_t(tier_of(node.id)) == t) ++count;
    }
    node_count_[t] = count;
    node_count_ready_[t] = true;
  }
}

} // namespace devils_engine::pf09
