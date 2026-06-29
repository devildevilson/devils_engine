#ifndef DEVILS_ENGINE_ACT_EFFECT_SINK_H
#define DEVILS_ENGINE_ACT_EFFECT_SINK_H

#include <span>
#include "devils_engine/utils/string_id.h" // utils::id
#include "value.h"

namespace devils_engine {
namespace act {
// effect_sink — приёмник эффектов: логирование + dry-run.
//   nullptr  ⇒ dry-run  (планировщик GOAP, предиктивный UI) — мутаций нет.
//   реальный ⇒ боевой тик: вызов пишется в канал (INPUT/MUTATOR) для replay/трейса.
// Контракт: эффект мутирует мир ТОЛЬКО через exec_context (world + sink), никогда через
// приватный стейт бэкенда — иначе его нельзя dry-run'ить и логировать (поэтому Lua тут
// гость, а не бэкенд эффектов).
//
// ВРЕМЕННО здесь: по плану автора effect_sink переедет в libs/catalogue (его реальная
// реализация — consumer канала). Пока это простой интерфейс-заглушка, чтобы exec_context
// и эффекты собирались. Конкретный приёмник подключается позже.

struct effect_sink {
  virtual ~effect_sink() noexcept = default;
  // эффект объявляет себя сюда; sink решает применить/залогировать. args — уже
  // вычисленные value (provenance кладёт вызывающий слой — напр. intent.source_action).
  virtual void emit(utils::id effect_id, std::span<const value> args) = 0;
};

}
}

#endif
