#include "devils_engine/originator/primitive_field.h"

#include <algorithm>
#include <cmath>

#include "devils_engine/utils/core.h"

// Поле из примитивов: сборка, отсечение по коробкам влияния, запросы.
//
// ОЦЕНКА ТОЧКИ: фон, затем формы-кандидаты по порядку, минимум строгим `<` (ничья оставляет раннюю
// форму). Кандидат, чья коробка не содержит точку, пропускается без расчёта: вне коробки он не ниже
// оценки фона и минимум не меняет.
//
// ИНДЕКСА ПРОСТРАНСТВА НЕТ: формы окна — сотни, и линейный проход по коробкам при подготовке
// области дешевле любого дерева; горячий путь — уже подготовленная область. Дерево понадобится,
// когда поле станет тысячами форм за раз.

namespace devils_engine {
namespace originator {

namespace {
// Поля текущего исполняемого генератора: ставит сборка экземпляра (generator_chain.cpp).
thread_local const std::vector<std::pair<std::string, std::shared_ptr<const primitive_field>>>* active_fields = nullptr;
} // namespace

namespace detail {
const std::vector<std::pair<std::string, std::shared_ptr<const primitive_field>>>* exchange_active_fields(
  const std::vector<std::pair<std::string, std::shared_ptr<const primitive_field>>>* fields) noexcept {
  const auto* previous = active_fields;
  active_fields = fields;
  return previous;
}
} // namespace detail

bool field_box::contains(const field_vector& p) const noexcept {
  if (!(p[0] >= low[0] && p[0] <= high[0] && p[1] >= low[1] && p[1] <= high[1] && p[2] >= low[2] && p[2] <= high[2])) {
    return false;
  }
  if (capsule_radius < 0.0) return true;
  const double bx = b[0] - a[0], by = b[1] - a[1], bz = b[2] - a[2];
  const double px = p[0] - a[0], py = p[1] - a[1], pz = p[2] - a[2];
  const double length2 = bx * bx + by * by + bz * bz;
  const double t = length2 > 0.0 ? std::clamp((px * bx + py * by + pz * bz) / length2, 0.0, 1.0) : 0.0;
  const double dx = px - bx * t, dy = py - by * t, dz = pz - bz * t;
  return dx * dx + dy * dy + dz * dz <= capsule_radius * capsule_radius;
}

field_box field_box::capsule(const field_vector& a, const field_vector& b, const double radius) noexcept {
  field_box out = around(a, b, radius);
  out.a = a;
  out.b = b;
  out.capsule_radius = radius;
  return out;
}

bool field_box::overlaps(const field_vector& other_low, const field_vector& other_high) const noexcept {
  return low[0] <= other_high[0] && high[0] >= other_low[0] && low[1] <= other_high[1] && high[1] >= other_low[1] &&
         low[2] <= other_high[2] && high[2] >= other_low[2];
}

field_box field_box::around(const field_vector& a, const field_vector& b, const double radius) noexcept {
  field_box out;
  for (size_t i = 0; i < 3; ++i) {
    out.low[i] = std::min(a[i], b[i]) - radius;
    out.high[i] = std::max(a[i], b[i]) + radius;
  }
  return out;
}

field_point::field_point(const shape_vocabulary& vocabulary, const field_vector& position, const uint64_t seed) noexcept :
  vocabulary_(&vocabulary), position_(position), seed_(seed) {}

const field_vector& field_point::position() const noexcept { return position_; }
uint64_t field_point::seed() const noexcept { return seed_; }

double field_point::slot(const size_t index) {
  if (index >= max_slots) {
    utils::error{}("originator field: slot {} is beyond the {} a point holds", index, max_slots);
  }
  if ((ready_ & (1u << index)) == 0) {
    values_[index] = vocabulary_->slot_at(index)(*this);
    ready_ |= 1u << index;
  }
  return values_[index];
}

size_t shape_vocabulary::add_slot(std::string name, slot_function compute) {
  if (slots_.size() >= field_point::max_slots) {
    utils::error{}("originator field: at most {} slots, '{}' is one too many", field_point::max_slots, name);
  }
  for (const auto& existing : slots_) {
    if (existing.name == name) utils::error{}("originator field: slot '{}' is declared twice", name);
  }
  slots_.push_back({std::move(name), std::move(compute)});
  return slots_.size() - 1;
}

size_t shape_vocabulary::slot(const std::string_view& name) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].name == name) return i;
  }
  utils::error{}("originator field: no slot '{}'", name);
  return 0;
}

