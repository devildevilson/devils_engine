#ifndef DEVILS_ENGINE_ACT_CALL_CONTEXT_H
#define DEVILS_ENGINE_ACT_CALL_CONTEXT_H

#include <string>
#include <string_view>
#include <vector>

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
  value result{};

  // Сбрасывает значения, но сохраняет схему имён и capacity вложенных списков: обычный per-worker reuse.
  void clear() noexcept {
    for (auto& a : arguments_) a.data = value{};
    for (auto& l : lists_) l.values.clear();
    result = value{};
  }

  // Полностью забыть схему вызова (load/rebind path, не горячий tick path).
  void clear_schema() noexcept {
    arguments_.clear();
    lists_.clear();
    result = value{};
  }

  void reserve(const size_t argument_count, const size_t list_count) {
    arguments_.reserve(argument_count);
    lists_.reserve(list_count);
  }

  value* find_argument(const utils::id name) noexcept {
    for (auto& a : arguments_) if (a.name == name) return &a.data;
    return nullptr;
  }
  const value* find_argument(const utils::id name) const noexcept {
    for (const auto& a : arguments_) if (a.name == name) return &a.data;
    return nullptr;
  }
  value* find_argument(const std::string_view name) noexcept { return find_argument(utils::string_hash(name)); }
  const value* find_argument(const std::string_view name) const noexcept { return find_argument(utils::string_hash(name)); }

  value& argument(const utils::id name) {
    if (auto* v = find_argument(name); v != nullptr) return *v;
    return arguments_.emplace_back(named_value{name, value{}}).data;
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

  std::vector<named_value>& arguments() noexcept { return arguments_; }
  const std::vector<named_value>& arguments() const noexcept { return arguments_; }
  std::vector<named_list>& lists() noexcept { return lists_; }
  const std::vector<named_list>& lists() const noexcept { return lists_; }

private:
  std::vector<named_value> arguments_;
  std::vector<named_list> lists_;
};

}
}

#endif
