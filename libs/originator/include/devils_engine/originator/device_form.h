#ifndef DEVILS_ENGINE_ORIGINATOR_DEVICE_FORM_H
#define DEVILS_ENGINE_ORIGINATOR_DEVICE_FORM_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "tools.h"

// СБОРКА УСТРОЙСТВЕННОЙ ФОРМЫ: привязки ГЕНЕРИРУЮТСЯ, а не пишутся в тексте инструмента.
//
// Это прямое следствие §6.2 `ORIGINATOR_GPGPU.md`: «род ВЫВОДИТСЯ, форма ОБЪЯВЛЯЕТСЯ». Если род
// ресурса (буфер или картинка) выводится из того, КАК его читают во всей очереди, то инструмент не
// может знать его заранее — а значит и объявить `layout(std430 ...) buffer` в своём тексте не может.
// Пока текст писали целиком, род был спрятан в нём, и «выводится» было бы неправдой.
//
// Поэтому инструмент объявляет ТЕЛО, написанное против аксессоров, а преамбулу собирает эта функция,
// уже зная выведенные роды:
//
//   in_<i>_at(uint at)        — элемент входа (у картинки это texelFetch по свёрнутой координате);
//   in_<i>_length()           — сколько элементов во входе;
//   in_<i>_sample(vec2 uv)    — ТОЛЬКО у входа, объявленного фильтруемым: чтение МЕЖДУ элементами;
//   out_<j>_set(uint, value)  — запись своего элемента;
//   out_<j>_add(uint, value)  — атомарное накопление, только у целого буфера (см. `order_free_writes`).
//
// Второе, что этим закрывается, — ТИП. Прежние тексты объявляли `float data[]` у любого поля, а в
// очередь пускаются роды `v`, `ui` и `i`: поле `ui1`, прочитанное как `float[]`, дало бы БИТЫ,
// перевёрнутые в float, — ровно ту ошибку, которую `as_span<T>` ловит на CPU. Аксессор типизирован по
// объявленному роду поля, поэтому такого чтения больше не существует.

namespace devils_engine {
namespace originator {

// КАК вызов читает свой вход на устройстве.
//
// `filtered` — это и есть критерий §6.3: читают МЕЖДУ элементами. Ничем другим картинка от буфера не
// отличается настолько, чтобы менять род ресурса, поэтому объявляется ровно это, а «сделай картинкой»
// не объявляется вовсе.
struct device_access {
  enum values {
    plain,    // ровно свой элемент по индексу
    filtered, // выборка по нецелой координате: аппаратный фильтр между элементами
    count
  };
};

// ГДЕ поле лежит на устройстве. Величина ВЫВЕДЕННАЯ: её считает устройственный план по тому, как
// поле читают все вызовы очереди вместе, а не объявляет ни инструмент, ни автор конфига.
struct device_residence {
  enum values {
    in_buffer,
    in_image,
    count
  };
};

struct device_binding {
  field_base::values base = field_base::v;
  device_residence::values residence = device_residence::in_buffer;
  device_access::values access = device_access::plain;
  // Выход вызова. Порядок привязок — сначала все входы, затем все выходы, тот же, что у `queue_call`.
  bool writable = false;
  // Нужен ли атомарный накопитель. Ставится только у вызова, объявившего свои записи независимыми от
  // порядка, и только у целого буфера: у плавающего сложения порядок значим (§4.3).
  bool accumulates = false;
};

// Написание рода в GLSL: `v` -> float, `ui` -> uint, `i` -> int. Роды уже `2`/`ub`/`c` на устройство
// не переносятся вовсе и сюда не приходят.
std::string_view device_type_name(const field_base::values base) noexcept;

// Собирает полный текст шейдера: шапка push-константы, привязки с аксессорами, свёртка индекса и
// тело. ОДИН способ выложить байты и один способ обратиться к полю на все инструменты и все переводы.
std::string build_device_shader(const std::span<const device_binding>& bindings,
                                const std::span<const device_param>& params,
                                const std::string_view& body,
                                const uint32_t group_size = 64);

} // namespace originator
} // namespace devils_engine

#endif
