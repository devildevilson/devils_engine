#include "devils_engine/originator/script_host.h"

#include "devils_engine/bindings/env.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <functional>
#include <cstring>
#include <format>

#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

namespace {
// Что видит скрипт генератора. Список намеренно короче UI-окружения: ни math.random, ни os.time, ни
// файловой системы — всё, из чего мог бы просочиться недетерминизм.
//
// Ключи здесь именно const char*, и это не косметика: с ключом std::string_view проксирование sol2
// молча промахивалось, и весь этот список оседал в окружении как nil. Тела шагов GN01 этого не
// заметили, потому что обращались только к step, originator и арифметике, а единственный error() в
// terrain.lua стоял на пути, который не срабатывал. Поэтому ниже есть ещё и проверка ЯДРА списка:
// такая пропажа обязана падать при создании хоста, а не всплывать через полгода на первой ошибке.
constexpr const char* whitelisted_globals[] = {
  "assert", "error", "ipairs", "next", "pairs", "pcall", "xpcall", "print", "select",
  "tonumber", "tostring", "type", "unpack", "rawequal", "rawget", "rawset", "rawlen",
  "setmetatable", "getmetatable", "_VERSION",
};

// Без чего тело шага писать нельзя. Отсутствие любого из них означает, что окружение собралось
// неправильно, а не что автор скрипта чего-то не знает.
constexpr const char* mandatory_globals[] = {
  "assert", "error", "pcall", "type", "tostring", "tonumber", "pairs", "ipairs",
};

constexpr const char* safe_libraries[] = {"coroutine", "string", "table", "math", "utf8"};

// Имена, занятые самой таблицей `originator` и её неймспейсом `queue`. Ярлыки инструментов кладутся
// в те же таблицы, поэтому инструмент с таким именем затёр бы функцию МОЛЧА — и тело шага получило
// бы вместо очереди обычный вызов или вместо `run_script` инструмент. Кто кого затрёт, зависело бы
// ещё и от порядка регистрации, а такую пропажу по результату генерации не видно.
constexpr const char* reserved_api_names[] = {"run", "run_script", "tool_exists", "queue"};

char script_host_registry_key = 0;

// ФУНКЦИИ УХОДЯТ В LUA ЧЕРЕЗ std::function С ОБЪЯВЛЕННОЙ СИГНАТУРОЙ, А НЕ ЛЯМБДОЙ НАПРЯМУЮ, и это
// не стиль, а обход ИЗМЕРЕННОЙ ошибки порчи памяти.
//
// sol2 хранит функтор в userdata и кэширует его деструктор (`__gc`) в реестре lua по имени
// метатаблицы, а имя получает из ДЕМАНГЛИРОВАННОГО имени типа. Gcc печатает лямбду как
// `функция()::<lambda(аргументы)>` — БЕЗ порядкового номера. Значит две РАЗНЫЕ лямбды, объявленные
// в одной функции с одинаковым списком параметров, получают одно и то же имя метатаблицы:
// `luaL_newmetatable` находит уже созданную, и `__gc` ПЕРВОЙ лямбды начинает разрушать функтор
// ВТОРОЙ по чужой раскладке. Проверено: `sol.main()::<lambda(sol::table)>.user` совпадает у двух
// лямбд с разными захватами, а падение выглядит как «free(): invalid size» при закрытии стейта, то
// есть далеко от места ошибки и без всякой связи с ней.
//
// У `std::function<результат(аргументы)>` имя своё и полное, поэтому совпадение имён означает
// совпадение ТИПОВ, а разные типы получают разные метатаблицы. Ошибка перестаёт быть возможной, а
// не становится менее вероятной: две регистрации с одной сигнатурой ниже — это намеренно ОДИН тип.
using immediate_run_binding = std::function<sol::object(const std::string&, const sol::table, sol::this_state)>;
using immediate_tool_binding = std::function<sol::object(const sol::table, sol::this_state)>;
using immediate_script_binding = std::function<void(const sol::table)>;
using tool_exists_binding = std::function<bool(const std::string&)>;
using queue_named_record_binding = std::function<queue_call(const std::string&, const sol::table)>;
using queue_record_binding = std::function<queue_call(const sol::table)>;
using queue_run_binding = std::function<sol::object(const sol::table, const sol::table, sol::this_state)>;

int64_t now_us() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<field_ref> read_field_list(const sol::table& args, const char* key) {
  std::vector<field_ref> result;
  const sol::optional<sol::table> list = args[key];
  if (!list.has_value()) {
    return result;
  }

  const size_t size = list->size();
  result.reserve(size);
  for (size_t i = 1; i <= size; ++i) {
    const sol::optional<field_ref> entry = (*list)[i];
    if (!entry.has_value()) {
      utils::error{}("originator: '{}' entry {} is not a field reference", key, i);
    }
    result.push_back(*entry);
  }
  return result;
}

parameters read_parameters(const sol::table& args) {
  parameters result;
  const sol::optional<sol::table> table = args["params"];
  if (!table.has_value()) {
    return result;
  }

  for (const auto& [key, value] : *table) {
    if (!key.is<std::string>()) {
      continue;
    }
    const auto name = key.as<std::string>();
    if (value.is<double>()) {
      result.set_number(name, value.as<double>());
    } else if (value.is<std::string>()) {
      result.set_string(name, value.as<std::string>());
    } else if (value.is<bool>()) {
      // Флаг инструмента — то же число, только записанное так, как его естественно писать в lua.
      // Проверка идёт ПОСЛЕДНЕЙ: у числа lua_toboolean тоже истинен, и порядок здесь единственное,
      // что не даёт единице приехать булевым. Без этой ветки `absolute = true` молча пропадал бы —
      // параметр не число и не строка, значит его просто нет, и инструмент брал бы значение по
      // умолчанию, ничего не сообщая.
      result.set_number(name, value.as<bool>() ? 1.0 : 0.0);
    }
  }
  return result;
}

// Откуда берётся диапазон по умолчанию, решает АПЕРТУРА. У scatter выход — структура другого
// размера (смещения групп, суммы по корзинам), поэтому её размер задаёт число корзин, а не число
// обрабатываемых элементов: считать по нему было бы тихой обработкой первых N элементов.
size_t default_range_end(const aperture::values shape,
                         const std::span<const field_ref>& inputs,
                         const std::span<const field_ref>& outputs) noexcept {
  const bool count_from_inputs = shape == aperture::scatter || outputs.empty();
  if (count_from_inputs) {
    return inputs.empty() ? 0 : inputs.front().count();
  }
  return outputs.front().count();
}

void read_range(const sol::table& args, size_t& begin, size_t& end) {
  const sol::optional<sol::table> range = args["range"];
  if (!range.has_value()) {
    return;
  }
  begin = (*range)[1].get_or<size_t>(0);
  end = (*range)[2].get_or<size_t>(end);
}

// Зерно ходит через lua как знаковое целое: у lua нет беззнакового 64-битного типа, а значение
// здесь непрозрачный токен, а не число, которым считают.
uint64_t read_seed(const sol::table& args, const uint64_t fallback) {
  const sol::optional<int64_t> explicit_seed = args["seed"];
  return explicit_seed.has_value() ? std::bit_cast<uint64_t>(*explicit_seed) : fallback;
}
} // namespace