const shape_vocabulary::slot_function& shape_vocabulary::slot_at(const size_t index) const noexcept {
  return slots_[index].compute;
}

void shape_vocabulary::set_background(background_function function, const double upper_bound, const double lower_bound) {
  background_ = std::move(function);
  background_bound_ = upper_bound;
  background_lower_ = lower_bound;
}

double shape_vocabulary::background_lower() const noexcept { return background_lower_; }

const shape_vocabulary::background_function& shape_vocabulary::background() const noexcept { return background_; }
double shape_vocabulary::background_bound() const noexcept { return background_bound_; }

void shape_vocabulary::add_kind_erased(std::string name, decode_function decode, value_function value) {
  for (const auto& existing : kinds_) {
    if (existing.name == name) utils::error{}("originator field: shape kind '{}' is declared twice", name);
  }
  kinds_.push_back({std::move(name), std::move(decode), std::move(value), {}});
}

void shape_vocabulary::add_kind_batch(std::string name, decode_function decode, batch_function batch) {
  for (const auto& existing : kinds_) {
    if (existing.name == name) utils::error{}("originator field: shape kind '{}' is declared twice", name);
  }
  // Поточечная форма — пакет из одной точки: одна формула, два способа звать.
  value_function value = [batch](const void* rows, const size_t row, field_point& point) {
    const auto& p = point.position();
    field_point* lane = &point;
    double result = 0.0;
    uint32_t tag = 0;
    field_batch one{1, &p[0], &p[1], &p[2], &lane, &result, &tag};
    batch(rows, row, one);
    return shape_value{result, tag};
  };
  kinds_.push_back({std::move(name), std::move(decode), std::move(value), std::move(batch)});
}

void shape_vocabulary::set_role(const std::string_view& name, const shape_role role, const double blend) {
  if (blend < 0.0) utils::error{}("originator field: shape kind '{}' has a negative blend {}", name, blend);
  auto& entry = kinds_[kind(name)];
  entry.role = role;
  entry.blend = blend;
}

shape_role shape_vocabulary::role(const size_t kind) const noexcept { return kinds_[kind].role; }
double shape_vocabulary::blend(const size_t kind) const noexcept { return kinds_[kind].blend; }

const shape_vocabulary::batch_function& shape_vocabulary::batcher(const size_t kind) const noexcept {
  return kinds_[kind].batch;
}

size_t shape_vocabulary::kind(const std::string_view& name) const {
  for (size_t i = 0; i < kinds_.size(); ++i) {
    if (kinds_[i].name == name) return i;
  }
  utils::error{}("originator field: no shape kind '{}'", name);
  return 0;
}

const shape_vocabulary::decode_function& shape_vocabulary::decoder(const size_t kind) const noexcept {
  return kinds_[kind].decode;
}

const shape_vocabulary::value_function& shape_vocabulary::evaluator(const size_t kind) const noexcept {
  return kinds_[kind].value;
}

primitive_field::primitive_field(const shape_vocabulary& vocabulary, std::span<const field_term_source> terms,
                                 const uint64_t seed) :
  vocabulary_(&vocabulary), seed_(seed) {
  if (!vocabulary.background()) {
    utils::error{}("originator field: the vocabulary has no background");
  }
  for (const auto& term : terms) {
    const size_t kind = vocabulary.kind(term.kind);
    for (const auto& instance : term.instances) add_rows(kind, instance->decoded_rows(vocabulary, kind));
  }
}

