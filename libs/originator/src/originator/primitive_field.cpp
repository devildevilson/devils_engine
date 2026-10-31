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
  return p[0] >= low[0] && p[0] <= high[0] && p[1] >= low[1] && p[1] <= high[1] && p[2] >= low[2] && p[2] <= high[2];
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

void shape_vocabulary::set_background(background_function function, const double upper_bound) {
  background_ = std::move(function);
  background_bound_ = upper_bound;
}

const shape_vocabulary::background_function& shape_vocabulary::background() const noexcept { return background_; }
double shape_vocabulary::background_bound() const noexcept { return background_bound_; }

void shape_vocabulary::add_kind_erased(std::string name, decode_function decode, value_function value) {
  for (const auto& existing : kinds_) {
    if (existing.name == name) utils::error{}("originator field: shape kind '{}' is declared twice", name);
  }
  kinds_.push_back({std::move(name), std::move(decode), std::move(value)});
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
  const auto source = uint32_t(sources_.size());
  sources_.push_back(std::move(decoded.rows));
  for (size_t row = 0; row < decoded.boxes.size(); ++row) {
    shapes_.push_back({uint32_t(kind), source, uint32_t(row), decoded.boxes[row]});
  }
}

const shape_vocabulary& primitive_field::vocabulary() const noexcept { return *vocabulary_; }
uint64_t primitive_field::seed() const noexcept { return seed_; }
size_t primitive_field::shape_count() const noexcept { return shapes_.size(); }

shape_value primitive_field::evaluate(std::span<const uint32_t> candidates, const field_vector& p) const {
  field_point point(*vocabulary_, p, seed_);
  bool stop = false;
  shape_value best = vocabulary_->background()(point, stop);
  if (stop) return best;
  for (const uint32_t index : candidates) {
    const shape& s = shapes_[index];
    if (!s.box.contains(p)) continue;
    const shape_value got = vocabulary_->evaluator(s.kind)(sources_[s.source].get(), s.row, point);
    if (got.value < best.value) best = got;
  }
  return best;
}

shape_value primitive_field::value(const field_vector& p) const {
  thread_local std::vector<uint32_t> all;
  all.resize(shapes_.size());
  for (size_t i = 0; i < shapes_.size(); ++i) all[i] = uint32_t(i);
  return evaluate(all, p);
}

primitive_field::region primitive_field::prepare(const field_vector& low, const field_vector& high) const {
  region out;
  out.field_ = this;
  out.low_ = low;
  out.high_ = high;
  for (size_t i = 0; i < shapes_.size(); ++i) {
    if (shapes_[i].box.overlaps(low, high)) out.shapes_.push_back(uint32_t(i));
  }
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
  primitive_field::sampler samples(field, call.params->number("half", 16.0));
  for (size_t i = 0; i < count; ++i) {
    const shape_value got = samples.sample(read_vector(positions, i));
    out.set(i, got.value);
    if (tagged) tags.set(i, double(got.tag));
  }
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
  add(tool_description{.name = "field_march", .shape = aperture::sequential, .input_count = 2, .output_count = 1,
                       .body = tool_field_march, .footprint = no_temporary_memory});
}

} // namespace originator
} // namespace devils_engine
