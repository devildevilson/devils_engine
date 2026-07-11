#ifndef TILE_FRONTIER_CORE_SCRIPT_RESOURCE_H
#define TILE_FRONTIER_CORE_SCRIPT_RESOURCE_H

#include <string>

#include <devils_engine/demiurg/resource_base.h>
#include <devils_engine/act/function.h> // act::category
#include <devils_script/container.h>

namespace devils_script { struct system; }

namespace tile_frontier {
namespace core {

// tavl-документ скрипта: тип возврата, root-скоуп и текст выражения. Срез поддерживает только
// ret="bool" / scope="actor" (entity_scope). Позже — общий диспетчер (ret,scope) -> parse<RetT,ROOT>.
struct actor_script_config {
  std::string ret;   // "bool"
  std::string scope; // "actor"
  std::string expr;  // текст скрипта devils_script, напр. "hunger >= 0.5"
};

// demiurg-ресурс скрипта: на load_cold парсит tavl → компилирует expr через впрыснутый
// devils_script::system → держит готовый container. Потребитель (act::script_function) берёт
// program() и исполняет его на пер-воркер ds::context. system нужен только тут (на парсе).
class script_resource : public devils_engine::demiurg::resource_interface {
public:
  explicit script_resource(devils_script::system* sys);

  const devils_script::container* program() const noexcept { return &program_; }
  devils_engine::act::category category() const noexcept { return category_; }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  devils_script::system* sys_ = nullptr; // заимствован (владелец — assets script_environment)
  devils_script::container program_;
  devils_engine::act::category category_ = devils_engine::act::category::predicate;
};

}
}

#endif
