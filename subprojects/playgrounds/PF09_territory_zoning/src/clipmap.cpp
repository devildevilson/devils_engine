#include "clipmap.h"

#include <algorithm>
#include <cmath>

#include "devils_engine/utils/core.h"

namespace devils_engine::pf09 {

namespace {

[[nodiscard]] int64_t wrap_texel(const int64_t value, const int64_t side) noexcept {
  const int64_t m = value % side;
  return m < 0 ? m + side : m;
}

} // namespace

clipmap::clipmap(const territory& map, const clipmap_config& config) : map_(&map), config_(config) {
  if (config_.side < 8 || (config_.side & (config_.side - 1)) != 0) {
    utils::error{}("PF09 clipmap: side {} must be a power of two of at least 8", config_.side);
  }
  if (config_.min_tier_texels == 0) {
    utils::error{}("PF09 clipmap: a tier resolvable in zero texels is not resolvable at all");
  }
  if (config_.resident_levels == 0) {
    utils::error{}("PF09 clipmap: the pool must hold at least one level");
  }

  // По умолчанию тексель самого мелкого уровня выводится из иерархии: он такой, чтобы листовая
  // территория занимала объявленное число текселей. Явное значение перекрывает это, потому что резкость
  // вблизи задаёт экран, а не дерево.
  base_texel_m_ = config_.base_texel_m > 0.0 ? config_.base_texel_m
                                             : map.tier_span_m(leaf_tier) / double(config_.min_tier_texels);

  const double world = map.config().world_span_m;
  level_count_ = 1;
  while (double(config_.side) * base_texel_m_ * double(1ull << (level_count_ - 1)) < world) {
    ++level_count_;
  }

  texel_size_.resize(level_count_);
  level_tier_.resize(level_count_);
  levels_.resize(level_count_);

  for (uint32_t k = 0; k < level_count_; ++k) {
    texel_size_[k] = base_texel_m_ * double(1ull << k);

    // Самый глубокий разрешимый ярус. Записывать на этот уровень что-то мельче — записывать шум: ячейка
    // просто не попадёт в достаточное число текселей, чтобы её было видно.
    const double resolvable = double(config_.min_tier_texels) * texel_size_[k];
    auto chosen = tier::world;
    for (size_t t = 0; t < tier_count; ++t) {
      if (map.tier_span_m(tier(t)) >= resolvable) chosen = tier(t);
    }
    level_tier_[k] = chosen;

    levels_[k].texels.assign(size_t(config_.side) * config_.side, invalid_zone);
  }
}

double clipmap::texel_size_m(const uint32_t level) const noexcept { return texel_size_[level]; }

double clipmap::coverage_m(const uint32_t level) const noexcept { return texel_size_[level] * double(config_.side); }

uint64_t clipmap::resident_bytes() const noexcept {
  return uint64_t(resident_count_) * config_.side * config_.side * sizeof(zone_id);
}

uint32_t clipmap::required_first(const double meters_per_pixel) const noexcept {
  if (meters_per_pixel <= base_texel_m_) return 0;
  const double octave = std::log2(meters_per_pixel / base_texel_m_);
  return uint32_t(std::clamp(std::floor(octave), 0.0, double(level_count_ - 1)));
}

uint32_t clipmap::required_last(const double view_distance_m) const noexcept {
  for (uint32_t k = 0; k < level_count_; ++k) {
    if (coverage_m(k) * 0.5 >= view_distance_m) return k;
  }
  return level_count_ - 1;
}

glm::i64vec2 clipmap::origin_for(const glm::dvec2& center_m, const uint32_t level) const {
  const double size = texel_size_[level];
  const int64_t half = int64_t(config_.side) / 2;
  return {int64_t(std::floor(center_m.x / size)) - half, int64_t(std::floor(center_m.y / size)) - half};
}

glm::dvec2 clipmap::texel_center_m(const glm::i64vec2 texel, const uint32_t level) const {
  const double size = texel_size_[level];
  return {(double(texel.x) + 0.5) * size, (double(texel.y) + 0.5) * size};
}

void clipmap::emit_region(const uint32_t level, const glm::i64vec2 begin, const glm::i64vec2 end, update_cost& cost) {
  const int64_t side = int64_t(config_.side);
  if (begin.x >= end.x || begin.y >= end.y) return;

  // Мировая полоса по модулю стороны может пересечь шов текстуры. Регион, пересекающий шов, нельзя залить
  // одной командой копирования, поэтому он режется здесь, а не оставляется на потребителя.
  const int64_t x0 = wrap_texel(begin.x, side);
  const int64_t y0 = wrap_texel(begin.y, side);
  const int64_t width = end.x - begin.x;
  const int64_t height = end.y - begin.y;

  const int64_t x_parts[2] = {std::min(width, side - x0), width - std::min(width, side - x0)};
  const int64_t y_parts[2] = {std::min(height, side - y0), height - std::min(height, side - y0)};
  const int64_t x_start[2] = {x0, 0};
  const int64_t y_start[2] = {y0, 0};

  for (uint32_t iy = 0; iy < 2; ++iy) {
    if (y_parts[iy] <= 0) continue;
    for (uint32_t ix = 0; ix < 2; ++ix) {
      if (x_parts[ix] <= 0) continue;
      regions_.push_back({level, uint32_t(x_start[ix]), uint32_t(y_start[iy]), uint32_t(x_parts[ix]),
                          uint32_t(y_parts[iy])});
      ++cost.regions;
    }
  }
}

void clipmap::fill_rect(const uint32_t level, const glm::i64vec2 begin, const glm::i64vec2 end, update_cost& cost) {
  if (begin.x >= end.x || begin.y >= end.y) return;

  const int64_t side = int64_t(config_.side);
  const auto value = level_tier_[level];
  auto& texels = levels_[level].texels;

  for (int64_t y = begin.y; y < end.y; ++y) {
    const int64_t row = wrap_texel(y, side) * side;
    for (int64_t x = begin.x; x < end.x; ++x) {
      texels[size_t(row + wrap_texel(x, side))] = map_->resolve(texel_center_m({x, y}, level), value);
    }
  }

  cost.texels += uint64_t(end.x - begin.x) * uint64_t(end.y - begin.y);
  emit_region(level, begin, end, cost);
}

// Печка идёт СТРОКАМИ окна и продолжается с того места, где кончился бюджет. Строка выбрана единицей
// потому, что регион заливки из целых строк — это один прямоугольник во всю ширину, а не лоскут: дробить
// печку колонками значило бы плодить команды копирования там, где нужна одна.
void clipmap::advance_bake(const uint32_t level, uint64_t& budget, update_cost& cost) {
  auto& state = levels_[level];
  const int64_t side = int64_t(config_.side);

  const int64_t affordable = budget == 0 ? side : int64_t(std::min<uint64_t>(budget / uint64_t(side), uint64_t(side)));
  const int64_t rows = std::min(side - state.baked_rows, std::max<int64_t>(affordable, 1));

  const int64_t begin_y = state.origin.y + state.baked_rows;
  fill_rect(level, {state.origin.x, begin_y}, {state.origin.x + side, begin_y + rows}, cost);
  state.baked_rows += rows;

  const uint64_t spent = uint64_t(rows) * uint64_t(side);
  budget = budget <= spent ? 0 : budget - spent;

  if (state.baked_rows < side) {
    ++cost.pending_levels;
    return;
  }

  state.baking = false;
  state.valid = true;
  ++cost.rebuilt_levels;
}

update_cost clipmap::focus(const glm::dvec2& center_m, const double meters_per_pixel, const double view_distance_m) {
  regions_.clear();
  update_cost cost{};

  const uint32_t wanted = required_first(meters_per_pixel);

  // Гистерезис живёт на непрерывной октаве, а не на уже округлённом уровне: округление само по себе
  // переключается ровно на границе, и камера, стоящая на ней, переключала бы окно каждый кадр.
  uint32_t needed_first = wanted;
  if (focused_) {
    const double octave = meters_per_pixel <= base_texel_m_ ? 0.0 : std::log2(meters_per_pixel / base_texel_m_);
    const double current = double(needed_first_);
    const bool coarser = octave > current + 1.0 + config_.hysteresis_octaves;
    const bool finer = octave < current - config_.hysteresis_octaves;
    needed_first = coarser || finer ? wanted : needed_first_;
  }

  const uint32_t needed_last = std::max(needed_first, required_last(view_distance_m));

  // Юбка расширяет окно в обе стороны: уровень должен успеть испечься ДО того, как зум до него доедет.
  const uint32_t skirt = config_.prefetch_levels;
  uint32_t first = needed_first > skirt ? needed_first - skirt : 0;
  uint32_t last = std::min(level_count_ - 1, needed_last + skirt);

  // Порядок, в котором пул отдаёт слоты при нехватке, задан явно, потому что цена у концов РАЗНАЯ и
  // выяснилась она измерением, а не рассуждением:
  //   1. мелкая юбка — её потеря даёт лишь всплеск детализации при приближении;
  //   2. мелкие нужные уровни — картинка вблизи становится менее резкой, и это переживаемо;
  //   3. грубая юбка — отдаётся ПОСЛЕДНЕЙ.
  // Третий пункт неочевиден и стоил отдельного прогона. Дыры появляются только при ОТДАЛЕНИИ, когда
  // растёт дальность видимости, и закрывает их заранее испечённый грубый уровень. Отдав грубую юбку ради
  // детализации, мы меняем «чуть мутнее вблизи» на «на периферии пусто, пока печётся» — то есть меняем
  // косметику на дыру.
  while (last - first + 1 > config_.resident_levels && first < needed_first) ++first;
  while (last - first + 1 > config_.resident_levels && first < needed_last) {
    ++first;
    ++cost.starved_levels;
  }
  while (last - first + 1 > config_.resident_levels && last > needed_last) --last;

  const uint32_t count = last - first + 1;

  uint64_t budget = config_.bake_budget_texels;

  for (uint32_t k = 0; k < level_count_; ++k) {
    const bool now_resident = k >= first && k < first + count;
    if (!now_resident) {
      levels_[k].valid = false;
      levels_[k].baking = false;
      levels_[k].baked_rows = 0;
      continue;
    }

    const auto origin = origin_for(center_m, k);
    const int64_t side = int64_t(config_.side);

    // Пока уровень печётся, его окно ЗАМОРОЖЕНО. Иначе съехавшая на тексель камера сбрасывала бы печку в
    // начало, и уровень с мелким текселем не заканчивался бы никогда: он движется каждый кадр. Дрейф за
    // время печки догоняется обычными полосами сразу после её завершения, и это дёшево — дрейф ограничен
    // скоростью камеры, а не размером окна.
    if (levels_[k].baking) continue;

    const auto previous = levels_[k].origin;
    const glm::i64vec2 delta{origin.x - previous.x, origin.y - previous.y};

    // Прыжок дальше стороны окна не оставляет ни одного пригодного текселя, поэтому полосами обновлять
    // нечего: это та же полная печка, только записанная сложнее.
    const bool rebuild = !levels_[k].valid || std::abs(delta.x) >= side || std::abs(delta.y) >= side;
    levels_[k].origin = origin;

    if (rebuild) {
      levels_[k].valid = false;
      levels_[k].baking = true;
      levels_[k].baked_rows = 0;
      continue;
    }
    if (delta.x == 0 && delta.y == 0) continue;

    ++cost.shifted_levels;

    // Вертикальная полоса берёт всю новую высоту, горизонтальная — только ту часть новой ширины, которой
    // вертикальная не касалась. Так угол попадает ровно в один из двух проходов, и ни один тексель не
    // пишется дважды: двойная запись стоила бы вдвое дороже и прятала бы ошибку адресации.
    const int64_t column_begin = delta.x > 0 ? previous.x + side : origin.x;
    const int64_t column_end = delta.x > 0 ? origin.x + side : previous.x;
    fill_rect(k, {column_begin, origin.y}, {column_end, origin.y + side}, cost);

    const int64_t kept_begin = delta.x > 0 ? origin.x : column_end;
    const int64_t kept_end = delta.x > 0 ? column_begin : origin.x + side;
    const int64_t row_begin = delta.y > 0 ? previous.y + side : origin.y;
    const int64_t row_end = delta.y > 0 ? origin.y + side : previous.y;
    fill_rect(k, {kept_begin, row_begin}, {kept_end, row_end}, cost);
  }

  // Печка идёт ОТ САМОГО ГРУБОГО из ожидающих. Порядок не косметический: при отдалении растёт дальность
  // видимости, и незакрытым остаётся именно край — его закрывает грубый уровень. При приближении дыры не
  // бывает вовсе, есть лишь временно меньшая детализация. Обратный порядок (от мелкого) отдаёт бюджет
  // тому уровню, отсутствие которого никого не ранит, и морит голодом тот, без которого показывать нечего.
  for (uint32_t k = first + count; k-- > first;) {
    if (!levels_[k].baking) continue;
    advance_bake(k, budget, cost);
  }

  first_resident_ = first;
  resident_count_ = count;
  needed_first_ = needed_first;
  focused_ = true;
  return cost;
}

bool clipmap::contains(const glm::dvec2& point_m, const uint32_t level) const {
  const double size = texel_size_[level];
  const int64_t side = int64_t(config_.side);
  const glm::i64vec2 texel{int64_t(std::floor(point_m.x / size)), int64_t(std::floor(point_m.y / size))};
  const auto origin = levels_[level].origin;
  return texel.x >= origin.x && texel.y >= origin.y && texel.x < origin.x + side && texel.y < origin.y + side;
}

uint32_t clipmap::serving_level(const glm::dvec2& point_m) const {
  for (uint32_t k = first_resident_; k < first_resident_ + resident_count_; ++k) {
    if (ready(k) && contains(point_m, k)) return k;
  }
  return level_count_;
}

level_pick clipmap::pick(const glm::dvec2& point_m, const double meters_per_pixel) const {
  level_pick out{};

  const double octave = meters_per_pixel <= base_texel_m_ ? 0.0 : std::log2(meters_per_pixel / base_texel_m_);
  const double clamped = std::clamp(octave, 0.0, double(level_count_ - 1));

  // Непрерывная октава даёт и пару уровней, и вес между ними. Округление здесь было бы ровно тем
  // переключением на границе, ради устранения которого и заводился гистерезис.
  const auto ideal = uint32_t(std::floor(clamped));
  out.weight = clamped - std::floor(clamped);

  out.fine = serving_level(point_m);
  if (out.fine >= level_count_) return out;
  out.covered = true;

  // Мелкий уровень не может быть мельче идеального: показывать тексель вчетверо мельче пикселя значит
  // платить памятью и трафиком за то, чего не видно.
  for (uint32_t k = out.fine; k < ideal && k + 1 < level_count_; ++k) {
    if (!ready(k + 1) || !contains(point_m, k + 1)) break;
    out.fine = k + 1;
    out.weight = clamped - double(out.fine);
  }
  out.weight = std::clamp(out.weight, 0.0, 1.0);

  out.coarse = out.fine;
  if (out.fine + 1 < level_count_ && ready(out.fine + 1) && contains(point_m, out.fine + 1)) {
    out.coarse = out.fine + 1;
  } else {
    out.weight = 0.0; // смешивать не с чем, и притворяться обратного нельзя
  }
  return out;
}

zone_id clipmap::sample(const glm::dvec2& point_m, const uint32_t level) const {
  if (!ready(level)) return invalid_zone;

  const double size = texel_size_[level];
  const int64_t side = int64_t(config_.side);
  const glm::i64vec2 texel{int64_t(std::floor(point_m.x / size)), int64_t(std::floor(point_m.y / size))};
  const auto origin = levels_[level].origin;

  if (!contains(point_m, level)) return invalid_zone;
  return levels_[level].texels[size_t(wrap_texel(texel.y, side) * side + wrap_texel(texel.x, side))];
}

std::vector<zone_id> clipmap::bake_reference(const uint32_t level, const glm::dvec2& center_m) const {
  const int64_t side = int64_t(config_.side);
  const auto origin = origin_for(center_m, level);
  const auto value = level_tier_[level];

  std::vector<zone_id> out(size_t(side) * side, invalid_zone);
  for (int64_t y = origin.y; y < origin.y + side; ++y) {
    const int64_t row = wrap_texel(y, side) * side;
    for (int64_t x = origin.x; x < origin.x + side; ++x) {
      out[size_t(row + wrap_texel(x, side))] = map_->resolve(texel_center_m({x, y}, level), value);
    }
  }
  return out;
}

} // namespace devils_engine::pf09
