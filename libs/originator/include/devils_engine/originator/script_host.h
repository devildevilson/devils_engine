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

// LUA-ХОСТ ГЕНЕРАТОРА: headless-окружение, в котором ТЕЛО ШАГА — это lua-функция, плюс регистрация
// программ `devils_script` и сборка очередей вычислений.
//
// РОЛЬ LUA — НЕ «ОПИСАТЬ ПАЙПЛАЙН» (тот лежит в конфиге), а занять место, которое в графическом
// пайплайне занимает command: продирижировать одним куплетом генерации. В графике набор команд можно
// было зафиксировать, потому что зафиксирован конвейер; у генератора конвейера нет, поэтому команда
// обязана быть программируемой.
//
// ЧТО LUA ДЕЛАЕТ: выбирает инструмент, диапазон и буферы, точечно правит небольшие множества,
// вызывает `devils_script` на плотных данных, объявляет очереди, переиспользует буферы. ЧЕГО НЕ
// ДЕЛАЕТ: не решает вопросы многопоточности (они выводятся из апертуры) и не обходит плотный буфер
// поэлементно — на миллионах элементов это секунды.
//
// ОКРУЖЕНИЕ НАМЕРЕННО НЕ ТО ЖЕ, ЧТО У VISAGE: ни UI-глобалов, ни `math.random`, ни файловой системы.
// Детерминизм здесь часть контракта, а не пожелание. По той же причине у тела шага есть БЮДЖЕТ, и он
// не нулевой по умолчанию: зациклившееся тело обязано падать с именем шага и номером строки, а не
// вешать генерацию молча.

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

// Бюджет выполнения тела шага; ноль означает «не ограничивать». Ограничение идёт по ИНСТРУКЦИЯМ, а
// не по времени: счётчик инструкций растёт только пока исполняется lua, поэтому он ловит цикл в
// скрипте и не срабатывает на честном шаге, который час считает шум в нативном ядре. Часы одно от
// другого не отличают, поэтому `wall_time_us` по умолчанию выключен и остаётся инструментом хоста,
// который знает свои сроки.
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

  // ИСПОЛНИТЕЛЬ УСТРОЙСТВЕННЫХ ОЧЕРЕДЕЙ: ставится снаружи, потому что ядру генератора незачем знать
  // про Vulkan. Не поставлен, а очередь просит устройство (`originator.queue{ ..., device = true }`)
  // — ГРОМКИЙ отказ, а не тихий переход на CPU.
  void set_device_executor(queue_executor* executor) noexcept;
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

  // Устройственная форма той же программы. Кэшируется отдельно потому, что зависит ещё и от РОДА
  // ВЫХОДА: одна и та же программа, пишущая в `v1` и в `ui1`, переводится по-разному.
  struct device_form_entry {
    std::string name;
    std::string signature;
    translated_form form;
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
  // Исполнение очереди, объявившей себя устройственной. Отсутствие исполнителя — громкий отказ.
  queue_report run_on_device(const computation_queue& queue);

  // Перевод программы в устройственную форму, с тем же кэшем, что у компиляции: перевод — чистая
  // функция от (текст, имена и роды привязок, род выхода), поэтому ключ у них общий.
  const translated_form& acquire_device_form(const std::string_view& program_name,
                                             const std::string_view& signature,
                                             const std::span<const field_ref>& inputs,
                                             const field_ref& output,
                                             const script_program::result_kind kind);

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
  std::vector<device_form_entry> device_forms_;
  queue_executor* device_executor_ = nullptr;

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