size_t script_buffer_view::count() const noexcept {
  return source != nullptr ? source->count() : 0;
}

bool script_buffer_view::writable() const noexcept {
  return target != nullptr;
}

std::string_view script_buffer_view::name() const noexcept {
  return source != nullptr ? std::string_view(source->name()) : std::string_view("<none>");
}

field_ref script_buffer_view::field(const std::string& field_name) const {
  if (source == nullptr) {
    utils::error{}("originator: field '{}' requested from an empty buffer view", field_name);
  }

  const size_t index = source->find_field(field_name);
  if (index == buffer_layout::npos) {
    utils::error{}("originator buffer '{}': no field named '{}'", source->name(), field_name);
  }

  return field_ref{source, target, index};
}

void script_buffer_view::clear() const {
  if (target == nullptr) {
    utils::error{}("originator buffer '{}': bound for reading, cannot be cleared", name());
  }
  target->clear();
}

script_host::script_host(tool_registry& tools, thread::atomic_pool* pool) : tools_(&tools), pool_(pool) {
  lua_.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::utf8);

  env_ = sol::environment(lua_, sol::create);
  env_["_G"] = env_;

  for (const auto* name : whitelisted_globals) {
    // unpack в 5.4 не существует (он уехал в table.unpack), поэтому отсутствие имени в списке —
    // норма, а вот отсутствие ЗНАЧЕНИЯ у скопированного имени было бы тихой потерей.
    sol::object value = lua_[name];
    if (value.valid()) {
      env_[name] = value;
    }
  }

  for (const auto* library : safe_libraries) {
    const sol::object source = lua_[library];
    if (!source.is<sol::table>()) {
      utils::error{}("originator: standard library '{}' is not available in the generator state", library);
    }

    sol::table copy(lua_, sol::create);
    const sol::table original = source.as<sol::table>();
    size_t copied = 0;
    for (const auto& pair : original) {
      copy[pair.first] = pair.second;
      ++copied;
    }
    if (copied == 0) {
      utils::error{}("originator: standard library '{}' copied empty into the generator environment", library);
    }
    env_[library] = copy;
  }

  for (const auto* name : mandatory_globals) {
    const sol::object value = env_[name];
    if (!value.valid()) {
      utils::error{}("originator: generator environment lost the mandatory global '{}'", name);
    }
  }

  // Источник недетерминизма номер один: скрипт не имеет своего генератора случайности. Зерно
  // приходит от шага, а случайность считается инструментами как функция от индекса.
  env_["math"]["random"] = sol::nil;
  env_["math"]["randomseed"] = sol::nil;

  // БАЗОВЫЕ ФУНКЦИИ ДВИЖКА в таблице `base`, а не своя копия в каждом скрипте. Они сделаны на общих
  // утилитах (`utils::prng`, `utils::dice`, упаковка), поэтому скрипт генератора хеширует ТЕМ ЖЕ, чем
  // хеширует остальной движок: `base.prng64_2(a, b)` вместо рукописного splitmix в теле шага,
  // `base.prng64_normalize` вместо деления на 2^53 руками, `base.value`/`base.dice` для чисел из
  // состояния.
  //
  // Случайность отсюда НЕ противоречит запрету выше: у всех этих функций нет своего состояния —
  // число выводится из аргументов, то есть из зерна шага и номера элемента. Единственные функции
  // таблицы, на которые телу шага опираться нельзя, — `base.perf` (замер времени) и
  // `base.script_stack`: это диагностика, и решение, зависящее от времени, сломало бы и
  // воспроизводимость, и равенство «параллельно == последовательно».
  bindings::basic_functions(env_);

  lua_pushlightuserdata(lua_.lua_state(), this);
  lua_rawsetp(lua_.lua_state(), LUA_REGISTRYINDEX, &script_host_registry_key);

  // Бюджет по умолчанию ненулевой, поэтому шаг хука обязан быть посчитан от него, а не остаться
  // значением поля: конструктор и set_budget должны приводить хост в одно и то же состояние.
  set_budget(budget_);

  bind_types();
  bind_tools();
}