primitive_field::primitive_field(const shape_vocabulary& vocabulary, std::span<const field_term_rows> terms,
                                 const uint64_t seed) :
  vocabulary_(&vocabulary), seed_(seed) {
  if (!vocabulary.background()) {
    utils::error{}("originator field: the vocabulary has no background");
  }
  for (const auto& term : terms) add_rows(vocabulary.kind(term.kind), term.rows);
}

void primitive_field::add_rows(const size_t kind, shape_vocabulary::decoded decoded) {
  if (decoded.boxes.empty()) return;
  if (decoded.rows == nullptr) {
    utils::error{}("originator field: {} boxes of a '{}' term come without rows", decoded.boxes.size(), kind);
  }
  const void* rows = decoded.rows.get();
  sources_.push_back(std::move(decoded.rows));
  for (size_t row = 0; row < decoded.boxes.size(); ++row) {
    shapes_.push_back(shape{uint32_t(kind), uint32_t(row), rows, decoded.boxes[row]});
  }
}

void primitive_field::collect(const field_vector& low, const field_vector& high, std::vector<const void*>& out) const {
  for (const auto& s : shapes_) {
    if (s.box.overlaps(low, high)) out.push_back(&s);
  }
}

const shape_vocabulary& primitive_field::vocabulary() const noexcept { return *vocabulary_; }
uint64_t primitive_field::seed() const noexcept { return seed_; }
size_t primitive_field::shape_count() const noexcept { return shapes_.size(); }

namespace {

// Сглаженный максимум (заливка): у шва двух пород вместо складки — скругление радиусом blend.
double smooth_max(const double a, const double b, const double blend) {
  if (blend <= 0.0) return std::max(a, b);
  const double h = std::max(blend - std::abs(a - b), 0.0) / blend;
  return std::max(a, b) + h * h * blend * 0.25;
}

// Сложить значение формы своей роли с текущим: природная и вырез — минимум, заливка и вставка —
// сглаженный максимум. Метка — у формы, которая победила.
void combine(const shape_role role, const double blend, const double value, const uint32_t tag, double& best,
             uint32_t& best_tag) {
  if (role == shape_role::fill || role == shape_role::insert) {
    if (value > best) best_tag = tag;
    best = smooth_max(best, value, blend);
  } else if (value < best) {
    best = value;
    best_tag = tag;
  }
}

} // namespace

shape_value primitive_field::evaluate(std::span<const void* const> candidates, const field_vector& p) const {
  field_point point(*vocabulary_, p, seed_);
  bool stop = false;
  shape_value best = vocabulary_->background()(point, stop);
  if (stop) return best;
  // Стадии по порядку: природное, заливка, вырез, вставка.
  for (const shape_role stage : {shape_role::natural, shape_role::fill, shape_role::cut, shape_role::insert}) {
    for (const void* candidate : candidates) {
      const shape& s = *static_cast<const shape*>(candidate);
      if (vocabulary_->role(s.kind) != stage || !s.box.contains(p)) continue;
      const shape_value got = vocabulary_->evaluator(s.kind)(s.rows, s.row, point);
      combine(stage, vocabulary_->blend(s.kind), got.value, got.tag, best.value, best.tag);
    }
  }
  return best;
}

// Без области — та же оценка по всем формам, чьи коробки содержат точку.
shape_value primitive_field::value(const field_vector& p) const {
  thread_local std::vector<const void*> all;
  all.clear();
  collect(p, p, all);
  return evaluate(all, p);
}

