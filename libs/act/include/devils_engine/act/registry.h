#ifndef DEVILS_ENGINE_ACT_REGISTRY_H
#define DEVILS_ENGINE_ACT_REGISTRY_H

#include <memory>
#include <string>
#include <string_view>
#include <gtl/phmap.hpp>
#include "devils_engine/utils/string_id.h" // utils::id, string_hash
#include "function.h"

namespace devils_engine {
namespace act {
// registry — глобальная таблица геймплейных функций. fn_id = string_hash(name): именам
// функций плотный индекс НЕ нужен (решение автора). ВАЖНО: GOAP-флаги состояний живут
// ОТДЕЛЬНО через utils::string_pool (плотные bit-position id) — другое пространство
// имён, НЕ конфликтовать с этим.
//
// Контракт потокобезопасности (как у string_pool): reg() — ТОЛЬКО в однопоточной фазе
// загрузки модулей; get()/call() потокобезопасны после неё. Потребители (acumen/mood)
// кэшируют типизированный указатель при сборке плана/таблицы → lookup уходит из цикла A*.

using fn_id = utils::id;

class registry {
public:
  // регистрация владеющей функции; возвращает её fn_id (= string_hash(name)).
  fn_id reg(const std::string_view& name, std::unique_ptr<function_base> f);

  // generic-доступ (nullptr если нет).
  const function_base* get(const fn_id id) const noexcept;
  bool has(const fn_id id) const noexcept { return get(id) != nullptr; }

  // типизированный доступ с проверкой категории (nullptr при несовпадении категории).
  template <typename RetT>
  const function<RetT>* get_typed(const fn_id id) const noexcept {
    const auto* f = get(id);
    return (f != nullptr && f->cat == detail::category_of<RetT>())
      ? static_cast<const function<RetT>*>(f) : nullptr;
  }

  const predicate_function* predicate(const fn_id id) const noexcept { return get_typed<bool>(id); }
  const number_function*    number(const fn_id id)    const noexcept { return get_typed<real_t>(id); }
  const effect_function*    effect(const fn_id id)    const noexcept { return get_typed<void>(id); }
  const string_function*    string_fn(const fn_id id) const noexcept { return get_typed<utils::id>(id); }
  const object_function*    object(const fn_id id)    const noexcept { return get_typed<entity_id>(id); }

private:
  gtl::flat_hash_map<fn_id, std::unique_ptr<function_base>> functions;
  // имена по fn_id — только для ДИАГНОСТИКИ на загрузке: отличить повторную регистрацию того же
  // имени от настоящей хеш-коллизии двух РАЗНЫХ имён (см. reg()). Не используется в рантайме.
  gtl::flat_hash_map<fn_id, std::string> names_;
};

}
}

#endif
