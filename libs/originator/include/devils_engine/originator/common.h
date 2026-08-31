#ifndef DEVILS_ENGINE_ORIGINATOR_COMMON_H
#define DEVILS_ENGINE_ORIGINATOR_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string_view>

// Базовые типы originator: описание типа поля буфера и словарь режимов.
//
// Написание типа намеренно повторяет painter (<род><число компонент>): автору конфига не нужно
// учить второй словарь форматов. Отличие в том, что здесь нет ни одного Vulkan-специфичного типа,
// зато есть сырые 8/16-битные целые, которых у painter нет, — генератору они нужны для компактных
// идентификаторов и меток.
//
// Канонический тип чтения/записи одного компонента — double. Он точно представляет всё, что здесь
// хранится (включая uint32), и совпадает с числовым типом lua, поэтому граница со скриптом не
// теряет значений.

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
  X(c, 1, normalized)

struct field_kind {
  enum values {
    floating,         // v (f32), sf (f16)
    unsigned_integer, // ui (u32), us (u16), ub (u8)
    signed_integer,   // i (i32), is (i16), ib (i8)
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

// Разбирает написание вида "v3", "ui1", "c4". Возвращает невалидный тип, если строка не разобрана;
// вызывающий решает, ошибка это конфига (падать) или ожидаемый отказ.
field_type parse_field_type(const std::string_view& str) noexcept;
std::string_view to_string(const field_base::values base) noexcept;
std::string_view to_string(const field_kind::values kind) noexcept;

// Чтение/запись одной компоненты в канонический double. Указатель — начало КОМПОНЕНТЫ, а не поля.
double load_component(const void* ptr, const field_base::values base) noexcept;
void store_component(void* ptr, const field_base::values base, const double value) noexcept;

// Как шаг привязан к буферу. Read-вью физически не умеет писать, поэтому нарушение не выражается,
// а не ловится проверкой.
struct binding_mode {
  enum values {
    read,  // произвольный доступ, неизменяем на всё время шага
    write, // читать И писать может только владелец шага; в массовой операции — только свой элемент
    count
  };
};

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

std::string_view to_string(const aperture::values value) noexcept;
std::string_view to_string(const binding_mode::values value) noexcept;

// Параллелится ли апертура сама по себе. Для gather этого НЕ достаточно: нужна ещё проверка
// непересечения источника и приёмника, см. tools.h.
bool is_parallel(const aperture::values value) noexcept;

} // namespace originator
} // namespace devils_engine

#endif
