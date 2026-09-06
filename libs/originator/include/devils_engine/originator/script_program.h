#ifndef DEVILS_ENGINE_ORIGINATOR_SCRIPT_PROGRAM_H
#define DEVILS_ENGINE_ORIGINATOR_SCRIPT_PROGRAM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "tools.h"

// Средний уровень исполнения: devils_script над плотным буфером.
//
// Программа компилируется против ИМЁН входных полей — то есть скрипт пишет `height`, а не `get(0)`.
// Имена берутся у самих привязок в момент первой компиляции, поэтому одно и то же имя означает одно
// и то же в конфиге, в lua и в скрипте.
//
// Апертура здесь не объявляется руками, а СЛЕДУЕТ ИЗ РЕГИСТРАЦИИ. Скрипту доступны ровно два вида
// обращений: прочитать поле СВОЕГО элемента и вернуть значение. Функции, которая пишет, не
// зарегистрировано; функции, которая доберётся до соседа, тоже. Поэтому программа структурно
// pointwise, и её невозможно случайно сделать не такой. Когда появится доступ к соседям, он
// приедет отдельной регистрацией — и вместе с ней апертура станет gather со всеми её проверками.
//
// Случайность: перед каждым элементом состояние PRNG у devils_script выставляется в
// hash(зерно шага, индекс элемента). Поэтому `chance`/`random` внутри скрипта тоже не зависят от
// того, как работа разложилась по потокам. НА УСТРОЙСТВЕ ЭТО ДРУГОЙ ПОТОК: соли мест вызова там
// выводятся переводом, а хеш 32-битный — см. `script_translate.h`.
//
// Параметры: то, что скрипт читает как `ctx:arg:имя`, приходит из параметров шага. Это разделение
// правила и его порогов — само правило живёт в скрипте, а числа остаются в конфиге, где их можно
// крутить, не трогая ни скрипт, ни C++. Аргументы выставляются ОДИН раз на контекст и постоянны на
// весь проход: они описывают проход, а не элемент.

namespace devils_engine {
namespace thread {
class atomic_pool;
}

namespace originator {

// Сколько входных полей программа может видеть. Ограничение существует потому, что каждое имя —
// отдельная compile-time инстанциация аксессора.
constexpr size_t max_script_inputs = 16;

class script_program {
public:
  enum class result_kind {
    number,   // скрипт возвращает число
    predicate // скрипт возвращает истину/ложь, в поле уходит 1 или 0
  };

  ~script_program() noexcept;

  script_program(const script_program&) = delete;
  script_program& operator=(const script_program&) = delete;

  // Компилирует исходник. Имена входов задают, как поля видны внутри скрипта, и их порядок
  // совпадает с порядком привязок при запуске. Ошибка компиляции критическая: бросает utils::error.
  static std::unique_ptr<script_program> compile(const std::string_view& name,
                                                 const std::string_view& source,
                                                 const std::span<const std::string>& input_names,
                                                 const result_kind kind);

  const std::string& name() const noexcept;
  size_t input_count() const noexcept;
  // Имена, которые скрипт читает как `ctx:arg:имя`. Каждое обязано найтись в параметрах шага.
  const std::vector<std::string>& argument_names() const noexcept;
  result_kind kind() const noexcept;
  aperture::values shape() const noexcept;
  // Имена, под которыми программа была скомпилирована: пересобирать её при других именах нельзя.
  const std::vector<std::string>& input_names() const noexcept;

  // Внутреннее состояние (система devils_script и скомпилированный контейнер) намеренно скрыто:
  // публичный заголовок не должен тянуть за собой заголовки devils_script.
  struct implementation;

private:
  script_program() noexcept;

  std::unique_ptr<implementation> impl_;

  friend size_t script_footprint(const script_program&, const size_t);

  friend void dispatch_script(const script_program&,
                              const std::span<const field_ref>&,
                              const std::span<const field_ref>&,
                              const parameters&,
                              const uint64_t,
                              const size_t,
                              const size_t,
                              const std::string_view&,
                              thread::atomic_pool*);
};

// ВРЕМЕННАЯ ПАМЯТЬ ПРОГРАММЫ: контекст виртуальной машины `devils_script`, СВОЙ У КАЖДОГО ЧАНКА.
// Величина маленькая (стек операндов, сохранённые слоты и списки, объявленные самой программой), но
// она есть, а «есть и мало» и «неизвестно» — разные ответы: молчание здесь означало бы, что учёт
// памяти считает программы бесплатными, ничего об этом не сказав.
size_t script_footprint(const script_program& program, const size_t workers);

// Проверяет вызов программы, не исполняя его: число входов, совпадение имён привязок с теми, под
// которыми программа компилировалась, изменяемость выхода, диапазон и наличие каждого `ctx:arg:` в
// параметрах. Существует отдельно от запуска ради очереди вычислений: очередь обязана отказать ДО
// первого вызова, а не упасть на середине работы.
dispatch_check check_script_dispatch(const script_program& program,
                                     const std::span<const field_ref>& inputs,
                                     const std::span<const field_ref>& outputs,
                                     const parameters& params,
                                     const size_t range_begin,
                                     const size_t range_end,
                                     const std::string_view& step_name);

// Запуск программы над диапазоном элементов. Форма та же, что у нативного инструмента, и проверки
// те же: выход обязан быть привязан на запись, диапазон обязан помещаться, число входов обязано
// совпасть с тем, против чего программа компилировалась.
void dispatch_script(const script_program& program,
                     const std::span<const field_ref>& inputs,
                     const std::span<const field_ref>& outputs,
                     const parameters& params,
                     const uint64_t seed,
                     const size_t range_begin,
                     const size_t range_end,
                     const std::string_view& step_name,
                     thread::atomic_pool* pool);

} // namespace originator
} // namespace devils_engine

#endif
