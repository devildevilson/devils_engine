#ifndef DEVILS_ENGINE_ORIGINATOR_SCRIPT_PROGRAM_H
#define DEVILS_ENGINE_ORIGINATOR_SCRIPT_PROGRAM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "tools.h"
#include "neighbourhood.h"

// Средний уровень исполнения: devils_script над плотным буфером.
//
// Программа компилируется против ИМЁН входных полей — то есть скрипт пишет `height`, а не `get(0)`.
// Имена берутся у самих привязок в момент первой компиляции, поэтому одно и то же имя означает одно
// и то же в конфиге, в lua и в скрипте.
//
// Обычный запуск структурно pointwise: программа читает только свой элемент. Gather имеет
// отдельную регистрацию own/other и свёртку по явно ограниченному CSR/stencil/множеству.
// Поля-векторы читаются как name_x/y/z/w; result_component выбирает формулу группы выходов.
// Pure subset исполняется колонками, остальное — VM; отказ column compiler является данными,
// а критическая ошибка не перехватывается ради переключения исполнителя.
//
// Случайность: перед каждым элементом состояние PRNG у devils_script выставляется в
// hash(зерно шага, индекс элемента). Поэтому `chance`/`random` внутри скрипта тоже не зависят от
// того, как работа разложилась по потокам. НА УСТРОЙСТВЕ ЭТО ДРУГОЙ ПОТОК: соли мест вызова там
// выводятся переводом, а хеш 32-битный — см. `script_translate.h`.
//
// Параметры: то, что скрипт читает как `ctx:arg:имя`, приходит из параметров шага. Это разделение
// правила и его порогов — само правило живёт в скрипте, а числа остаются в конфиге, где их можно
// крутить, не трогая ни скрипт, ни C++. Аргументы выставляются ОДИН раз на контекст и постоянны на
// весь проход: они описывают проход, а не элемент. param_<name> — числовые uniforms вне
// ограниченного root/argument стека VM; имена явно входят в compile binding signature.

namespace devils_engine {
namespace thread {
class atomic_pool;
}

namespace originator {

// Сколько входных полей программа может видеть. Ограничение существует потому, что каждое имя —
// отдельная compile-time инстанциация аксессора.
constexpr size_t max_script_inputs = 16;
constexpr size_t max_script_parameters = 64;
enum class script_backend { automatic, virtual_machine };
struct script_gather_call;

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
                                                 const result_kind kind,
                                                 const std::span<const field_type> input_types = {},
                                                 const std::span<const std::string> parameter_names = {},
                                                 script_backend backend = script_backend::automatic);

  const std::string& name() const noexcept;
  size_t input_count() const noexcept;
  // Имена, которые скрипт читает как `ctx:arg:имя`. Каждое обязано найтись в параметрах шага.
  const std::vector<std::string>& argument_names() const noexcept;
  result_kind kind() const noexcept;
  const std::vector<field_type>& input_types() const noexcept;
  aperture::values shape() const noexcept;
  // Имена, под которыми программа была скомпилирована: пересобирать её при других именах нельзя.
  const std::vector<std::string>& input_names() const noexcept;

  // Внутреннее состояние (система devils_script и скомпилированный контейнер) намеренно скрыто:
  // публичный заголовок не должен тянуть за собой заголовки devils_script.
  struct implementation;

private:
  script_program() noexcept;

  std::unique_ptr<implementation> impl_;

  friend std::optional<size_t> script_footprint(const script_program&, const size_t);
  friend void check_gather_program(const script_gather_call&);
  friend void dispatch_gather_program(const script_gather_call&, thread::atomic_pool*);

  friend void dispatch_script(const script_program&,
                              const std::span<const field_ref>&,
                              const std::span<const field_ref>&,
                              const parameters&,
                              const uint64_t,
                              const size_t,
                              const size_t,
                              const std::string_view&,
                              thread::atomic_pool*, uint32_t, uint32_t);
};

struct script_reduction {
  enum class kind { maximum, minimum, maximum_absolute, sum, argmin, argmax };
  kind operation = kind::maximum;
  uint32_t score_component = 0;
  double initial = 0;
};

// Программа видит fields своего элемента и other_fields соседнего. Остальные адреса
// выразить нельзя. Список соседей — CSR/stencil либо всё ограниченное множество other.
// Каждый output скалярный; result_component выбирает формулу score/value. Validity
// необязательна и тоже определяется программой, не названием предметного алгоритма.
struct script_gather_call {
  const script_program* program = nullptr;
  std::span<const field_ref> inputs;
  size_t own_inputs = 0;
  std::span<const field_ref> outputs;
  const parameters* params = nullptr;
  const neighbourhood* around = nullptr;
  size_t other_count = 0;
  std::span<const script_reduction> reductions;
  int validity_component = -1;
  bool empty_self = false;
  uint64_t seed = 0;
  size_t begin = 0;
  size_t end = 0;
  std::string_view step;
};

void check_gather_program(const script_gather_call& call);
void dispatch_gather_program(const script_gather_call& call, thread::atomic_pool* pool);

// Верхняя оценка временного frame на одновременно работающих chunks. VM lists могут
// расти из программы: для них возвращается nullopt, а не размер одних заголовков списков.
// nullopt также означает непредставимый размер; allocator metadata не входит в оценку.
std::optional<size_t> script_footprint(const script_program& program, const size_t workers);

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
                                     const std::string_view& step_name, uint32_t output_component = 0);

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
                     thread::atomic_pool* pool, uint32_t output_component = 0, uint32_t result_component = UINT32_MAX);

} // namespace originator
} // namespace devils_engine

#endif
