#include "devils_engine/demiurg/synchronous_loader.h"

#include <algorithm>

#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace demiurg {

void synchronous_loader::load(resource_interface* res, const int32_t target) {
  if (res == nullptr) {
    utils::error{}("demiurg: synchronous loader was asked to load a null resource");
  }
  loader_.request(res, target);
  settle(res, std::min(std::max(target, 0), res->final_state()));
}

void synchronous_loader::load(resource_interface* res) {
  if (res == nullptr) {
    utils::error{}("demiurg: synchronous loader was asked to load a null resource");
  }
  load(res, res->final_state());
}

size_t synchronous_loader::load_directory(const resource_system& resources, const std::string_view prefix) {
  std::vector<resource_interface*> found;
  resources.directory(prefix, found);
  for (auto* res : found) {
    load(res);
  }
  return found.size();
}

void synchronous_loader::settle(resource_interface* res, const int32_t target) {
  // Каждый update делает не больше одного перехода на ресурс; сведение без внешних шагов
  // заканчивается за число переходов всех затронутых ресурсов. Предел ловит зависание (ресурс,
  // чей переход не меняет состояния), а не честно долгую загрузку.
  constexpr size_t step_limit = 1'000'000;
  for (size_t step = 0; step < step_limit; ++step) {
    jobs_.clear();
    const size_t pending = loader_.update(jobs_);
    if (!jobs_.empty()) {
      utils::error{}("demiurg: synchronous loader cannot run the external (GPU) step of '{}' "
                     "(requested '{}' up to level {}); a context without an assets thread loads CPU-only resources",
                     jobs_.front().res->id, res->id, target);
    }
    if (pending == 0) {
      if (res->state() < target) {
        utils::error{}("demiurg: synchronous loader finished but '{}' is at level {}, expected {}",
                       res->id, res->state(), target);
      }
      return;
    }
  }
  utils::error{}("demiurg: synchronous loader made no progress on '{}' within {} steps", res->id, step_limit);
}

} // namespace demiurg
} // namespace devils_engine
