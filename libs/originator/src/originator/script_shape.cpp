#include "devils_engine/originator/script_shape.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <memory>
#include <mutex>

#include "devils_engine/originator/script_column.h"
#include "devils_engine/originator/value_noise.h"
#include "devils_engine/utils/core.h"

// Вид формы на ds: строки из буфера экземпляра в плоскую таблицу скаляров, значение — колоночная
// программа над пакетом точек.

namespace devils_engine {
namespace originator {

namespace {

struct field_reference {
  std::string buffer;
  std::string field;
};

field_reference reference_of(const std::string& text, const std::string& buffer) {
  const auto dot = text.find('.');
  if (dot == std::string::npos) return {buffer, text};
  return {text.substr(0, dot), text.substr(dot + 1)};
}

bool number_of(const std::string& text, double& out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc() && end == text.data() + text.size();
}

// Плоская таблица строк: по stride скаляров на строку.
struct script_rows {
  std::vector<double> data;
  size_t stride = 0;
  struct part_rows {
    std::vector<double> data;
    size_t stride = 0;
    std::vector<std::pair<size_t, size_t>> ranges;
    std::vector<field_box> boxes;
  };
  std::vector<part_rows> parts;
};

std::vector<std::string> scalar_names(const buffer& rows, const std::string& prefix = {}) {
  std::vector<std::string> names;
  for (const auto& field : rows.layout().fields) {
    if (field.type.components == 1) names.push_back(prefix + field.name);
    else {
      static constexpr std::array<const char*, 4> suffix{"_x", "_y", "_z", "_w"};
      for (uint32_t c = 0; c < field.type.components; ++c) names.push_back(prefix + field.name + suffix[c]);
    }
  }
  return names;
}

void copy_scalars(const buffer& rows, const size_t count, std::vector<double>& data, size_t& stride) {
  const auto& layout = rows.layout();
  for (const auto& field : layout.fields) stride += field.type.components;
  data.reserve(stride * count);
  for (size_t i = 0; i < count; ++i) {
    for (size_t f = 0; f < layout.fields.size(); ++f) {
      const auto accessor = rows.field(f);
      for (uint32_t c = 0; c < layout.fields[f].type.components; ++c) data.push_back(accessor.get(i, c));
    }
  }
}

// Общее состояние вида: программа компилируется один раз, при первом разборе.
struct script_kind_state {
  chain_shape shape;
  double background_upper = 0.0;
  bool max_can_skip = false;
  // Имена слотов словаря на момент объявления вида (словарь потом может переехать: держать на него
  // указатель нельзя). Слоты объявляются раньше видов.
  std::vector<std::string> slot_names;
  std::once_flag once;
  std::unique_ptr<column_program> program;
  std::vector<size_t> slot_of_column; // столбцы 3.. — слоты словаря
  std::string layout_buffer;          // раскладка, против которой собрана программа
  size_t tag_scalar = 0;              // метка из поля строки: номер её скаляра
  // Досягаемость коробки выражением по полям строки (не число и не одно поле): считается на строку.
  std::unique_ptr<column_program> reach_program;
  struct compiled_part {
    std::unique_ptr<column_program> program;
    bool maximum = false;
  };
  std::vector<compiled_part> parts;

