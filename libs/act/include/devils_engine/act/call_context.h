#ifndef DEVILS_ENGINE_ACT_CALL_CONTEXT_H
#define DEVILS_ENGINE_ACT_CALL_CONTEXT_H

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <devils_script/context.h>

#include "devils_engine/utils/string_id.h"
#include "value.h"

namespace devils_engine {
namespace act {

// Mutable-часть одного gameplay-вызова. exec_context описывает неизменяемое окружение
// (world/scopes/rng/tick/sink), а call_context — значения, которые функция вправе менять:
// именованные in/out-аргументы, результат и именованные списки. Контейнеры принадлежат вызывающему,
// поэтому один call_context можно reserve() один раз и переиспользовать на worker-е без аллокаций.
struct named_value {
  utils::id name{};
  value data{};
};

struct named_list {
  utils::id name{};
  std::vector<value> values;
};

class call_context {
public:
  static constexpr size_t max_arguments = devils_script::context::script_arguments_size;
  static_assert(max_arguments == 8, "act::call_context expects the standard ds argument stack");

  value result{};

  // Сбрасывает значения, но сохраняет схему имён и capacity вложенных списков: обычный per-worker reuse.
  void clear() noexcept {
    for (auto& a : arguments()) a.data = value{};
    for (auto& l : lists_) l.values.clear();
    result = value{};
  }

  // Полностью забыть схему вызова (load/rebind path, не горячий tick path).
  void clear_schema() noexcept {
    argument_count_ = 0;
    lists_.clear();
    result = value{};
  }

  void reserve(const size_t argument_count, const size_t list_count) {
    if (argument_count > max_arguments) throw std::length_error("act::call_context: too many scalar arguments");
    lists_.reserve(list_count);
  }

  value* find_argument(const utils::id name) noexcept {
    for (auto& a : arguments()) if (a.name == name) return &a.data;
    return nullptr;
  }
  const value* find_argument(const utils::id name) const noexcept {
    for (const auto& a : arguments()) if (a.name == name) return &a.data;
    return nullptr;
  }
  value* find_argument(const std::string_view name) noexcept { return find_argument(utils::string_hash(name)); }
  const value* find_argument(const std::string_view name) const noexcept { return find_argument(utils::string_hash(name)); }

  value& argument(const utils::id name) {
    if (auto* v = find_argument(name); v != nullptr) return *v;
    if (argument_count_ == max_arguments) throw std::length_error("act::call_context: scalar argument stack is full");
    auto& out = arguments_[argument_count_++];
    out = named_value{name, value{}};
    return out.data;
  }
  value& argument(const std::string_view name) { return argument(utils::string_hash(name)); }
  void set(const std::string_view name, const value v) { argument(name) = v; }

  named_list* find_list(const utils::id name) noexcept {
    for (auto& l : lists_) if (l.name == name) return &l;
    return nullptr;
  }
  const named_list* find_list(const utils::id name) const noexcept {
    for (const auto& l : lists_) if (l.name == name) return &l;
    return nullptr;
  }
  named_list* find_list(const std::string_view name) noexcept { return find_list(utils::string_hash(name)); }
  const named_list* find_list(const std::string_view name) const noexcept { return find_list(utils::string_hash(name)); }

  named_list& list(const utils::id name) {
    if (auto* l = find_list(name); l != nullptr) return *l;
    return lists_.emplace_back(named_list{name, {}});
  }
  named_list& list(const std::string_view name) { return list(utils::string_hash(name)); }

  std::span<named_value> arguments() noexcept { return {arguments_.data(), argument_count_}; }
  std::span<const named_value> arguments() const noexcept { return {arguments_.data(), argument_count_}; }
  std::vector<named_list>& lists() noexcept { return lists_; }
  const std::vector<named_list>& lists() const noexcept { return lists_; }

private:
  std::array<named_value, max_arguments> arguments_{};
  size_t argument_count_ = 0;
  std::vector<named_list> lists_;
};

}
}

#endif
