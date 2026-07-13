#ifndef DEVILS_ENGINE_SIMUL_LUA_APP_BINDINGS_H
#define DEVILS_ENGINE_SIMUL_LUA_APP_BINDINGS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include <devils_engine/bindings/lua_header.h>
#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/visage/image.h>
#include <devils_engine/visage/system.h>

#include "lifecycle.h"
#include "messages.h" // resource_ref, command_sound_*, generate_task_id
#include "pause.h"

// Движковые lua-биндинги приложения, общие для любого проекта: звуковой плеер (движковая подсистема,
// каналы в standard_broker), мост картинки demiurg→visage и lifecycle/pause/logging. Регистрируются
// хостом в общей таблице `app` UI-песочницы ДО проектных биндингов. Формы — свободные template-хелперы,
// duck-typed по типу состояния/хоста (как window_runtime/lua_resource_bindings): состояние даёт ui/br/
// sound_state/ui_resource_scope, хост — state()/request_runtime_state(). См. docs/simul-extraction-design.md
// (шаг 3): «список функций» проекта = act-реестр, а эти движковые API — часть инструментария движка.

namespace devils_engine {
namespace simul {

// Lua-ОБЁРТКА id звуковой задачи. usertype без арифметических метаметодов ⇒ скрипт не считает id и не
// подсовывает случайное число в поле after (секвенсинг). В контракте сообщений ходит голый size_t.
struct sound_handle {
  size_t value = SIZE_MAX;
  bool valid() const noexcept {
    return value != SIZE_MAX;
  }
};

// Main-локальная запись таблицы состояния звука для UI: wire-запись (taskid/progress) + deadline.
// 0 — ПОДТВЕРЖДЕНА последней публикацией звука; >0 — ОПТИМИСТИЧНАЯ (play только что запрошен, ещё не
// доехал в публикацию), живёт до этого main-кадра. Срок отличает «ещё не стартовал» (вернём 0) от
// «уже закончился» (nil), убирая отдельный pending-контейнер.
struct ui_sound_state_entry {
  size_t taskid;
  double progress;
  size_t deadline;
};

// Звуковой плеер: app.play_sound / stop_sound / sound_state / set_sound_device. Каждый — обычное
// сообщение на звуковой тред (presentation→sound, в лог реплея НЕ попадает). Очередь/кроссфейд плеер
// собирает на lua, опрашивая state. sound_enabled=false ⇒ вызовы безопасно no-op.
template <typename State>
void install_sound_lua_bindings(sol::table app, State& c, const bool sound_enabled) {
  auto* cptr = &c;
  auto& L = c.ui->script_state();
  L.template new_usertype<sound_handle>("sound_handle",
                                        sol::no_constructor,
                                        "valid", &sound_handle::valid);

  // Первый аргумент — sol::object (resource_handle ИЛИ таблица-опции с полем resource/res), второй —
  // необязательная таблица-опции. sol::object у НЕпоследнего параметра безопаснее sol::optional.
  app.set_function("play_sound",
                   [cptr, sound_enabled](sol::object a, sol::optional<sol::table> b) -> sound_handle {
                     auto& c = *cptr;
                     if (!sound_enabled) {
                       return sound_handle{};
                     }

                     demiurg::resource_handle sound_res;
                     sol::optional<sol::table> opts;
                     if (a.is<sol::table>()) {
                       opts = a.as<sol::table>();
                       const sol::optional<demiurg::resource_handle> resource = (*opts)["resource"];
                       const sol::optional<demiurg::resource_handle> res = (*opts)["res"];
                       if (resource) {
                         sound_res = *resource;
                       } else if (res) {
                         sound_res = *res;
                       }
                     } else if (a.is<demiurg::resource_handle>()) {
                       sound_res = a.as<demiurg::resource_handle>();
                       opts = b;
                     }
                     if (sound_res.get() == nullptr) {
                       return sound_handle{};
                     }

                     command_sound_play play{};
                     play.taskid = generate_task_id();
                     play.after = SIZE_MAX;
                     play.start = 0.0;
                     if (opts) {
                       play.start = std::clamp(opts->get_or("start", 0.0), 0.0, 1.0);
                       const sol::optional<sound_handle> after = (*opts)["after"];
                       if (after && after->valid()) {
                         play.after = after->value; // хэндл → секвенсинг
                       }
                     }
                     play.res = resource_ref::from_handle(sound_res);
                     c.br->sound_play.try_push(play);
                     // оптимистичная запись: пока play не доедет в публикацию (latency 1-2 кадра),
                     // app.sound_state по ней вернёт 0, а не nil (deadline = окно старта).
                     constexpr size_t startup_grace_frames = 30;
                     c.sound_state.push_back({play.taskid, 0.0, c.sound_frame + startup_grace_frames});
                     return sound_handle{play.taskid};
                   });

  app.set_function("stop_sound", [cptr, sound_enabled](const sound_handle& h) {
    auto& c = *cptr;
    if (!sound_enabled || !h.valid()) {
      return;
    }
    command_sound_stop stop{};
    stop.taskid = h.value;
    c.br->sound_stop.try_push(stop);
  });

  // ищет id в единой таблице sound_state. Возвращает прогресс [0,1] или nil. Раз вернули число — звук в
  // обработке (играет/в очереди/только что запрошен); nil — задачи уже нет. Оптимистичная запись с
  // истёкшим окном старта (так и не доехала) трактуется как nil.
  app.set_function("sound_state", [cptr](const sound_handle& h) -> sol::object {
    auto& c = *cptr;
    auto& lua = c.ui->script_state();
    if (!h.valid()) {
      return sol::nil;
    }
    for (const auto& s : c.sound_state) {
      if (s.taskid != h.value) {
        continue;
      }
      if (s.deadline != 0 && c.sound_frame > s.deadline) {
        return sol::nil; // окно вышло
      }
      return sol::make_object(lua, s.progress);
    }
    return sol::nil;
  });

  // Смена звукового устройства: пере-создаём system2 через канал recreate.
  app.set_function("set_sound_device", [cptr, sound_enabled](const std::string& name) {
    auto& c = *cptr;
    if (!sound_enabled || c.br == nullptr) {
      return;
    }
    c.br->recreate_sound.try_push(command_recreate_sound_system{name});
  });
}

// Слияние оптимистичного sound_state с публикацией звукового треда (latest-wins мейлбокс). Зовётся раз
// за main-кадр (перед проходом UI): накручивает sound_frame и оставляет только доехавшие/ещё-в-окне записи.
template <typename State>
void advance_sound_state(State& c) {
  c.sound_frame += 1;
  const command_sound_state* msg = c.br ? c.br->sound_state.consume() : nullptr;
  if (msg == nullptr) {
    return;
  }
  auto& cur = c.sound_state;
  auto& next = c.sound_state_next;
  next.clear();
  for (const auto& s : msg->sounds) {
    next.push_back({s.taskid, s.progress, 0});
  }
  for (const auto& e : cur) {
    if (e.deadline == 0) {
      continue;
    }
    if (e.deadline < c.sound_frame) {
      continue;
    }
    bool in_pub = false;
    for (const auto& s : msg->sounds) {
      if (s.taskid == e.taskid) {
        in_pub = true;
        break;
      }
    }
    if (!in_pub) {
      next.push_back(e);
    }
  }
  std::swap(cur, next);
}

// Мост картинки demiurg→visage: app.image(resource_handle [, {region={x,y,w,h}}]) -> visage::image | nil.
// Строит хендл из gpu_index+размера когда текстура usable() (на GPU), иначе nil. Только ресурсы из
// активного UI-scope.
template <typename State>
void install_image_lua_binding(sol::table app, State& c) {
  auto* cptr = &c;
  app.set_function("image", [cptr](sol::object resource, sol::optional<sol::table> opts) -> sol::object {
    auto& c = *cptr;
    auto& lua = c.ui->script_state();
    if (!resource.is<demiurg::resource_handle>()) {
      return sol::nil;
    }
    const auto handle = resource.as<demiurg::resource_handle>();
    if (!c.ui_resource_scope->contains(handle)) {
      return sol::nil;
    }
    auto* tex = handle.template get<painter::gpu_texture_resource>();
    if (tex == nullptr || !tex->usable()) {
      return sol::nil; // ещё не на GPU
    }
    visage::image img{};
    img.texture_id = tex->gpu_index;
    img.w = uint16_t(tex->width);
    img.h = uint16_t(tex->height);
    if (opts) {
      const sol::optional<sol::table> region = (*opts)["region"];
      if (region) {
        for (int i = 0; i < 4; ++i) {
          img.region[i] = uint16_t(region->get_or(i + 1, 0));
        }
      }
    }
    return sol::make_object(lua, img);
  });
}

// Lifecycle/pause/logging движка для UI: app.state()/runtime_state()/request_state(id)/set_paused(b)/
// paused()/set_log_level(domain,depth). Хост даёт state() (lifecycle/current_runtime_state/pause) и
// request_runtime_state(). Прогресс загрузки и проектные метрики регистрирует проект отдельно.
template <typename Host>
void install_app_lifecycle_bindings(sol::table app, Host& host) {
  auto* hptr = &host;
  app.set_function("state", [hptr]() -> std::string {
    return std::string(to_string(hptr->state().lifecycle.phase()));
  });
  app.set_function("runtime_state", [hptr]() -> std::string {
    auto* state = hptr->state().current_runtime_state.get();
    return state != nullptr ? std::string(state->id) : std::string{};
  });
  app.set_function("request_state", [hptr](const std::string& id) -> bool {
    return hptr->request_runtime_state(id);
  });
  app.set_function("loading_progress", [hptr]() -> double {
    return hptr->loading_progress();
  });
  app.set_function("set_paused", [hptr](const bool value) {
    hptr->state().pause.set_world(value);
  });
  app.set_function("paused", [hptr]() {
    return hptr->state().pause.paused(pause_domain::gameplay);
  });
  // рантайм-переключение глубины логгирования домена (работает и в release):
  // app.set_log_level("sound","trace"). Домены main/assets/sound/render/ui/gameplay/resource/demiurg;
  // глубина off/info/flow/trace.
  app.set_function("set_log_level", [](const std::string& domain, const std::string& depth) -> bool {
    catalogue::log_depth d = catalogue::log_depth::off;
    if (!catalogue::parse_log_depth(depth, d)) {
      utils::warn("set_log_level: bad depth '{}'", depth);
      return false;
    }
    if (!catalogue::logs().set_level(domain, d)) {
      utils::warn("set_log_level: unknown domain '{}'", domain);
      return false;
    }
    utils::info("log domain '{}' -> {}", domain, depth);
    return true;
  });
}

} // namespace simul
} // namespace devils_engine

#endif