  void compile(const generator_result& instance, const buffer& rows) {
    std::call_once(once, [&] {
      std::vector<std::string> scalars = scalar_names(rows);
      std::vector<std::string> columns{"x", "y", "z"};
      for (size_t k = 0; k < slot_names.size(); ++k) {
        columns.push_back(slot_names[k]);
        slot_of_column.push_back(k);
      }
      program = column_program::compile(shape.path, shape.text, columns, scalars);
      double fixed_reach = 0.0;
      if (!number_of(shape.reach, fixed_reach)) {
        // Одно поле — тоже выражение: программа из имени читает скаляр.
        reach_program = column_program::compile(shape.name + ".reach", "{ " + shape.reach + " }",
                                                std::span<const std::string>{}, scalars);
      }
      double fixed = 0.0;
      if (!shape.tag.empty() && !number_of(shape.tag, fixed)) {
        const auto found = std::find(scalars.begin(), scalars.end(), shape.tag);
        if (found == scalars.end()) {
          utils::error{}("originator shape '{}': tag '{}' is not a field of '{}'", shape.name, shape.tag, rows.name());
        }
        tag_scalar = size_t(found - scalars.begin());
      }
      layout_buffer = rows.name();
      for (const auto& part : shape.parts) {
        const buffer& children = instance.at(part.buffer);
        auto names = scalars;
        auto child_names = scalar_names(children, "child_");
        names.insert(names.end(), child_names.begin(), child_names.end());
        parts.push_back({column_program::compile(part.path, part.text, columns, names), part.op == "max"});
      }
    });
  }
};

} // namespace

void configure_script_field(shape_vocabulary& words, const chain_description& chain, const parameters* constants) {
  for (const auto& slot : chain.noise_slots) {
    words.add_slot(slot.name, [offset = slot.seed_add, period = slot.period](field_point& p) {
      return value_noise(p.seed() + offset, p.position(), period);
    });
  }
  if (!chain.has_background) return;
  auto b = chain.background;
  if ((!b.y_min_constant.empty() || !b.y_max_constant.empty()) && constants == nullptr) {
    utils::error{}("originator field: background references constants but none were supplied");
  }
  for (const auto& name : {b.y_min_constant, b.y_max_constant}) {
    if (!name.empty() && !constants->has(name)) {
      utils::error{}("originator field: background constant '{}' is missing", name);
    }
  }
  if (!b.y_min_constant.empty()) b.y_min = constants->number(b.y_min_constant);
  if (!b.y_max_constant.empty()) b.y_max = constants->number(b.y_max_constant);
  if (!std::isfinite(b.y_min) || !std::isfinite(b.y_max) || b.y_min > b.y_max) {
    utils::error{}("originator field: invalid background vertical bounds");
  }
  std::vector<std::pair<size_t, double>> terms;
  for (const auto& term : b.terms) terms.emplace_back(words.slot(term.slot), term.weight);
  words.set_background(
    [b, terms = std::move(terms)](field_point& p, bool& stop) {
      if (p.position()[1] < b.y_min || p.position()[1] > b.y_max) {
        stop = true;
        return shape_value{b.outside, b.tag};
      }
      double sum = 0.0;
      for (const auto& [slot, weight] : terms) sum += weight * p.slot(slot);
      return shape_value{b.base + sum, b.tag};
    }, b.upper, b.lower);
}

void add_script_kind(shape_vocabulary& words, const chain_shape& shape) {
  auto state = std::make_shared<script_kind_state>();
  state->shape = shape;
  state->background_upper = words.background_bound();
  state->max_can_skip = shape.role.empty() || shape.role == "natural";
  for (size_t k = 0; k < words.slot_count(); ++k) state->slot_names.emplace_back(words.slot_name(k));

  auto decode = [state](const generator_result& instance) {
    const chain_shape& s = state->shape;
    const buffer& rows = instance.at(s.buffer);
    state->compile(instance, rows);
    size_t count = rows.count();
    if (!s.count.empty()) {
      const auto ref = reference_of(s.count, s.buffer);
      count = size_t(instance.field(ref.buffer, ref.field).get(0));
      if (count > rows.count()) {
        utils::error{}("originator shape '{}': '{}' says {} rows, the buffer holds {}", s.name, s.count, count,
                       rows.count());
      }
    }
    auto table = std::make_shared<script_rows>();
    copy_scalars(rows, count, table->data, table->stride);
    for (const auto& part : s.parts) {
      const buffer& children = instance.at(part.buffer);
      script_rows::part_rows items;
      copy_scalars(children, children.count(), items.data, items.stride);
      if (!part.box_a.empty()) {
        const auto low = instance.field(part.buffer, part.box_a);
        const auto high = instance.field(part.buffer, part.box_b);
        if (low.type().components != 3 || high.type().components != 3) {
          utils::error{}("originator shape '{}': part '{}' boxes need 3 components", s.name, part.buffer);
        }
        for (size_t i = 0; i < children.count(); ++i) {
          field_box box;
          for (size_t axis = 0; axis < 3; ++axis) {
            box.low[axis] = low.get(i, uint32_t(axis));
            box.high[axis] = high.get(i, uint32_t(axis));
            if (box.low[axis] > box.high[axis]) {
              utils::error{}("originator shape '{}': part '{}' row {} has inverted box", s.name, part.buffer, i);
            }
          }
          items.boxes.push_back(box);
        }
      }
      const auto first = instance.field(s.buffer, part.first), many = instance.field(s.buffer, part.count);
      for (size_t i = 0; i < count; ++i) {
        const size_t begin = size_t(first.get(i)), amount = size_t(many.get(i));
        if (begin > children.count() || amount > children.count() - begin) {
          utils::error{}("originator shape '{}': part '{}' range {}+{} exceeds {} rows", s.name,
                         part.buffer, begin, amount, children.count());
        }
        items.ranges.emplace_back(begin, amount);
      }
      table->parts.push_back(std::move(items));
    }
    const auto point_of = [&](const std::string& name, const size_t i) {
      const auto ref = reference_of(name, s.buffer);
      const auto f = instance.field(ref.buffer, ref.field);
      return std::array<double, 3>{f.get(i, 0), f.get(i, 1), f.get(i, 2)};
    };
    double fixed_reach = 0.0;
    const bool reach_is_number = number_of(s.reach, fixed_reach);
    shape_vocabulary::decoded out;
    out.boxes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto a = point_of(s.box_a, i);
      const auto b = s.box_b.empty() ? a : point_of(s.box_b, i);
      double reach = fixed_reach;
      if (!reach_is_number) {
        const std::span<const double> scalars(table->data.data() + i * table->stride, table->stride);
        state->reach_program->run(1, std::span<const double* const>{}, scalars, &reach);
      }
      if (!std::isfinite(reach) || reach < 0.0) {
        utils::error{}("originator shape '{}': row {} has invalid reach {}", s.name, i, reach);
      }
      // Ящик сохраняет заданные углы; капсула дополнительно отсекает точки по расстоянию до оси.
      if (s.box == "aabb") {
        field_box box;
        for (size_t axis = 0; axis < 3; ++axis) {
          if (a[axis] > b[axis]) utils::error{}("originator shape '{}': inverted aabb on axis {}", s.name, axis);
          box.low[axis] = a[axis] - reach;
          box.high[axis] = b[axis] + reach;
        }
        out.boxes.push_back(box);
      } else {
        out.boxes.push_back(field_box::capsule(a, b, reach));
      }
    }
    out.rows = table;
    return out;
  };