script_host::~script_host() noexcept = default;

sol::state& script_host::state() noexcept {
  return lua_;
}

sol::environment& script_host::env() noexcept {
  return env_;
}

void script_host::set_budget(const script_budget budget) noexcept {
  budget_ = budget;

  // Шаг хука выводится из лимита, а не берётся константой: с лимитом в 500 инструкций хук, стоящий
  // раз в 10000, не сработает НИ РАЗУ, и маленький бюджет окажется тихо бесконечным.
  const uint64_t interval = budget_.instruction_limit == 0
                              ? 10000ull
                              : std::min<uint64_t>(budget_.instruction_limit, 10000ull);
  hook_interval_ = uint32_t(std::max<uint64_t>(interval, 1ull));
}

const script_budget& script_host::budget() const noexcept {
  return budget_;
}

void script_host::instruction_hook(lua_State* L, lua_Debug* ar) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &script_host_registry_key);
  auto* self = static_cast<script_host*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (self == nullptr) {
    return;
  }

  self->instruction_counter_ += self->hook_interval_;

  const bool instructions_exceeded =
    self->budget_.instruction_limit != 0 && self->instruction_counter_ >= self->budget_.instruction_limit;
  const bool time_exceeded =
    self->budget_.wall_time_us != 0 && uint64_t(now_us() - self->start_time_us_) >= self->budget_.wall_time_us;

  if (!instructions_exceeded && !time_exceeded) {
    return;
  }

  // Флаг ставится ДО ошибки: её может поймать pcall внутри тела, а факт исчерпания бюджета от этого
  // не исчезает. После возврата из тела хост посмотрит на флаг и всё равно провалит шаг.
  self->budget_tripped_ = true;

  lua_getinfo(L, "Sl", ar);
  // Сообщение собирается заранее и уходит одной строкой: lua_pushfstring понимает лишь узкий набор
  // спецификаторов, и '%llu' для него не существует.
  const auto message = std::format("originator step '{}': script budget exceeded ({} instructions, {} us)",
                                   self->current_step_, self->instruction_counter_,
                                   uint64_t(now_us() - self->start_time_us_));
  luaL_error(L, "%s at %s:%d", message.c_str(), ar->short_src, ar->currentline);
}

