#ifndef DEVILS_ENGINE_ACT_VALUE_H
#define DEVILS_ENGINE_ACT_VALUE_H

#include <cstdint>
#include "devils_engine/utils/string_id.h" // utils::id
#include "common.h"

namespace devils_engine {
namespace act {
// value — слим POD tagged union. НЕ тип возврата геймплейной функции (возврат
// типизирован классом функции, см. function.h). Нужен только на generic-границах:
// аргументы эффекта в effect_sink::emit (лог в catalogue) и маршалинг args в скрипт.
// Набор типов узкий и фиксированный — дёшево кладётся на диск / уходит по сети.
// Категории совпадают с devils_script::user_function_type. string = ХЕШ (не инлайн).

enum class value_kind : uint8_t {
  none, boolean, integer, number, vector, handle, string
};

struct value {
  value_kind kind;
  union {
    bool      bln;
    int64_t   inum;
    real_t    num;
    vec3      vec;
    uint64_t  hnd; // entity_id / контекстный индекс
    utils::id str; // string_hash(...)
  };

  // vec3 имеет нетривиальный дефолт-ctor (member-инициализаторы) ⇒ дефолтный ctor union
  // удалён; задаём явный. Инициализируем и union (inum=0) — никаких неинициализированных
  // байт (важно для детерминированных чексумм: считать поля, не memcmp с паддингом).
  value() noexcept : kind(value_kind::none), inum(0) {}

  static value none_v()          noexcept { value r; r.kind = value_kind::none;    return r; }
  static value of(bool x)        noexcept { value r; r.kind = value_kind::boolean; r.bln  = x;    return r; }
  static value of(int64_t x)     noexcept { value r; r.kind = value_kind::integer; r.inum = x;    return r; }
  static value of(real_t x)      noexcept { value r; r.kind = value_kind::number;  r.num  = x;    return r; }
  static value of(vec3 x)        noexcept { value r; r.kind = value_kind::vector;  r.vec  = x;    return r; }
  static value of(entity_id e)   noexcept { value r; r.kind = value_kind::handle;  r.hnd  = e.id; return r; }
  static value strv(utils::id x) noexcept { value r; r.kind = value_kind::string;  r.str  = x;    return r; }
};

}
}

#endif
