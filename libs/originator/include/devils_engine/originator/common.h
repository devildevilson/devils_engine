#ifndef DEVILS_ENGINE_ORIGINATOR_COMMON_H
#define DEVILS_ENGINE_ORIGINATOR_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

// БАЗОВЫЕ ТИПЫ: род поля буфера, апертура и носитель ключа — словарь, на котором говорят все
// остальные заголовки библиотеки.
//
// Написание типа намеренно повторяет painter (`<род><число компонент>`): автору конфига не нужно
// учить второй словарь форматов. Отличие — ни одного Vulkan-специфичного типа, зато есть сырые
// 8/16-битные целые: генератору они нужны для компактных идентификаторов и меток.
//
// `d` (f64) — для мировых координат грубых проходов: макрограф и залы считают в double, и float32 в
// буфере округлил бы их на миллиметры за десятки километров. На устройство `d` не едет (Float64 в
// Vulkan не гарантирован): для перевода и тел это род без устройственного типа, то есть отказ.
//
// `ul`/`l` (u64/i64) — для ключей, хешей и зёрен: слово ключа цепочки — одна компонента `ul`, а не
// пара `ui`. Как и `d`, на устройство они не едут: роскошь CPU, которой GPU-путь не обязан повторять.
//
// КАНОНИЧЕСКИЙ ТИП ЧТЕНИЯ И ЗАПИСИ ОДНОЙ КОМПОНЕНТЫ — `double`. Он точно представляет всё 32-битное,
// что здесь хранится, и совпадает с числовым типом lua. 64-битные целые через double теряют младшие
// биты выше 2^53, поэтому у них есть второй, ЦЕЛЫЙ путь (`load_integer`/`store_integer`): им ходят
// ключи, им пользуется lua (целое lua — 64 бита), а `ds`, читающий поле как double, видит округлённое
// значение — это его граница, не поля. Быстрый путь `as_span` требует точного совпадения рода
// (`exact_storage_base`).

namespace devils_engine {
namespace originator {

// X(написание, байт на компоненту, род)
#define DEVILS_ENGINE_ORIGINATOR_FIELD_BASE_LIST \
  X(v, 4, floating)                              \
  X(sf, 2, floating)                             \
  X(ui, 4, unsigned_integer)                     \
  X(us, 2, unsigned_integer)                     \
  X(ub, 1, unsigned_integer)                     \
  X(i, 4, signed_integer)                        \
  X(is, 2, signed_integer)                       \
  X(ib, 1, signed_integer)                       \
  X(c, 1, normalized)                            \
  X(d, 8, floating)                              \
  X(ul, 8, unsigned_integer)                     \
  X(l, 8, signed_integer)

struct field_kind {
  enum values {
    floating,         // v (f32), sf (f16), d (f64)
    unsigned_integer, // ul (u64), ui (u32), us (u16), ub (u8)
    signed_integer,   // l (i64), i (i32), is (i16), ib (i8)
    normalized,       // c (u8, значение 0..1)
    count
  };
};

struct field_base {
  enum values {
#define X(name, bytes, kind) name,
    DEVILS_ENGINE_ORIGINATOR_FIELD_BASE_LIST
#undef X
      count
  };
};

constexpr uint32_t max_field_components = 4;

// Тип одного поля: род хранения плюс число компонент (1..4).
struct field_type {
  field_base::values base = field_base::count;
  uint32_t components = 0;

  bool valid() const noexcept;
  size_t component_byte_size() const noexcept;
  size_t byte_size() const noexcept;
  size_t alignment() const noexcept;
  field_kind::values kind() const noexcept;

