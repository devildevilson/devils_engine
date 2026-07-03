#include "resource_loader.h"

#include <algorithm>

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

void resource_loader::request(resource_interface* res, int32_t target) {
  if (res == nullptr) return;

  // target не может превышать эффективный потолок ресурса (учёт warm_and_hot_same / top_state)
  target = std::min(std::max(target, 0), res->final_state());

  if (auto* e = find(res)) {
    e->target = target; // последний запрос побеждает; зависимости уже заведены при первом request
    return;
  }

  // Уже в нужном состоянии и ничего не делаем — нет смысла заводить запись.
  // (его зависимости к этому моменту тоже usable — иначе он бы сюда не добрался.)
  if (res->state() == target) return;

  entries.push_back(entry{res, target, false});

  // Обеспечим зависимости: каждую доводим до usable (её final_state). Рекурсия завершается на
  // уже заведённых записях (find выше) — предполагается DAG. Делаем ПОСЛЕ push, чтобы цикл
  // A→B→A терминировал на повторном request(A).
  for (auto* dep : res->dependencies) request(dep, dep->final_state());
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
      // движемся вверх: зависимости должны быть usable ПРЕЖДЕ продвижения (over-approx: гейтим
      // ресурс целиком, а не конкретный шаг). Зависимости крутятся в этом же loader'е и дойдут сами.
      bool deps_ready = true;
      for (auto* dep : e.res->dependencies) { if (!dep->usable()) { deps_ready = false; break; } }
      if (!deps_ready) { ++i; continue; } // ждём зависимости — ресурс остаётся pending

      // движемся вверх: переход cur -> cur+1
      if (e.res->is_external_step(cur)) {
        // внешний (GPU/поток рендера) — отдаём наружу
        out.push_back(external_job{e.res, true});
        e.in_flight = true;
      } else {
        e.res->load(handle); // локально (диск/CPU)
      }
    } else {
      // движемся вниз: переход cur -> cur-1 (обратный к (cur-1)->cur)
      if (e.res->is_external_step(cur - 1)) {
        out.push_back(external_job{e.res, false});
        e.in_flight = true;
      } else {
        e.res->unload(handle); // локально
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