void script_host::bind_types() {
  // Ссылка на поле: имя разрешено один раз, дальше работа идёт по ней. Индекс элемента — с НУЛЯ,
  // как в самих данных: индексная арифметика вида i % width иначе становится источником ошибок.
  env_.new_usertype<field_ref>(
    "field",
    "get", [](const field_ref& self, const size_t index, const sol::optional<uint32_t> component) {
      return self.read().get(index, component.value_or(0));
    },
    "set", [](const field_ref& self, const size_t index, const double value, const sol::optional<uint32_t> component) {
      if (!self.writable()) {
        utils::error{}("originator: field '{}.{}' is bound for reading and cannot be written",
                       self.buffer_name(), self.field_name());
      }
      self.write().set(index, value, component.value_or(0));
    },
    "count", [](const field_ref& self) { return self.count(); },
    "name", [](const field_ref& self) { return std::string(self.field_name()); },
    "buffer_name", [](const field_ref& self) { return std::string(self.buffer_name()); },
    "writable", [](const field_ref& self) { return self.writable(); },
    "components", [](const field_ref& self) { return self.type().components; });

  // Объявленный вызов очереди. Конструктора нет намеренно: собрать его можно только через
  // originator.queue.<инструмент>{...}, где привязки проверяются на месте. Методы здесь
  // диагностические — они отвечают на «что я объявил», а не дают что-то менять.
  env_.new_usertype<queue_call>(
    "queue_call", sol::no_constructor,
    "label", [](const queue_call& self) { return self.label; },
    "aperture", [](const queue_call& self) { return std::string(to_string(self.shape)); },
    "count", [](const queue_call& self) { return self.range_end > self.range_begin ? self.range_end - self.range_begin : 0; });

  env_.new_usertype<script_buffer_view>(
    "buffer_view",
    "count", [](const script_buffer_view& self) { return self.count(); },
    "field", [](const script_buffer_view& self, const std::string& name) { return self.field(name); },
    "writable", [](const script_buffer_view& self) { return self.writable(); },
    "name", [](const script_buffer_view& self) { return std::string(self.name()); },
    "clear", [](const script_buffer_view& self) { self.clear(); });
}