  bool operator==(const field_type& other) const noexcept = default;
};

// Точное соответствие C++-типа роду хранения для быстрого пути. Роды без точного C++-типа (`sf` —
// половинная точность, `c` — нормализованный байт) его не получают вовсе: к ним обращаются только
// через аксессор, который делает преобразование.
template <typename type_t>
constexpr field_base::values exact_storage_base() noexcept {
  if constexpr (std::is_same_v<type_t, float>) {
    return field_base::v;
  } else if constexpr (std::is_same_v<type_t, double>) {
    return field_base::d;
  } else if constexpr (std::is_same_v<type_t, uint32_t>) {
    return field_base::ui;
  } else if constexpr (std::is_same_v<type_t, uint16_t>) {
    return field_base::us;
  } else if constexpr (std::is_same_v<type_t, uint8_t>) {
    return field_base::ub;
  } else if constexpr (std::is_same_v<type_t, int32_t>) {
    return field_base::i;
  } else if constexpr (std::is_same_v<type_t, int16_t>) {
    return field_base::is;
  } else if constexpr (std::is_same_v<type_t, int8_t>) {
    return field_base::ib;
  } else if constexpr (std::is_same_v<type_t, uint64_t>) {
    return field_base::ul;
  } else if constexpr (std::is_same_v<type_t, int64_t>) {
    return field_base::l;
  } else {
    return field_base::count; // нет точного соответствия => быстрый путь недоступен
  }
}

// Разбирает написание вида "v3", "ui1", "c4". Возвращает невалидный тип, если строка не разобрана;
// вызывающий решает, ошибка это конфига (падать) или ожидаемый отказ.
field_type parse_field_type(const std::string_view& str) noexcept;
std::string_view to_string(const field_base::values base) noexcept;
std::string_view to_string(const field_kind::values kind) noexcept;

// Чтение/запись одной компоненты в канонический double. Указатель — начало КОМПОНЕНТЫ, а не поля.
double load_component(const void* ptr, const field_base::values base) noexcept;
void store_component(void* ptr, const field_base::values base, const double value) noexcept;
// Целый путь: значение компоненты как int64 (у `ul` — его биты, u64 ↔ i64 без потерь; у плавающих —
// усечение к нулю). Запись в узкий целый род зажимает так же, как запись через double.
int64_t load_integer(const void* ptr, const field_base::values base) noexcept;
void store_integer(void* ptr, const field_base::values base, const int64_t value) noexcept;
bool is_wide_integer(const field_base::values base) noexcept;

// Апертура: как инструмент адресуется относительно обрабатываемого элемента. Из неё выводится
// параллельность — инструмент её ОБЪЯВЛЯЕТ, скрипт её не выбирает.
struct aperture {
  enum values {
    pointwise,  // только свой элемент; источник и приёмник могут совпадать
    gather,     // читает произвольные элементы, пишет свой; источник ДОЛЖЕН отличаться от приёмника
    scatter,    // пишет по чужим индексам; доступен только готовым инструментам (group_by/accumulate)
    reduce,     // много -> одно, фиксированное разбиение на чанки
    sequential, // порядок значим, один поток
    count
  };
};

// Носитель ключа у scatter: докуда простирается группа, в которую инструмент разбрасывает элементы.
// В отличие от апертуры это ОБЪЯВЛЕНИЕ автора пайплайна, а не вывод: что означает ключ — «блок внутри
// чанка» или «область на всей карте» — из одного чанка не проверяется никак. Поэтому по умолчанию
// стоит безопасное: необъявленный ключ считается глобальным, и при чанкованной генерации scatter
// отклоняется. Молчание означает отказ, а не разрешение.
struct key_support {
  enum values {
    chunk_local, // группа целиком лежит внутри одного чанка: чанк заканчивает её сам
    global,      // группа собирает элементы со всей карты: ни один чанк её не заканчивает
    count
  };
};

std::string_view to_string(const key_support::values value) noexcept;
key_support::values parse_key_support(const std::string_view& str) noexcept;

std::string_view to_string(const aperture::values value) noexcept;

// Параллелится ли апертура сама по себе. Для gather этого НЕ достаточно: нужна ещё проверка
// непересечения источника и приёмника, см. tools.h.
bool is_parallel(const aperture::values value) noexcept;

} // namespace originator
} // namespace devils_engine

#endif
