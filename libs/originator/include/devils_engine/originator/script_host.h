#ifndef DEVILS_ENGINE_ORIGINATOR_SCRIPT_HOST_H
#define DEVILS_ENGINE_ORIGINATOR_SCRIPT_HOST_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>

#include "computation_queue.h"
#include "pipeline.h"
#include "script_program.h"
#include "tools.h"

// Lua-хост генератора: отдельное headless-окружение, в котором ТЕЛО ШАГА — это lua-функция.
//
// Роль lua здесь не «описать пайплайн» — пайплайн лежит в конфиге. Lua занимает то место, которое в
// графическом пайплайне занимает command: она дирижирует одним куплетом генерации. В графике набор
// команд можно было зафиксировать, потому что зафиксирован сам конвейер; у генератора конвейера нет,
// поэтому команда должна быть программируемой.
//
// Что lua делает: выбирает инструмент, диапазон и буферы, точечно правит небольшие множества,
// вызывает devils_script на плотных данных, переиспользует буферы. Чего lua НЕ делает: не решает
// вопросы многопоточности (они выводятся из апертуры инструмента) и не обходит плотный буфер
// поэлементно — на миллионах элементов это секунды.
//
// Окружение намеренно НЕ то же, что у visage: у генератора нет UI-глобалов, нет math.random и нет
// файловой системы. Детерминизм здесь — часть контракта, а не пожелание.

namespace devils_engine {
namespace thread {
class atomic_pool;
}

namespace originator {

// Вью на буфер со стороны скрипта. Изменяемость наследуется от привязки шага: у вью из reads
// target == nullptr, и получить из него изменяемую ссылку на поле невозможно.
struct script_buffer_view {
  const buffer* source = nullptr;
  buffer* target = nullptr;

  size_t count() const noexcept;
  bool writable() const noexcept;
  std::string_view name() const noexcept;
  // Разрешает имя поля в ссылку ОДИН раз; дальше в горячем пути идёт уже она, без поиска по строке.
  field_ref field(const std::string& field_name) const;
  void clear() const;
};

// Бюджеты выполнения тела шага. Ноль означает «не ограничивать» — но по умолчанию бюджет НЕ нулевой,
// и это осознанное отличие от прежнего поведения: зациклившееся тело шага обязано падать с именем
// шага и номером строки, а не вешать генерацию молча. Ровно та же роль, что у budget_config в visage,
// с одним отличием в выборе величины.
//
// Ограничение по умолчанию идёт по ИНСТРУКЦИЯМ, а не по времени, и причина в том, что эти две
// величины измеряют разное. Счётчик инструкций растёт только пока исполняется lua: время, проведённое
// внутри нативного инструмента, в него не попадает. Значит бюджет по инструкциям ловит именно то, что
// нужно ловить — цикл в скрипте, — и не срабатывает на честном шаге, который час считает мировой шум
// в нативном ядре. Часы, напротив, не отличают одно от другого, поэтому wall_time_us по умолчанию
// выключен и остаётся инструментом хоста, который знает свои сроки.
//
// Величина взята из правила самой библиотеки: lua обходит только те множества, которые сама
// перечислила. Тело, потратившее сотни миллионов инструкций, это правило уже нарушило, а перф-стенду,
// который нарушает его намеренно, ничего не мешает выставить ноль вслух.
struct script_budget {
  uint64_t instruction_limit = 200000000;
  uint64_t wall_time_us = 0;
};

class script_host {
public:
  script_host(tool_registry& tools, thread::atomic_pool* pool);
  ~script_host() noexcept;

  script_host(const script_host&) = delete;
  script_host& operator=(const script_host&) = delete;

  sol::state& state() noexcept;
  sol::environment& env() noexcept;

  void set_budget(const script_budget budget) noexcept;
  const script_budget& budget() const noexcept;

  // Загружает тело шага. Чанк ОБЯЗАН вернуть функцию — она и есть шаг.
  void load_body(const std::string_view& step_name, const std::string_view& source, const std::string_view& chunk_name);
  bool has_body(const std::string_view& step_name) const noexcept;