void script_host::bind_tools() {
  for (const auto* reserved : reserved_api_names) {
    if (tools_->find(reserved) != nullptr) {
      utils::error{}("originator: a tool named '{}' would shadow originator.{} and its queue namesake", reserved, reserved);
    }
  }

  sol::table api(lua_, sol::create);

  const auto run = [this](const std::string& tool_name, const sol::table args, sol::this_state s) -> sol::object {
    const auto* tool = tools_->find(tool_name);
    if (tool == nullptr) {
      utils::error{}("originator step '{}': no tool named '{}'", current_step_, tool_name);
    }

    const auto inputs = read_field_list(args, "inputs");
    const auto outputs = read_field_list(args, "outputs");
    const auto params = read_parameters(args);

    const uint64_t seed = read_seed(args, current_seed_);

    size_t begin = 0;
    size_t end = default_range_end(tool->shape, inputs, outputs);
    read_range(args, begin, end);

    // Носитель ключа объявляет автор: из одного чанка его не проверить. Молчание = глобальный, то
    // есть при чанкованной генерации scatter отклоняется, пока автор не скажет обратное вслух.
    if (tool->shape == aperture::scatter) {
      const sol::optional<std::string> declared = args["key_support"];
      const auto support = declared.has_value() ? parse_key_support(*declared) : key_support::global;
      if (declared.has_value() && support == key_support::count) {
        utils::error{}("originator step '{}': tool '{}' got unknown key_support '{}', expected chunk_local or global",
                       current_step_, tool_name, *declared);
      }

      const auto check = check_key_support(*tool, support, current_chunked_, current_step_);
      if (!check.allowed) {
        utils::error{}("originator {}", check.message);
      }
    }

    if (tool->shape == aperture::reduce) {
      const double value = dispatch_reduce(*tool, inputs, params, seed, begin, end, current_step_, pool_);
      return sol::make_object(s, value);
    }

    dispatch(*tool, inputs, outputs, params, seed, begin, end, current_step_, pool_);
    return sol::nil;
  };

  api.set_function("run", immediate_run_binding(run));

  // Ярлык на каждый зарегистрированный инструмент: originator.value_noise{ outputs = {...} }
  // читается лучше, чем run("value_noise", ...), и остаётся ровно тем же вызовом.
  for (size_t i = 0; i < tools_->size(); ++i) {
    const std::string name = tools_->at(i).name;
    api.set_function(name, immediate_tool_binding([name, run](const sol::table args, sol::this_state s) {
      return run(name, args, s);
    }));
  }

  // Средний уровень: devils_script над плотным буфером. Форма вызова та же, что у инструмента, —
  // скрипт не выбирает ни потоки, ни апертуру.
  api.set_function("run_script", immediate_script_binding([this](const sol::table args) {
    const sol::optional<std::string> program_name = args["program"];
    if (!program_name.has_value()) {
      utils::error{}("originator step '{}': run_script needs a 'program' name", current_step_);
    }

    const auto inputs = read_field_list(args, "inputs");
    const auto outputs = read_field_list(args, "outputs");
    const auto params = read_parameters(args);

    const sol::optional<bool> predicate = args["predicate"];
    const auto kind = predicate.value_or(false) ? script_program::result_kind::predicate
                                                : script_program::result_kind::number;

    const auto& program = acquire_program(*program_name, inputs, kind);

    const uint64_t seed = read_seed(args, current_seed_);

    size_t begin = 0;
    size_t end = default_range_end(program.shape(), inputs, outputs);
    read_range(args, begin, end);

    dispatch_script(program, inputs, outputs, params, seed, begin, end, current_step_, pool_);
  }));

  // Существует потому, что примитивы (FastNoise2, jc_voronoi) — ОТДЕЛЬНАЯ цель сборки: скрипт может
  // законно спросить, доступен ли инструмент, и выбрать другой путь.
  api.set_function("tool_exists", tool_exists_binding([this](const std::string& name) { return tools_->find(name) != nullptr; }));

  api["queue"] = make_queue_api();

  env_["originator"] = api;
}

queue_call script_host::make_tool_call(const std::string& tool_name, const sol::table& args) {
  const auto* tool = tools_->find(tool_name);
  if (tool == nullptr) {
    utils::error{}("originator step '{}': no tool named '{}'", current_step_, tool_name);
  }

  queue_call call;
  call.label = tool_name;
  call.tool = tool;
  call.shape = tool->shape;
  call.inputs = read_field_list(args, "inputs");
  call.outputs = read_field_list(args, "outputs");
  call.params = read_parameters(args);
  call.seed = read_seed(args, current_seed_);
  call.range_end = default_range_end(call.shape, call.inputs, call.outputs);
  read_range(args, call.range_begin, call.range_end);

  // Апертура и привязки проверяются ЗДЕСЬ, на объявлении, а не при запуске очереди. Проверка та же
  // самая (check_queue делает её ещё раз для очередей, собранных из C++), но сообщение приходит из
  // той строки lua, где вызов написан, — а исполняется очередь в одном месте, и по ней уже не
  // видно, кто из элементов её собрал.
  //
  // key_support здесь не спрашивается намеренно: scatter в очередь не пускается вовсе, значит
  // носителю ключа нечего охранять.
  if (!fits_in_queue(call.shape)) {
    utils::error{}("originator step '{}': tool '{}' has aperture '{}', which the queue does not take: {}",
                   current_step_, tool_name, to_string(call.shape), queue_rejection_reason(call.shape));
  }

  const auto check = check_dispatch(*tool, call.inputs, call.outputs, call.range_begin, call.range_end, current_step_);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  return call;
}

