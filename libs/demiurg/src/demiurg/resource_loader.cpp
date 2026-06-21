#include "resource_loader.h"

#include "devils_engine/utils/safe_handle.h"

namespace devils_engine {
namespace demiurg {

resource_loader::resource_loader() noexcept = default;
resource_loader::~resource_loader() noexcept = default;

resource_loader::entry* resource_loader::find(resource_interface* res) noexcept {
  for (auto& e : entries) {
    if (e.res == res) return &e;
  }
  return nullptr;
}

void resource_loader::request(resource_interface* res, state::values target) {
  if (res == nullptr) return;

  if (auto* e = find(res)) {
    e->target = static_cast<int32_t>(target); // последний запрос побеждает
    return;
  }

  // Уже в нужном состоянии и ничего не делаем — нет смысла заводить запись.
  if (res->state() == target) return;

  entries.push_back(entry{res, static_cast<int32_t>(target), false});
}

size_t resource_loader::update(std::vector<external_job>& out) {
  size_t still_pending = 0;

  for (size_t i = 0; i < entries.size();) {
    auto& e = entries[i];
    const auto cur = static_cast<int32_t>(e.res->state());

    if (cur == e.target) { // достигли цели — запись больше не нужна
      entries[i] = entries.back();
      entries.pop_back();
      continue;
    }

    still_pending += 1;

    if (e.in_flight) { ++i; continue; } // ждём, пока рендер завершит внешний переход

    const utils::safe_handle_t handle(e.res);

    if (cur < e.target) {
      // движемся вверх по состояниям
      if (cur == static_cast<int32_t>(state::cold)) {
        e.res->load(handle); // cold->warm, локально (диск/CPU)
      } else {
        // warm->hot: GPU-сторона, отдаём наружу
        out.push_back(external_job{e.res, true});
        e.in_flight = true;
      }
    } else {
      // движемся вниз по состояниям
      if (cur == static_cast<int32_t>(state::hot)) {
        // hot->warm: GPU-сторона, отдаём наружу
        out.push_back(external_job{e.res, false});
        e.in_flight = true;
      } else {
        e.res->unload(handle); // warm->cold, локально
      }
    }

    ++i;
  }

  return still_pending;
}

void resource_loader::external_done(resource_interface* res) {
  if (auto* e = find(res)) e->in_flight = false;
}

size_t resource_loader::pending_count() const noexcept { return entries.size(); }

void resource_loader::clear() noexcept { entries.clear(); }

}
}
