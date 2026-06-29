#include "devils_engine/act/registry.h"

#include "devils_engine/utils/core.h" // utils::assertf

namespace devils_engine {
namespace act {

fn_id registry::reg(const std::string_view& name, std::unique_ptr<function_base> f) {
  utils::assertf(f != nullptr, "act::registry::reg: попытка зарегистрировать nullptr (имя '{}')", name);
  const fn_id id = utils::string_hash(name);
  const auto [itr, inserted] = functions.emplace(id, std::move(f));
  // коллизия хеша = разные имена дали один fn_id; ловим на загрузке, а не в рантайме.
  utils::assertf(inserted, "act::registry::reg: коллизия fn_id для имени '{}' (hash {:#x})", name, id);
  return id;
}

const function_base* registry::get(const fn_id id) const noexcept {
  const auto itr = functions.find(id);
  return itr != functions.end() ? itr->second.get() : nullptr;
}

}
}