queue_call script_host::make_script_call(const sol::table& args) {
  const sol::optional<std::string> program_name = args["program"];
  if (!program_name.has_value()) {
    utils::error{}("originator step '{}': queue.run_script needs a 'program' name", current_step_);
  }

  queue_call call;
  call.label = *program_name;
  call.inputs = read_field_list(args, "inputs");
  call.outputs = read_field_list(args, "outputs");
  call.params = read_parameters(args);
  call.seed = read_seed(args, current_seed_);

  const sol::optional<bool> predicate = args["predicate"];
  const auto kind = predicate.value_or(false) ? script_program::result_kind::predicate
                                              : script_program::result_kind::number;

  // Компиляция идёт на ОБЪЯВЛЕНИИ, то есть всё ещё до исполнения очереди: программа компилируется
  // против имён фактических привязок, и они здесь уже известны.
  const auto& program = acquire_program(*program_name, call.inputs, kind);

  call.shape = program.shape();
  call.range_end = default_range_end(call.shape, call.inputs, call.outputs);
  read_range(args, call.range_begin, call.range_end);

  const auto check =
    check_script_dispatch(program, call.inputs, call.outputs, call.params, call.range_begin, call.range_end, current_step_);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  // Ядро очереди про devils_script не знает, поэтому элемент несёт своё исполнение телом. Указатель
  // на программу стабилен: программы живут в unique_ptr, и рост кэша их не перемещает.
  const auto* pointer = &program;
  call.body = [pointer](const queue_call& queued, const std::string_view& step_name, thread::atomic_pool* pool) {
    dispatch_script(*pointer, queued.inputs, queued.outputs, queued.params, queued.seed, queued.range_begin,
                    queued.range_end, step_name, pool);
  };

  return call;
}

sol::table script_host::make_queue_api() {
  sol::table api(lua_, sol::create);

  api.set_function("run", queue_named_record_binding([this](const std::string& tool_name, const sol::table args) {
    auto call = make_tool_call(tool_name, args);
    ++queue_records_;
    return call;
  }));

  // Ярлыки те же, что у немедленного вызова, и это важнее краткости: одна и та же строка аргументов
  // читается одинаково в обоих неймспейсах, а отличие ровно одно — когда работа исполняется.
  // Регистрируются ВСЕ инструменты, включая непригодные: `originator.queue.group_by{...}` обязан
  // сказать, почему scatter не пускается, а не упасть на «attempt to call a nil value».
  for (size_t i = 0; i < tools_->size(); ++i) {
    const std::string name = tools_->at(i).name;
    api.set_function(name, queue_record_binding([this, name](const sol::table args) {
      auto call = make_tool_call(name, args);
      ++queue_records_;
      return call;
    }));
  }

  api.set_function("run_script", queue_record_binding([this](const sol::table args) {
    auto call = make_script_call(args);
    ++queue_records_;
    return call;
  }));

  // Сама очередь — ВЫЗОВ таблицы: `originator.queue{ ... }`. Таблица нужна как неймспейс, вызов —
  // как исполнение, и метаметод позволяет иметь и то, и другое под одним именем.
  sol::table meta(lua_, sol::create);
  meta.set_function("__call", queue_run_binding([this](const sol::table self, const sol::table args, sol::this_state s) -> sol::object {
    (void)self;

    computation_queue queue;
    queue.name = current_step_;

    const size_t size = args.size();
    queue.calls.reserve(size);
    for (size_t i = 1; i <= size; ++i) {
      const sol::object entry = args[i];
      if (!entry.is<queue_call>()) {
        utils::error{}("originator step '{}': queue element {} is not a declared call — elements of a queue come "
                       "from originator.queue.<tool>{{...}}, not from originator.<tool>{{...}}",
                       current_step_, i);
      }
      queue.calls.push_back(entry.as<const queue_call&>());
      ++queue_consumed_;
    }

    queue.output = read_field_list(args, "output");

    const auto report = run_queue(queue, pool_);

    sol::table result(lua_, sol::create);
    result["calls"] = report.calls;
    result["passes"] = report.passes;
    return sol::make_object(s, result);
  }));
  api[sol::metatable_key] = meta;

  return api;
}

