#include "devils_engine/act/registry.h"

#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace act {

fn_id registry::reg(const std::string_view& name, std::unique_ptr<function_base> f) {
  if (f == nullptr) utils::error{}("act::registry::reg: попытка зарегистрировать nullptr (имя '{}')", name);
  const fn_id id = utils::string_hash(name);
  const auto [itr, inserted] = functions.emplace(id, std::move(f));
  if (!inserted) {
    // Различаем ДВА случая (раньше был один assert на оба): повторная регистрация того же ИМЕНИ
    // (ошибка в коде загрузки) vs настоящая хеш-коллизия двух РАЗНЫХ имён (переименуй одну функцию).
    const auto nitr = names_.find(id);
    const std::string_view prev = nitr != names_.end() ? std::string_view(nitr->second) : std::string_view("<unknown>");
    if (prev == name) {
      utils::error{}("act::registry::reg: функция '{}' уже зарегистрирована (повторная регистрация)", name);
    } else {
      utils::error{}("act::registry::reg: хеш-коллизия fn_id {:#x} между именами '{}' и '{}' — переименуй одну", id, prev, name);
    }
  }
  names_.emplace(id, std::string(name));
  return id;
}

const function_base* registry::get(const fn_id id) const noexcept {
  const auto itr = functions.find(id);
  return itr != functions.end() ? itr->second.get() : nullptr;
}

}
}