  // Регистрирует исходник программы devils_script под именем. Компиляция ОТЛОЖЕНА до первого вызова:
  // имена полей, против которых программа компилируется, известны только по фактическим привязкам.
  void load_program(const std::string_view& program_name, const std::string_view& source);
  bool has_program(const std::string_view& program_name) const noexcept;

  // Invoker для pipeline::run: находит тело по имени шага и вызывает его с построенной таблицей.
  step_invoker invoker();

private:
  struct body_entry {
    std::string step_name;
    sol::protected_function function;
  };

  struct program_source {
    std::string name;
    std::string source;
  };

  // Скомпилированная программа кэшируется по паре (имя, набор имён входов): та же программа с
  // другими входами — это другая компиляция, а не переиспользование.
  struct program_entry {
    std::string name;
    std::string signature;
    std::unique_ptr<script_program> program;
  };

  static void instruction_hook(lua_State* L, lua_Debug* ar);
  void bind_types();
  void bind_tools();
  // Отдельный неймспейс `originator.queue`: те же инструменты, но вызов не исполняется, а
  // ОБЪЯВЛЯЕТСЯ. Разделение по имени, а не по флагу в аргументах, потому что это два разных
  // действия: `originator.box_blur{...}` считает сейчас, `originator.queue.box_blur{...}` описывает
  // работу, которую исполнит очередь целиком.
  sol::table make_queue_api();
  // Ярлыки инструментов ставятся ЗАМЫКАНИЯМИ В LUA над привязанным `run` и константным именем.
  // Причина — в комментарии к биндингам в script_host.cpp: у лямбды из C++ имя типа не уникально,
  // а замыкание в lua не стоит ни одного C++-типа.
  void install_shortcuts(sol::table& api, const sol::object& run);

  // БИНДИНГИ — МЕТОДЫ, А НЕ ЛЯМБДЫ, и это обход измеренной ошибки порчи памяти в sol2; полностью
  // причина описана в script_host.cpp рядом со свободными функциями биндингов.
  sol::object run_tool(const std::string& tool_name, const sol::table args, sol::this_state s);
  void run_program(const sol::table args);
  bool has_tool(const std::string& tool_name) const;
  queue_call declare_tool(const std::string& tool_name, const sol::table args);
  queue_call declare_program(const sol::table args);
  sol::object execute_queue(const sol::table self, const sol::table args, sol::this_state s);

  // Собирает объявленный вызов из таблицы аргументов lua; счётчик объявлений трогают declare_*.
  queue_call make_tool_call(const std::string& tool_name, const sol::table& args);
  queue_call make_script_call(const sol::table& args);
  sol::table make_step_table(const step_context& context);
  const body_entry* find_body(const std::string_view& step_name) const noexcept;
  const script_program& acquire_program(const std::string_view& program_name,
                                        const std::span<const field_ref>& inputs,
                                        const script_program::result_kind kind);

  sol::state lua_;
  sol::environment env_;
  tool_registry* tools_ = nullptr;
  thread::atomic_pool* pool_ = nullptr;
  std::vector<body_entry> bodies_;
  std::vector<program_source> program_sources_;
  std::vector<program_entry> programs_;

  script_budget budget_{};
  // Объявленный и НЕ отданный очереди вызов — потерянная работа: тело написало `queue.blend{...}`,
  // ничего не посчиталось, и заметить это по результату почти нечем. Поэтому записи считаются, и
  // расхождение с числом принятых очередями проваливает шаг так же громко, как ошибка привязки.
  uint64_t queue_records_ = 0;
  uint64_t queue_consumed_ = 0;
  uint64_t instruction_counter_ = 0;
  // Шаг счётчика хуков считается из лимита ровно как в visage: маленький лимит обязан ловиться
  // рано, а большой — не платить за хук чаще, чем раз в 10000 инструкций.
  uint32_t hook_interval_ = 10000;
  // Бюджет исчерпан. Существует потому, что ошибку из хука можно поймать pcall'ом, а бюджет — не
  // рядовая ошибка тела: шаг, доевший его, не считается выполненным, даже если сам себя простил.
  bool budget_tripped_ = false;
  int64_t start_time_us_ = 0;
  uint64_t current_seed_ = 0;
  bool current_chunked_ = false;
  std::string current_step_;
};

} // namespace originator
} // namespace devils_engine

#endif