void primitive_field::sample_points(std::span<const field_vector> points, std::span<double> values,
                                    std::span<uint32_t> tags, const double half) const {
  if (values.size() < points.size() || (!tags.empty() && tags.size() < points.size())) {
    utils::error{}("originator field: {} points need as many values and tags", points.size());
  }
  const double side = std::max(2.0 * half, 1e-6);
  // Клетки по порядку первой точки: от разбиения зависит только цена, не ответ.
  std::vector<std::pair<std::array<int64_t, 3>, uint32_t>> keyed(points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    keyed[i] = {{int64_t(std::floor(points[i][0] / side)), int64_t(std::floor(points[i][1] / side)),
                 int64_t(std::floor(points[i][2] / side))},
                uint32_t(i)};
  }
  std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  constexpr size_t lanes = 256;
  std::vector<field_point> cell_points;
  std::vector<double> best;
  std::vector<uint32_t> best_tag;
  std::vector<uint8_t> stopped;
  std::vector<const void*> candidates;
  std::array<double, lanes> x{}, y{}, z{}, got{};
  std::array<uint32_t, lanes> got_tag{}, lane_of{};
  std::array<field_point*, lanes> lane_points{};

  for (size_t begin = 0; begin < keyed.size();) {
    size_t end = begin;
    while (end < keyed.size() && keyed[end].first == keyed[begin].first) ++end;
    field_vector low = points[keyed[begin].second], high = low;
    cell_points.clear();
    for (size_t k = begin; k < end; ++k) {
      const auto& p = points[keyed[k].second];
      for (size_t a = 0; a < 3; ++a) {
        low[a] = std::min(low[a], p[a]);
        high[a] = std::max(high[a], p[a]);
      }
      cell_points.emplace_back(*vocabulary_, p, seed_);
    }
    const size_t n = end - begin;
    best.assign(n, 0.0);
    best_tag.assign(n, 0);
    stopped.assign(n, 0);
    for (size_t k = 0; k < n; ++k) {
      bool stop = false;
      const shape_value b = vocabulary_->background()(cell_points[k], stop);
      best[k] = b.value;
      best_tag[k] = b.tag;
      stopped[k] = stop;
    }
    candidates.clear();
    collect(low, high, candidates);
    // Стадии по порядку: природное, заливка, вырез, вставка (внутри стадии — порядок слагаемых).
    std::stable_sort(candidates.begin(), candidates.end(), [this](const void* a, const void* b) {
      return vocabulary_->role(static_cast<const shape*>(a)->kind) < vocabulary_->role(static_cast<const shape*>(b)->kind);
    });
    for (const void* candidate : candidates) {
      const shape& s = *static_cast<const shape*>(candidate);
      const shape_role role = vocabulary_->role(s.kind);
      const double blend = vocabulary_->blend(s.kind);
      const auto& batch = vocabulary_->batcher(s.kind);
      if (!batch) {
        const auto& evaluate_one = vocabulary_->evaluator(s.kind);
        for (size_t k = 0; k < n; ++k) {
          if (stopped[k] || !s.box.contains(cell_points[k].position())) continue;
          const shape_value v = evaluate_one(s.rows, s.row, cell_points[k]);
          combine(role, blend, v.value, v.tag, best[k], best_tag[k]);
        }
        continue;
      }
      size_t filled = 0;
      const auto flush = [&] {
        if (filled == 0) return;
        field_batch run{filled, x.data(), y.data(), z.data(), lane_points.data(), got.data(), got_tag.data()};
        batch(s.rows, s.row, run);
        for (size_t l = 0; l < filled; ++l) {
          combine(role, blend, got[l], got_tag[l], best[lane_of[l]], best_tag[lane_of[l]]);
        }
        filled = 0;
      };
      for (size_t k = 0; k < n; ++k) {
        const auto& p = cell_points[k].position();
        if (stopped[k] || !s.box.contains(p)) continue;
        x[filled] = p[0];
        y[filled] = p[1];
        z[filled] = p[2];
        lane_points[filled] = &cell_points[k];
        lane_of[filled] = uint32_t(k);
        if (++filled == lanes) flush();
      }
      flush();
    }
    for (size_t k = 0; k < n; ++k) {
      const uint32_t i = keyed[begin + k].second;
      values[i] = best[k];
      if (!tags.empty()) tags[i] = best_tag[k];
    }
    begin = end;
  }
}

primitive_field::region primitive_field::prepare(const field_vector& low, const field_vector& high) const {
  region out;
  out.field_ = this;
  out.low_ = low;
  out.high_ = high;
  collect(low, high, out.shapes_);
  return out;
}

shape_value primitive_field::region::value(const field_vector& p) const {
  if (!contains(p)) return field_->value(p);
  return field_->evaluate(shapes_, p);
}

