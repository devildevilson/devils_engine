#ifndef GN07_NODE_SCOPE_H
#define GN07_NODE_SCOPE_H

#include <cstdint>
#include <string_view>

#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>
#include <tavl/parser.h>

#include "location_model.h"

// Скоуп узла для предикатов мест. Скоуп ds обязан быть не длиннее 16 байт и тривиально
// копируемым, поэтому он держит указатель на пару (граф, архетип) и номер узла.
//
// Словарь, который видит предикат, — вычисленные свойства графа плюс объявленная мета вида узла:
//   depth, depth_share, secret_depth, route_length, subtree       — числа
//   dead_end, on_secret_way, hidden_entry                          — признаки
//   has_tag = X, kind_is = X, function_is = X, at_side = X         — проверки по имени
//   has_fact = { имя, значение }                                   — факт локации с наследованием
// Имя, которого архетип не объявлял (опечатка в теге или виде), — исключение при первом же
// вычислении, а не тихое «нет»: иначе опечатка молча делает место пустым.

namespace gn07 {

struct location_view {
  const location_graph* graph = nullptr;
  const archetype* arch = nullptr;
  const std::vector<fact>* facts = nullptr; // факты локации; nullptr — фактов нет
};

struct node_ref {
  const location_view* view = nullptr;
  uint32_t index = 0;
  bool valid() const noexcept { return view != nullptr; }
};

static_assert(sizeof(node_ref) <= 16);

class script_env {
public:
  script_env();

  void configure_parser(tavl::parser& parser) const;
  // Разбирает одно значение прямо из парсера. `parse` с tavl::parser сообщает об отказе
  // ВОЗВРАТОМ и глушит бросок обработчика, поэтому возврат проверяется здесь, всегда.
  devils_script::container compile_predicate(std::string_view name, tavl::parser& parser) const;
  // То же из текста — для самопроверки словаря в --verify.
  devils_script::container compile_text(std::string_view name, std::string_view text) const;
  bool evaluate(const devils_script::container& program, const location_view& view, uint32_t node) const;

private:
  devils_script::system sys_;
  mutable devils_script::context vm_;
};

} // namespace gn07

#endif
