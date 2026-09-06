#ifndef DEVILS_ENGINE_ORIGINATOR_BUFFER_H
#define DEVILS_ENGINE_ORIGINATOR_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common.h"

// БУФЕР ГЕНЕРАТОРА: плотный массив элементов с ИМЕНОВАННЫМИ полями и явно выбранной раскладкой.
// Здесь же аксессор поля — единственный способ прочитать и записать элемент.
//
// РАСКЛАДКА (aos/soa) — свойство ХРАНЕНИЯ, а не контракта: имя поля разрешается в пару
// (смещение, шаг) один раз, поэтому ни lua, ни devils_script не видят разницы, и раскладку можно
// переключить в конфиге и померить, не тронув ни строки скрипта. SoA означает один непрерывный план
// на ПОЛЕ (компоненты внутри плана чередуются), а не на каждую компоненту: поле — это именованная
// единица, по которой рассуждает автор конфига.
//
// РАЗМЕР И ФОРМА — РАЗНЫЕ ВОПРОСЫ: размер говорит, сколько буфер СТОИТ, форма (`buffer_extent`) —
// чем его АДРЕСУЮТ. Форма объявляется ВМЕСТО размера и читается инструментом У ПРИВЯЗКИ; прежде она
// приезжала параметром в трёх разных написаниях, то есть одна истина дублировалась столько раз,
// сколько у неё читателей.
//
// БЫСТРЫЙ ПУТЬ (`as_span`) ТРЕБУЕТ ТОЧНОГО СОВПАДЕНИЯ РОДА, а не размера: `ui1` и `v1` оба
// четырёхбайтовые, и `span<float>` над сырым uint32 молча писал бы туда биты float. Это же правило
// повторяется на устройстве у типизированных аксессоров шейдера.
//
// Буфер владеет своей памятью и не меняет размер после создания: генератор обязан знать свою
// стоимость по памяти до запуска.

namespace devils_engine {
namespace originator {

struct storage_kind {
  enum values {
    aos, // [h t m][h t m]... — один элемент лежит целиком
    soa, // [h h h...][t t t...] — один план на поле
    count
  };
};

std::string_view to_string(const storage_kind::values value) noexcept;
storage_kind::values parse_storage_kind(const std::string_view& str) noexcept;

struct field_declaration {
  std::string name;
  field_type type;
};

// Где лежит поле внутри буфера: смещение элемента 0 и расстояние до следующего элемента.
struct field_placement {
  size_t offset = 0;
  size_t stride = 0;
};

// Объявленная раскладка буфера: упорядоченный список полей плюс выбор хранения. Порядок значим —
// он определяет расположение в памяти, поэтому в конфиге это список, а не отображение.
// ФОРМА буфера: сколько элементов по осям. Ноль по оси x означает «не объявлена»: такой буфер
// линейный, и адресуют его номером элемента.
struct buffer_extent {
  size_t x = 0;
  size_t y = 0;
  size_t z = 0;

  bool declared() const noexcept;
  // Сколько осей объявлено: 1, 2 или 3. Ноль означает, что формы нет вовсе.
  uint32_t axes() const noexcept;
  // Произведение объявленных осей — оно же число элементов буфера с такой формой.
  size_t count() const noexcept;

  bool operator==(const buffer_extent& other) const noexcept = default;
};

class buffer_layout {
public:
  storage_kind::values storage = storage_kind::aos;
  std::vector<field_declaration> fields;

  bool valid() const noexcept;
  size_t find_field(const std::string_view& name) const noexcept;
  // Шаг одного элемента при aos: сумма полей с выравниванием, округлённая до максимума выравниваний.
  size_t element_byte_size() const noexcept;
  size_t byte_size(const size_t count) const noexcept;

  static constexpr size_t npos = size_t(-1);
};

// Разбирает список пар (имя, написание типа) в раскладку. Пустое имя, дублирующееся имя или
// неразобранный тип — критическая ошибка конфига, поэтому бросает utils::error.
buffer_layout make_buffer_layout(const storage_kind::values storage,
                                 const std::span<const std::pair<std::string_view, std::string_view>>& fields,
                                 const std::string_view& buffer_name);

// Доступ к одному полю. Изменяемая и неизменяемая версии — РАЗНЫЕ типы: у неизменяемой нет set,
// поэтому запись в буфер, привязанный на чтение, не компилируется, а не отлавливается в рантайме.
template <bool is_const>
class basic_field_accessor {
public:
  using byte_pointer = std::conditional_t<is_const, const std::byte*, std::byte*>;

  basic_field_accessor() noexcept = default;
  basic_field_accessor(byte_pointer base, const field_placement placement, const field_type type, const size_t count) noexcept;

  // Неизменяемый вид строится из изменяемого молча; обратно — нет.
  template <bool other_const>
    requires(is_const && !other_const)
  basic_field_accessor(const basic_field_accessor<other_const>& other) noexcept;

  bool valid() const noexcept;
  size_t count() const noexcept;
  field_type type() const noexcept;
  size_t stride() const noexcept;
  byte_pointer data() const noexcept;

  double get(const size_t element, const uint32_t component = 0) const noexcept;

