#include "resource_loader.h"

#include <algorithm>

#include "catalogue_domain.h"
#include "devils_engine/catalogue/logging.h"
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
  install_catalogue_introspection();
  using request_t = catalogue_domain::fn_traits<&resource_loader::request_impl, "resource_loader.request", "self", "res", "target">;
  request_t::loc_fn_t{}(*this, res, target);
}

void resource_loader::request_impl(resource_interface* res, int32_t target) {
  if (res == nullptr) return;

  // target не может превышать эффективный потолок ресурса (учёт warm_and_hot_same / top_state)
  target = std::min(std::max(target, 0), res->final_state());

  // Диагностика ЦИКЛА: res уже на текущем пути DFS зависимостей → граф не DAG. (Уже заведённая
  // запись в entries — это НЕ цикл, а независимый более ранний запрос; она ловится find() ниже.)
  for (auto* v : visiting_) {
    if (v == res) {
      utils::warn("demiurg: обнаружен ЦИКЛ зависимостей на ресурсе '{}' — граф зависимостей должен быть DAG; ветка пропущена", res->id);
      return;
    }
  }

  if (auto* e = find(res)) {
    e->target = target; // последний запрос побеждает; зависимости уже заведены при первом request
    DE_LOG(catalogue::log_domain::demiurg, flow, "resource_loader: retarget '{}' -> {}", res->id, target);
    return;
  }

  // Уже в нужном состоянии и ничего не делаем — нет смысла заводить запись.
  // (его зависимости к этому моменту тоже usable — иначе он бы сюда не добрался.)
  if (res->state() == target) return;

  entries.push_back(entry{res, target, false});
  DE_LOG(catalogue::log_domain::demiurg, flow, "resource_loader: request '{}' level {} -> {}", res->id, res->state(), target);

  // Обеспечим зависимости: каждую доводим до usable (её final_state). visiting_ помечает текущий путь
  // DFS (для детекции цикла выше); pop после обхода. Рекурсия также завершается на уже заведённых
  // записях (find выше) — предполагается DAG.
  visiting_.push_back(res);
  for (auto* dep : res->dependencies) request(dep, dep->final_state());
  visiting_.pop_back();
}

size_t resource_loader::update(std::vector<external_job>& out) {
  install_catalogue_introspection();
  using update_t = catalogue_domain::fn_traits<&resource_loader::update_impl, "resource_loader.update", "self", "out">;
  return update_t::loc_fn_t{}(*this, out);
}

size_t resource_loader::update_impl(std::vector<external_job>& out) {
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
        DE_LOG(catalogue::log_domain::demiurg, flow, "resource_loader: external load '{}' level {}->{}", e.res->id, cur, cur + 1);
      } else {
        e.res->load(handle); // локально (диск/CPU)
      }
    } else {
      // движемся вниз: переход cur -> cur-1 (обратный к (cur-1)->cur)
      if (e.res->is_external_step(cur - 1)) {
        out.push_back(external_job{e.res, false});
        e.in_flight = true;
        DE_LOG(catalogue::log_domain::demiurg, flow, "resource_loader: external unload '{}' level {}->{}", e.res->id, cur, cur - 1);
      } else {
        e.res->unload(handle); // локально
      }
    }

    ++i;
  }

  return still_pending;
}

void resource_loader::external_done(resource_interface* res) {
  install_catalogue_introspection();
  using done_t = catalogue_domain::fn_traits<&resource_loader::external_done_impl, "resource_loader.external_done", "self", "res">;
  done_t::loc_fn_t{}(*this, res);
}

void resource_loader::external_done_impl(resource_interface* res) {
  if (auto* e = find(res)) {
    e->in_flight = false;
    DE_LOG(catalogue::log_domain::demiurg, flow, "resource_loader: external done '{}' level {}", res != nullptr ? res->id : std::string_view{}, res != nullptr ? res->state() : 0);
  }
}

size_t resource_loader::pending_count() const noexcept { return entries.size(); }

void resource_loader::clear() noexcept { entries.clear(); }

}
}