void script_host::load_body(const std::string_view& step_name, const std::string_view& source, const std::string_view& chunk_name) {
  const auto result = lua_.safe_script(source, env_, sol::script_pass_on_error, std::string(chunk_name));
  if (!result.valid()) {
    const sol::error err = result;
    utils::error{}("originator step '{}': could not load body '{}': {}", step_name, chunk_name, err.what());
  }

  const sol::object returned = result;
  if (returned.get_type() != sol::type::function) {
    utils::error{}("originator step '{}': body '{}' must return a function, got '{}'",
                   step_name, chunk_name, sol::type_name(lua_.lua_state(), returned.get_type()));
  }

  for (auto& entry : bodies_) {
    if (entry.step_name == step_name) {
      entry.function = returned.as<sol::protected_function>();
      return;
    }
  }

  bodies_.push_back(body_entry{std::string(step_name), returned.as<sol::protected_function>()});
}

void script_host::load_program(const std::string_view& program_name, const std::string_view& source) {
  for (auto& entry : program_sources_) {
    if (entry.name == program_name) {
      if (entry.source != source) {
        utils::error{}("originator: program '{}' is already registered with different source", program_name);
      }
      return;
    }
  }
  program_sources_.push_back(program_source{std::string(program_name), std::string(source)});
}

bool script_host::has_program(const std::string_view& program_name) const noexcept {
  for (const auto& entry : program_sources_) {
    if (entry.name == program_name) {
      return true;
    }
  }
  return false;
}

const script_program& script_host::acquire_program(const std::string_view& program_name,
                                                   const std::span<const field_ref>& inputs,
                                                   const script_program::result_kind kind) {
  // Имена входов берутся у самих привязок: поле зовётся в скрипте ровно так же, как в конфиге, и
  // второго списка имён, который мог бы разъехаться с первым, не существует.
  std::vector<std::string> names;
  names.reserve(inputs.size());
  std::string signature;
  for (const auto& binding : inputs) {
    if (!binding.valid()) {
      utils::error{}("originator step '{}': program '{}' got an invalid input", current_step_, program_name);
    }
    names.emplace_back(binding.field_name());
    signature.append(names.back());
    signature.push_back(kind == script_program::result_kind::predicate ? '?' : '#');
  }

  for (const auto& entry : programs_) {
    if (entry.name == program_name && entry.signature == signature) {
      return *entry.program;
    }
  }

  const program_source* source = nullptr;
  for (const auto& entry : program_sources_) {
    if (entry.name == program_name) {
      source = &entry;
      break;
    }
  }
  if (source == nullptr) {
    utils::error{}("originator step '{}': no devils_script program named '{}'", current_step_, program_name);
  }

  auto compiled = script_program::compile(program_name, source->source, names, kind);
  programs_.push_back(program_entry{std::string(program_name), std::move(signature), std::move(compiled)});
  return *programs_.back().program;
}

bool script_host::has_body(const std::string_view& step_name) const noexcept {
  return find_body(step_name) != nullptr;
}

const script_host::body_entry* script_host::find_body(const std::string_view& step_name) const noexcept {
  for (const auto& entry : bodies_) {
    if (entry.step_name == step_name) {
      return &entry;
    }
  }
  return nullptr;
}