  // Метка: поле строки (номер скаляра в таблице) или число.
  auto batch = [state](const void* rows, const size_t row, field_batch& points) {
    const auto& table = *static_cast<const script_rows*>(rows);
    thread_local std::vector<std::array<double, column_program::lanes>> slots;
    if (slots.size() < state->slot_of_column.size()) slots.resize(state->slot_of_column.size());
    const auto run = [&](const column_program& program, const std::span<const double> scalars,
                         const field_batch& input, double* output) {
      std::array<const double*, 3 + field_point::max_slots> columns{input.x, input.y, input.z};
      const auto& used = program.used_columns();
      for (size_t j = 0; j < state->slot_of_column.size(); ++j) {
        if (!used[3 + j]) { columns[3 + j] = nullptr; continue; }
        for (size_t l = 0; l < input.count; ++l) slots[j][l] = input.points[l]->slot(state->slot_of_column[j]);
        columns[3 + j] = slots[j].data();
      }
      program.run(input.count, std::span<const double* const>(columns.data(), 3 + state->slot_of_column.size()),
                  scalars, output);
    };
    const std::span<const double> scalars(table.data.data() + row * table.stride, table.stride);
    run(*state->program, scalars, points, points.values);
    thread_local std::vector<double> combined;
    std::array<double, column_program::lanes> child_values{};
    std::array<double, column_program::lanes> x{}, y{}, z{};
    std::array<field_point*, column_program::lanes> lanes{};
    std::array<uint32_t, column_program::lanes> indices{}, dummy_tags{};
    for (size_t j = 0; j < state->parts.size(); ++j) {
      const auto& part = state->parts[j];
      const auto& children = table.parts[j];
      const auto [first, count] = children.ranges[row];
      combined.resize(table.stride + children.stride);
      std::copy(scalars.begin(), scalars.end(), combined.begin());
      for (size_t i = first; i < first + count; ++i) {
        field_batch selected = points;
        if ((part.maximum && state->max_can_skip) || !children.boxes.empty()) {
          size_t n = 0;
          for (size_t l = 0; l < points.count; ++l) {
            if (part.maximum && state->max_can_skip && points.values[l] > state->background_upper) continue;
            if (!children.boxes.empty() && !children.boxes[i].contains(points.points[l]->position())) continue;
            x[n] = points.x[l]; y[n] = points.y[l]; z[n] = points.z[l];
            lanes[n] = points.points[l]; indices[n] = uint32_t(l);
            ++n;
          }
          if (n == 0) continue;
          selected = {n, x.data(), y.data(), z.data(), lanes.data(), child_values.data(), dummy_tags.data()};
        }
        const auto start = children.data.begin() + i * children.stride;
        std::copy_n(start, children.stride, combined.begin() + table.stride);
        run(*part.program, combined, selected, child_values.data());
        for (size_t l = 0; l < selected.count; ++l) {
          const size_t target = ((part.maximum && state->max_can_skip) || !children.boxes.empty()) ? indices[l] : l;
          points.values[target] = part.maximum ? std::max(points.values[target], child_values[l])
                                                : std::min(points.values[target], child_values[l]);
        }
      }
    }
    const chain_shape& s = state->shape;
    double tag = 0.0;
    if (!number_of(s.tag, tag) && !s.tag.empty()) tag = scalars[state->tag_scalar];
    for (size_t l = 0; l < points.count; ++l) points.tags[l] = uint32_t(tag);
  };
  words.add_kind_batch(shape.name, std::move(decode), std::move(batch));
  if (shape.role.empty()) words.set_role(shape.name, shape_role::natural, shape.blend);
  else if (shape.role == "fill") words.set_role(shape.name, shape_role::fill, shape.blend);
  else if (shape.role == "cut") words.set_role(shape.name, shape_role::cut, shape.blend);
  else if (shape.role == "insert") words.set_role(shape.name, shape_role::insert, shape.blend);
}

} // namespace originator
} // namespace devils_engine
