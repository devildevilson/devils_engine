#ifndef DEVILS_ENGINE_PREFAB_RESOURCE_H
#define DEVILS_ENGINE_PREFAB_RESOURCE_H

#include <string>
#include <string_view>

#include <devils_engine/demiurg/resource_base.h>

// Generic carrier for one prefab's raw TAVL text. A file may contain one prefab (logical name is the
// file id tail) or demiurg `//---` list entries (logical name is list_name). Component specs and
// construction behavior remain consumer-owned prefab_registry policy.

namespace devils_engine {
namespace prefab {

class prefab_resource : public devils_engine::demiurg::resource_interface {
public:
  prefab_resource();

  std::string_view prefab_name() const noexcept {
    return name_;
  }
  std::string_view text() const noexcept {
    return text_;
  }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  std::string name_; // логическое имя префаба (для spawn): list_name или базовое имя файла
  std::string text_; // сырой tavl-текст префаба (list-секция или весь файл)
};

} // namespace prefab
} // namespace devils_engine

#endif
