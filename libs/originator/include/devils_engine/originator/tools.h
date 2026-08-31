#ifndef DEVILS_ENGINE_ORIGINATOR_TOOLS_H
#define DEVILS_ENGINE_ORIGINATOR_TOOLS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "buffer.h"
#include "common.h"

// Реестр нативных инструментов и их запуск.
//
// Ключевое правило библиотеки: ПАРАЛЛЕЛЬНОСТЬ НЕ СООБЩАЕТСЯ СКРИПТОМ, А ВЫВОДИТСЯ. Инструмент
// объявляет апертуру — как он адресуется относительно обрабатываемого элемента, — а движок из неё
// и из фактических привязок решает, можно ли разбивать работу. Скрипт не может соврать про то,
// чего он не знает, а нарушающий вызов не собирается: у привязки на чтение нет операции записи, а
// gather с совпадающими источником и приёмником отклоняется ДО исполнения.
//
// Следствие для автора инструмента: тело получает диапазон элементов и обязано вести себя так,
// будто соседние диапазоны исполняются одновременно. Для pointwise/gather это выполняется само,
// если тело пишет только в свой элемент.

namespace devils_engine {
namespace thread {
class atomic_pool;
}

namespace originator {

// Ссылка на конкретное поле конкретного буфера. Изменяемость наследуется от привязки шага:
// получить изменяемую ссылку на буфер, объявленный в reads, невозможно.
struct field_ref {
  const buffer* source = nullptr;
  buffer* target = nullptr; // nullptr => только чтение
  size_t field_index = 0;

  bool valid() const noexcept;
  bool writable() const noexcept;
  size_t count() const noexcept;
  field_type type() const noexcept;
  std::string_view buffer_name() const noexcept;
  std::string_view field_name() const noexcept;

  const_field_accessor read() const noexcept;
  field_accessor write() const noexcept; // пустой аксессор, если ссылка не изменяемая

  // Один и тот же (буфер, поле). Поля НИКОГДА не делят байты, поэтому этого достаточно и для aos:
  // разные поля одного буфера не пересекаются независимо от раскладки.
  bool same_field_as(const field_ref& other) const noexcept;
};

// Небольшой набор именованных параметров вызова. Числа и строки — всё, что нужно инструменту;
// тяжёлые данные лежат в буферах и приходят привязками.
class parameters {
public:
  void set_number(const std::string_view& name, const double value);
  void set_string(const std::string_view& name, std::string value);

  bool has(const std::string_view& name) const noexcept;

  // Перечисление — чтобы хост мог отдать параметры конфига дальше (например в таблицу lua),
  // не заводя второго представления тех же значений.
  size_t size() const noexcept;
  std::string_view name_at(const size_t index) const noexcept;
  bool is_string_at(const size_t index) const noexcept;
  double number_at(const size_t index) const noexcept;
  std::string_view string_at(const size_t index) const noexcept;

  // Накладывает other поверх текущего набора: то, что задано в other, побеждает. Нужно для общих
  // значений пайплайна, которые шаг может переопределить своим параметром.
  void overlay(const parameters& other);

  double number(const std::string_view& name, const double fallback = 0.0) const noexcept;
  int64_t integer(const std::string_view& name, const int64_t fallback = 0) const noexcept;
  std::string_view string(const std::string_view& name, const std::string_view& fallback = {}) const noexcept;

private:
  struct entry {
    std::string name;
    std::string text;
    double value = 0.0;
    bool is_string = false;
  };

  const entry* find(const std::string_view& name) const noexcept;

  std::vector<entry> entries_;
};

// Что инструмент получает в теле.
struct tool_call {
  std::span<const field_ref> inputs;
  std::span<const field_ref> outputs;
  const parameters* params = nullptr;
  uint64_t seed = 0;
  size_t range_begin = 0;
  size_t range_end = 0;
  std::string_view step_name;
  std::string_view tool_name;

  // Состояние, построенное подготовкой инструмента один раз до разбиения работы: пространственный
  // индекс, диаграмма, таблица — то, что нельзя строить в каждом чанке заново.
  const void* shared = nullptr;

  // Пул доступен ТОЛЬКО телам с апертурой scatter и sequential: их движок не разбивает, потому что
  // корректное разбиение зависит от алгоритма, и они организуют свои фазы сами. Тело pointwise,
  // gather или reduce уже исполняется внутри чанка и параллелить себя не должно.
  thread::atomic_pool* pool = nullptr;