const field_vector& primitive_field::region::low() const noexcept { return low_; }
const field_vector& primitive_field::region::high() const noexcept { return high_; }
size_t primitive_field::region::shape_count() const noexcept { return shapes_.size(); }

bool primitive_field::region::contains(const field_vector& p) const noexcept {
  return p[0] >= low_[0] && p[0] <= high_[0] && p[1] >= low_[1] && p[1] <= high_[1] && p[2] >= low_[2] &&
         p[2] <= high_[2];
}

primitive_field::sampler::sampler(const primitive_field& field, const double half) : field_(&field), half_(half) {}

shape_value primitive_field::sampler::sample(const field_vector& p) {
  if (!region_ || !region_->contains(p)) {
    region_ = field_->prepare({p[0] - half_, p[1] - half_, p[2] - half_}, {p[0] + half_, p[1] + half_, p[2] + half_});
  }
  return region_->value(p);
}

double primitive_field::sampler::operator()(const field_vector& p) { return sample(p).value; }

double primitive_field::march(sampler& samples, const field_vector& origin, const field_vector& direction,
                              const double from, const double to, const double step, const bool want_rock,
                              const uint32_t refine) const {
  const auto hit = [&](const double s) {
    const field_vector p{origin[0] + direction[0] * s, origin[1] + direction[1] * s, origin[2] + direction[2] * s};
    const double value = samples(p);
    return want_rock ? value > 0.0 : value <= 0.0;
  };
  for (size_t k = 0;; ++k) {
    const double s = from + double(k) * step;
    if (s > to + 1e-9) break;
    if (!hit(s)) continue;
    if (k == 0 || refine == 0) return s;
    // Граница между последним промахом и попаданием — делением пополам; ответ — сторона попадания.
    double low = s - step, high = s;
    for (uint32_t i = 0; i < refine; ++i) {
      const double mid = (low + high) / 2.0;
      (hit(mid) ? high : low) = mid;
    }
    return high;
  }
  return -1.0;
}

const std::shared_ptr<const primitive_field>& current_field_handle(const std::string_view& name) {
  if (active_fields == nullptr) {
    utils::error{}("originator field: '{}' is asked outside a chain generator build", name);
  }
  for (const auto& [field_name, field] : *active_fields) {
    if (field_name == name) return field;
  }
  utils::error{}("originator field: the generator being built declares no field '{}'", name);
  return active_fields->front().second;
}

const primitive_field& current_field(const std::string_view& name) {
  if (active_fields == nullptr) {
    utils::error{}("originator field: '{}' is asked outside a chain generator build", name);
  }
  for (const auto& [field_name, field] : *active_fields) {
    if (field_name == name) return *field;
  }
  utils::error{}("originator field: the generator being built declares no field '{}'", name);
  return *active_fields->front().second;
}

