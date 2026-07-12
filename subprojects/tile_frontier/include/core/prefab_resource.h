#ifndef TILE_FRONTIER_CORE_PREFAB_RESOURCE_H
#define TILE_FRONTIER_CORE_PREFAB_RESOURCE_H

#include <string>
#include <string_view>

#include <devils_engine/demiurg/resource_base.h>

// demiurg-ресурс префаба: держит СЫРОЙ tavl-текст ОДНОГО префаба. Один файл prefab/*.tavl = либо один
// префаб (имя = базовое имя файла), либо список через `//---` (имя = поле `name`, id = `path:name` —
// см. config-file-convention). Слайс регистрирует C++-специи компонентов (data/reference/custom/…) и
// скармливает text() в prefab_registry.add_prefab(prefab_name(), ...). Сам текст движок разбирает —
// ресурс лишь дисковый носитель (парс форм — в prefab_registry, поведение — в act::registry).

namespace tile_frontier {
namespace core {

class prefab_resource : public devils_engine::demiurg::resource_interface {
public:
  prefab_resource();

  std::string_view prefab_name() const noexcept { return name_; }
  std::string_view text() const noexcept { return text_; }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  std::string name_; // логическое имя префаба (для spawn): list_name или базовое имя файла
  std::string text_; // сырой tavl-текст префаба (list-секция или весь файл)
};

}
}

#endif