  size_t range_count() const noexcept;
  const field_ref& input(const size_t index) const;
  const field_ref& output(const size_t index) const;
};

// Тело инструмента исполняется над ПОДДИАПАЗОНОМ. Для параллельных апертур поддиапазонов много и
// они идут одновременно, для sequential он ровно один и равен полному диапазону.
using tool_body = std::function<void(const tool_call& call, const size_t chunk_begin, const size_t chunk_end)>;

// Свёртка разбита на две части намеренно: частичный результат считается по чанку, а объединение
// идёт СТРОГО в порядке чанков. Поэтому сумма плавающих чисел не зависит от числа потоков.
using reduce_body = std::function<double(const tool_call& call, const size_t chunk_begin, const size_t chunk_end)>;
using reduce_combine = std::function<double(const double accumulated, const double partial)>;

// Подготовка: вызывается ОДИН раз перед разбиением работы, результат живёт до конца вызова и
// приходит в тело через tool_call::shared. Нужна инструментам, которым требуется общая структура.
using tool_prepare = std::function<std::shared_ptr<void>(const tool_call& call)>;

struct tool_description {
  std::string name;
  aperture::values shape = aperture::pointwise;
  uint32_t input_count = 0;
  uint32_t output_count = 0;
  // Инициализаторы по умолчанию нужны не для красоты: описание заполняется именованной
  // инициализацией с опущенными полями, и без них компилятор законно жалуется на пропуски.
  tool_body body = nullptr;
  tool_prepare prepare = nullptr;

  // Заполняются только у апертуры reduce; тогда body остаётся пустым.
  reduce_body partial = nullptr;
  reduce_combine combine = nullptr;
  double initial = 0.0;
};

// Свёртка: много элементов -> одно значение. Разбиение на чанки ФИКСИРОВАНО и не зависит от числа
// потоков, иначе порядок сложения плавающих чисел менял бы результат.
constexpr size_t reduce_chunk_size = 65536;

class tool_registry {
public:
  void add(tool_description description);
  const tool_description* find(const std::string_view& name) const noexcept;
  const tool_description& at(const size_t index) const noexcept;
  size_t size() const noexcept;

  // Регистрирует набор инструментов, поставляемых движком.
  void add_standard_tools();
  // Инструменты с апертурой scatter. Они существуют отдельно потому, что произвольного
  // параллельного разброса по чужим индексам НЕ БЫВАЕТ детерминированным: детерминизм даёт не
  // апертура, а конкретный алгоритм, поэтому scatter доступен только этим готовым инструментам.
  void add_scatter_tools();

private:
  std::vector<tool_description> tools_;
};

// Результат проверки вызова. Пустое сообщение => вызов допустим.
struct dispatch_check {
  bool allowed = true;
  bool parallel = false;
  std::string message;
};

// Проверяет вызов, не исполняя его: число привязок, изменяемость выходов, диапазон и — для gather —
// непересечение источника с приёмником.
dispatch_check check_dispatch(const tool_description& tool,
                              const std::span<const field_ref>& inputs,
                              const std::span<const field_ref>& outputs,
                              const size_t range_begin,
                              const size_t range_end,
                              const std::string_view& step_name);

// Проверяет, допустим ли scatter-вызов при активном чанке. Для остальных апертур и для
// нечанкованной генерации разрешает всегда: там носитель ключа ни на что не влияет.
//
// Глобальный scatter в чанковом проходе — не «неточный», а бессмысленный: ни одна группа не
// заканчивается, и результат зависит от того, какие чанки успели посчитаться. Такую сводку считают в
// грубом мировом проходе, где чанков нет, а не собирают из чанков.
dispatch_check check_key_support(const tool_description& tool,
                                 const key_support::values support,
                                 const bool chunk_active,
                                 const std::string_view& step_name);

// Запускает инструмент. pool == nullptr => всё исполняется в вызывающем потоке; результат обязан
// совпадать бит в бит с параллельным исполнением, и это проверяется тестом.
// Недопустимый вызов — критическая ошибка конфига/скрипта: бросает utils::error с именем шага.
void dispatch(const tool_description& tool,
              const std::span<const field_ref>& inputs,
              const std::span<const field_ref>& outputs,
              const parameters& params,
              const uint64_t seed,
              const size_t range_begin,
              const size_t range_end,
              const std::string_view& step_name,
              thread::atomic_pool* pool);

// Свёртка. Возвращает значение, одинаковое при любом числе потоков и при pool == nullptr.
double dispatch_reduce(const tool_description& tool,
                       const std::span<const field_ref>& inputs,
                       const parameters& params,
                       const uint64_t seed,
                       const size_t range_begin,
                       const size_t range_end,
                       const std::string_view& step_name,
                       thread::atomic_pool* pool);

} // namespace originator
} // namespace devils_engine

#endif