namespace {

field_vector read_vector(const const_field_accessor& field, const size_t i) {
  return {field.get(i, 0), field.get(i, 1), field.get(i, 2)};
}

size_t checked_count(const tool_call& call, const size_t capacity) {
  const auto count = call.params->integer("count", int64_t(capacity));
  if (count < 0 || size_t(count) > capacity) {
    utils::error{}("originator step '{}': {} got count {} for {} elements", call.step_name, call.tool_name, count,
                   capacity);
  }
  return size_t(count);
}

// Значения поля в точках: вход позиция (3 компоненты), выход значение и, необязательно, метка формы,
// давшей минимум (у мира — вид породы). Параметры: field — имя поля генератора, count — сколько точек
// занято, half — полусторона подготовленной области.
void tool_field_sample(const tool_call& call, const size_t, const size_t) {
  const auto& field = current_field(call.params->string("field"));
  const auto positions = call.input(0).read();
  auto out = call.output(0).write();
  const bool tagged = call.has_output(1);
  const auto tags = tagged ? call.output(1).write() : field_accessor{};
  size_t capacity = std::min(positions.count(), out.count());
  if (tagged) capacity = std::min(capacity, tags.count());
  const size_t count = checked_count(call, capacity);
  std::vector<field_vector> points(count);
  for (size_t i = 0; i < count; ++i) points[i] = read_vector(positions, i);
  std::vector<double> values(count);
  std::vector<uint32_t> got_tags(tagged ? count : 0);
  field.sample_points(points, values, got_tags, call.params->number("half", 16.0));
  for (size_t i = 0; i < count; ++i) {
    out.set(i, values[i]);
    if (tagged) tags.set(i, double(got_tags[i]));
  }
}

// Границы поля над коробкой точек (вход — позиции): выход 0 — сколько форм поля задевает коробку,
// 1 и 2 (необязательные) — нижняя и верхняя граница фона. Ноль форм и нижняя граница выше порога
// поверхности значат «вся коробка — порода»: такую область можно не сэмплировать вовсе.
void tool_field_bound(const tool_call& call, const size_t, const size_t) {
  const auto& field = current_field(call.params->string("field"));
  const auto positions = call.input(0).read();
  if (positions.count() == 0) utils::error{}("originator step '{}': field_bound got no points", call.step_name);
  field_vector low = read_vector(positions, 0), high = low;
  for (size_t i = 1; i < positions.count(); ++i) {
    const field_vector p = read_vector(positions, i);
    for (size_t a = 0; a < 3; ++a) {
      low[a] = std::min(low[a], p[a]);
      high[a] = std::max(high[a], p[a]);
    }
  }
  call.output(0).write().set(0, double(field.prepare(low, high).shape_count()));
  if (call.has_output(1)) call.output(1).write().set(0, field.vocabulary().background_lower());
  if (call.has_output(2)) call.output(2).write().set(0, field.vocabulary().background_bound());
}

// Луч до породы или воды: входы начало и направление (по 3 компоненты), выход расстояние или -1.
// Параметры: field, count, from, to, step, want = rock | water, half, refine — сколько делений пополам
// уточняют границу после первого попадания (0 — ответ с точностью шага).
void tool_field_march(const tool_call& call, const size_t, const size_t) {
  const auto& field = current_field(call.params->string("field"));
  const auto origin = call.input(0).read(), direction = call.input(1).read();
  auto hit = call.output(0).write();
  const size_t count = checked_count(call, std::min({origin.count(), direction.count(), hit.count()}));
  const double from = call.params->number("from", 0.0), to = call.params->number("to", 0.0);
  const double step = call.params->number("step", 0.5);
  const std::string_view want = call.params->string("want", "rock");
  if (want != "rock" && want != "water") {
    utils::error{}("originator step '{}': field_march wants 'rock' or 'water', got '{}'", call.step_name, want);
  }
  if (step <= 0.0) utils::error{}("originator step '{}': field_march step must be positive", call.step_name);
  const auto refine_value = call.params->integer("refine", 0);
  if (refine_value < 0 || refine_value > 64) {
    utils::error{}("originator step '{}': field_march refine {} is outside 0..64", call.step_name, refine_value);
  }
  const auto refine = uint32_t(refine_value);
  primitive_field::sampler samples(field, call.params->number("half", 16.0));
  for (size_t i = 0; i < count; ++i) {
    hit.set(i, field.march(samples, read_vector(origin, i), read_vector(direction, i), from, to, step, want == "rock",
                           refine));
  }
}

} // namespace

void tool_registry::add_field_tools() {
  add(tool_description{.name = "field_sample", .shape = aperture::sequential, .input_count = 1, .output_count = 2,
                       .optional_outputs = 1, .body = tool_field_sample, .footprint = no_temporary_memory});
  add(tool_description{.name = "field_bound", .shape = aperture::sequential, .input_count = 1, .output_count = 3,
                       .optional_outputs = 2, .body = tool_field_bound, .footprint = no_temporary_memory});
  add(tool_description{.name = "field_march", .shape = aperture::sequential, .input_count = 2, .output_count = 1,
                       .body = tool_field_march, .footprint = no_temporary_memory});
}

} // namespace originator
} // namespace devils_engine