sol::table script_host::make_step_table(const step_context& context) {
  sol::table step(lua_, sol::create);
  step["name"] = std::string(context.name);
  step["index"] = context.index;
  step["seed"] = std::bit_cast<int64_t>(context.seed);
  step["chunk_seed"] = std::bit_cast<int64_t>(context.chunk_seed);

  // Ключ чанка нужен телу шага, чтобы посчитать мировое смещение: поле шума в чанке (2,3) должно
  // продолжать поле соседнего чанка, а не начинаться заново.
  sol::table chunk(lua_, sol::create);
  chunk["x"] = context.chunk.x;
  chunk["y"] = context.chunk.y;
  chunk["z"] = context.chunk.z;
  step["chunk"] = chunk;

  // Параметры шага приходят из конфига: скрипт не хардкодит числа, которые хочет крутить автор.
  sol::table params(lua_, sol::create);
  if (context.params != nullptr) {
    for (size_t i = 0; i < context.params->size(); ++i) {
      const auto name = std::string(context.params->name_at(i));
      if (context.params->is_string_at(i)) {
        params[name] = std::string(context.params->string_at(i));
      } else {
        params[name] = context.params->number_at(i);
      }
    }
  }
  step["params"] = params;

  sol::table writes(lua_, sol::create);
  for (auto* target : context.writes) {
    writes[target->name()] = script_buffer_view{target, target};
  }
  step["writes"] = writes;

  // Программы шага видны телу по имени, объявленному в конфиге.
  sol::table programs(lua_, sol::create);
  for (const auto& [program_name, path] : context.programs) {
    (void)path;
    programs[program_name] = program_name;
  }
  step["programs"] = programs;

  sol::table reads(lua_, sol::create);
  for (const auto* source : context.reads) {
    reads[source->name()] = script_buffer_view{source, nullptr};
  }
  step["reads"] = reads;

  return step;
}

step_invoker script_host::invoker() {
  return [this](const step_context& context) {
    const auto* entry = find_body(context.name);
    if (entry == nullptr) {
      utils::error{}("originator step '{}': no body was loaded", context.name);
    }

    current_step_.assign(context.name);
    current_seed_ = context.seed;
    current_chunked_ = context.chunked;
    queue_records_ = 0;
    queue_consumed_ = 0;

    auto step = make_step_table(context);

    const bool limited = budget_.instruction_limit != 0 || budget_.wall_time_us != 0;
    if (limited) {
      instruction_counter_ = 0;
      budget_tripped_ = false;
      start_time_us_ = now_us();
      lua_sethook(lua_.lua_state(), &script_host::instruction_hook, LUA_MASKCOUNT, int(hook_interval_));
    }

    const auto result = entry->function(step);

    if (limited) {
      lua_sethook(lua_.lua_state(), nullptr, 0, 0);
    }

    if (!result.valid()) {
      const sol::error err = result;
      utils::error{}("originator step '{}': body failed: {}", context.name, err.what());
    }

    // Тело вернулось «успешно», но бюджет по дороге кончился — значит ошибку поймали внутри и
    // продолжили. Шаг всё равно провален: иначе зацикленное тело с pcall внутри осталось бы висеть,
    // а генерация — считать его выполненным.
    if (budget_tripped_) {
      utils::error{}("originator step '{}': script budget exceeded and the error was swallowed inside the body "
                     "({} instructions, {} us)",
                     context.name, instruction_counter_, uint64_t(now_us() - start_time_us_));
    }

    // ОБЪЯВЛЕННЫЙ ВЫЗОВ ОБЯЗАН ПОПАСТЬ В ОЧЕРЕДЬ РОВНО ОДИН РАЗ. Иначе тело шага получает две тихие
    // ошибки, которых не видно по результату: забытый элемент ничего не посчитал, а отданный дважды
    // посчитал одно и то же поле теми же аргументами второй раз.
    if (queue_consumed_ < queue_records_) {
      utils::error{}("originator step '{}': {} declared queue calls were never handed to a queue — "
                     "originator.queue.<tool>{{...}} only DECLARES work, it runs inside originator.queue{{ ... }}",
                     context.name, queue_records_ - queue_consumed_);
    }
    if (queue_consumed_ > queue_records_) {
      utils::error{}("originator step '{}': a declared queue call went into a queue more than once ({} elements "
                     "against {} declarations)",
                     context.name, queue_consumed_, queue_records_);
    }
  };
}

} // namespace originator
} // namespace devils_engine