  void set(const size_t element, const double value, const uint32_t component = 0) const noexcept
    requires(!is_const);

  // Поле лежит непрерывно (шаг равен размеру поля) — значит его можно отдать нативному ядру как
  // span и обработать векторно. При aos это ложно для всего, кроме однополевого буфера.
  bool contiguous() const noexcept;

  // Быстрый путь: сырая память поля как span<type_t>. Отдаётся ТОЛЬКО при точном совпадении рода и
  // однокомпонентном поле, иначе span пуст и вызывающий идёт через аксессор. Многокомпонентное поле
  // быстрого пути не имеет намеренно: ему понадобился бы тип-агрегат от вызывающего.
  template <typename type_t>
  std::span<std::conditional_t<is_const, const type_t, type_t>> as_span() const noexcept;

private:
  byte_pointer base_ = nullptr;
  size_t stride_ = 0;
  size_t count_ = 0;
  field_type type_{};
};

using field_accessor = basic_field_accessor<false>;
using const_field_accessor = basic_field_accessor<true>;

class buffer {
public:
  buffer() noexcept = default;
  buffer(std::string name, buffer_layout layout, const size_t count);
  // Буфер с объявленной ФОРМОЙ. Число элементов не передаётся отдельно, потому что оно уже сказано:
  // это произведение осей. Второй способ назвать то же число означал бы, что однажды они разойдутся.
  buffer(std::string name, buffer_layout layout, const buffer_extent extent);

  const std::string& name() const noexcept;
  const buffer_layout& layout() const noexcept;
  size_t count() const noexcept;
  const buffer_extent& extent() const noexcept;
  size_t byte_size() const noexcept;

  size_t find_field(const std::string_view& name) const noexcept;
  const field_placement& placement(const size_t field_index) const noexcept;

  field_accessor field(const size_t field_index) noexcept;
  const_field_accessor field(const size_t field_index) const noexcept;

  // Обнуление всей памяти. Переиспользование буфера под новую работу — решение скрипта, поэтому
  // движок его не делает сам, но даёт дешёвый явный способ.
  void clear() noexcept;

  const std::byte* base_pointer() const noexcept;

private:
  std::string name_;
  buffer_layout layout_;
  buffer_extent extent_{};
  size_t count_ = 0;
  std::vector<field_placement> placement_;
  std::vector<std::byte> memory_;
  std::byte* data_ = nullptr; // выровненное начало внутри memory_
};

// Template implementation

template <bool is_const>
basic_field_accessor<is_const>::basic_field_accessor(byte_pointer base,
                                                     const field_placement placement,
                                                     const field_type type,
                                                     const size_t count) noexcept :
  base_(base == nullptr ? nullptr : base + placement.offset), stride_(placement.stride), count_(count), type_(type) {}

template <bool is_const>
template <bool other_const>
  requires(is_const && !other_const)
basic_field_accessor<is_const>::basic_field_accessor(const basic_field_accessor<other_const>& other) noexcept :
  base_(other.data()), stride_(other.stride()), count_(other.count()), type_(other.type()) {}

template <bool is_const>
bool basic_field_accessor<is_const>::valid() const noexcept {
  return base_ != nullptr && type_.valid();
}

template <bool is_const>
size_t basic_field_accessor<is_const>::count() const noexcept {
  return count_;
}

template <bool is_const>
field_type basic_field_accessor<is_const>::type() const noexcept {
  return type_;
}

template <bool is_const>
size_t basic_field_accessor<is_const>::stride() const noexcept {
  return stride_;
}

template <bool is_const>
typename basic_field_accessor<is_const>::byte_pointer basic_field_accessor<is_const>::data() const noexcept {
  return base_;
}

template <bool is_const>
double basic_field_accessor<is_const>::get(const size_t element, const uint32_t component) const noexcept {
  if (base_ == nullptr || element >= count_ || component >= type_.components) {
    return 0.0;
  }
  const auto* ptr = base_ + element * stride_ + size_t(component) * type_.component_byte_size();
  return load_component(ptr, type_.base);
}

template <bool is_const>
void basic_field_accessor<is_const>::set(const size_t element, const double value, const uint32_t component) const noexcept
  requires(!is_const)
{
  if (base_ == nullptr || element >= count_ || component >= type_.components) {
    return;
  }
  auto* ptr = base_ + element * stride_ + size_t(component) * type_.component_byte_size();
  store_component(ptr, type_.base, value);
}

template <bool is_const>
bool basic_field_accessor<is_const>::contiguous() const noexcept {
  return type_.valid() && stride_ == type_.byte_size();
}

template <bool is_const>
template <typename type_t>
std::span<std::conditional_t<is_const, const type_t, type_t>> basic_field_accessor<is_const>::as_span() const noexcept {
  using element_type = std::conditional_t<is_const, const type_t, type_t>;
  if (!contiguous() || type_.components != 1 || type_.base != exact_storage_base<type_t>()) {
    return std::span<element_type>{};
  }
  if (sizeof(type_t) != type_.byte_size()) {
    return std::span<element_type>{};
  }
  return std::span<element_type>(reinterpret_cast<element_type*>(base_), count_);
}

} // namespace originator
} // namespace devils_engine

#endif
