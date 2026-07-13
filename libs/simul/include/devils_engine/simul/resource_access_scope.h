#ifndef DEVILS_ENGINE_SIMUL_RESOURCE_ACCESS_SCOPE_H
#define DEVILS_ENGINE_SIMUL_RESOURCE_ACCESS_SCOPE_H

#include <algorithm>
#include <memory>
#include <vector>

#include <devils_engine/demiurg/resource_system.h>

// Allowlist видимых ресурсов: скрипт/загрузчик может получать handles, читать usable()/state() и
// перечислять только разрешённые ресурсы, но не запускать load/unload. Чистая структура над
// demiurg::resource_handle — БЕЗ зависимости от lua (её используют и loading-контракт, и lua-биндинги).

namespace devils_engine {
namespace simul {

class resource_access_scope {
public:
  void grant(const demiurg::resource_handle handle) {
    if (handle.get() == nullptr || contains(handle)) {
      return;
    }
    handles_.push_back(handle);
  }

  void clear() {
    handles_.clear();
  }

  bool contains(const demiurg::resource_handle handle) const noexcept {
    return std::find_if(handles_.begin(), handles_.end(), [handle](const demiurg::resource_handle cur) {
             return cur.system == handle.system && cur.hash == handle.hash;
           }) != handles_.end();
  }

private:
  std::vector<demiurg::resource_handle> handles_;
};

bool resource_is_visible(
  const std::shared_ptr<const resource_access_scope>& scope,
  demiurg::resource_handle handle) noexcept;

} // namespace simul
} // namespace devils_engine

#endif
