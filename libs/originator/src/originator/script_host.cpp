#include "devils_engine/originator/script_host.h"

#include "devils_engine/originator/script_translate.h"

#include "devils_engine/bindings/env.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <functional>
#include <cstring>
#include <format>

#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"

// Реализация lua-хоста: окружение, биндинги инструментов, объявление и исполнение очередей.
//
// БИНДИНГИ — ФУНКЦИИ (методы класса и свободные), А НЕ ЛЯМБДЫ. Это обход измеренной ошибки sol2: две
// лямбды с одинаковой сигнатурой внутри одной функции делят метатаблицу `__gc`, и память портится
// молча. Ярлыки инструментов поэтому ставятся ЗАМЫКАНИЯМИ В LUA над привязанным `run`: замыкание в
// lua не стоит ни одного C++-типа.
//
// ДВА НЕЙМСПЕЙСА, А НЕ ФЛАГ В АРГУМЕНТАХ: `originator.box_blur{...}` считает сейчас,
// `originator.queue.box_blur{...}` ОБЪЯВЛЯЕТ работу, которую исполнит очередь целиком. Это два разных
// действия, и разделение по имени делает их различимыми в теле шага с первого взгляда.
//
// ОБЪЯВЛЕННЫЙ И НЕ ОТДАННЫЙ ОЧЕРЕДИ ВЫЗОВ — ПОТЕРЯННАЯ РАБОТА: тело написало `queue.blend{...}`,
// ничего не посчиталось, и по результату это почти незаметно. Поэтому записи считаются, и расхождение
// с числом принятых очередями проваливает шаг так же громко, как ошибка привязки.
//
// КОМПИЛЯЦИЯ ПРОГРАММ ОТЛОЖЕНА до первого вызова: имена полей, против которых компилируется
// программа, известны только по фактическим привязкам. Кэш ключуется парой (имя, набор имён входов),
// а устройственная форма — ещё и родом выхода: та же программа, пишущая в `v1` и в `ui1`, переводится
// по-разному.

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

// МЕХАНИЗМ ОШИБКИ, ИЗ-ЗА КОТОРОЙ БИНДИНГИ — ФУНКЦИИ, А НЕ ЛЯМБДЫ: sol2 кэширует деструктор функтора
// (`__gc`) в реестре lua по имени метатаблицы, а имя берёт из демангленного имени типа. Gcc печатает
// лямбду как `функция()::<lambda(аргументы)>` — БЕЗ порядкового номера, поэтому две разные лямбды,
// объявленные в одной функции с одинаковым списком параметров, получают ОДНО имя: `__gc` первой
// разрушает функтор второй по чужой раскладке. Падало это как «free(): invalid size» при закрытии
// стейта, а порядок захвата решал, упадёт оно или испортит память молча. У функции тип называется её
// собственным именем, поэтому ошибка перестаёт быть ВЫРАЗИМОЙ, а не менее вероятной.

double field_get(const field_ref& self, const size_t index, const sol::optional<uint32_t> component) {
  return self.read().get(index, component.value_or(0));
}

void field_set(const field_ref& self, const size_t index, const double value, const sol::optional<uint32_t> component) {
  if (!self.writable()) {
    utils::error{}("originator: field '{}.{}' is bound for reading and cannot be written",
                   self.buffer_name(), self.field_name());
  }
  self.write().set(index, value, component.value_or(0));
}

uint32_t field_components(const field_ref& self) {
  return self.type().components;
}

std::string_view queue_call_aperture(const queue_call& self) {
  return to_string(self.shape);
}

// Форма буфера телу шага. Отдаётся ТАБЛИЦЕЙ, а не тремя методами, потому что это одно утверждение:
// у объявленной формы оси читаются вместе. `axes` говорит, сколько их объявлено; необъявленная форма
// даёт нули, и по ним видно, что буфер линейный.
//
// Существует затем, чтобы скрипт брал ширину растра ОТТУДА ЖЕ, откуда её берёт инструмент. Пока
// ширина приезжала параметром, тело шага и инструмент читали два разных числа, случайно равных.
sol::table view_extent(const script_buffer_view& self, sol::this_state s) {
  const auto& extent = self.source != nullptr ? self.source->extent() : buffer_extent{};
  sol::state_view lua(s);
  sol::table result(lua, sol::create);
  result["x"] = extent.x;
  result["y"] = extent.y;
  result["z"] = extent.z;
  result["axes"] = extent.axes();
  return result;
}

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

// Диапазон вызова: пара чисел `range = { from, to }` либо СЧЁТЧИК `range = { count = поле }`.
//
// Счётчик не разрешается здесь намеренно. Его пишет предыдущий элемент очереди, а объявление вызова
// происходит ДО исполнения — прочитанное сейчас было бы значением с прошлого раза. Поэтому наружу
// уезжает сама ссылка, а число из неё читает тот, кто исполняет.
field_ref read_range(const sol::table& args, const std::string_view& step_name, size_t& begin, size_t& end) {
  const sol::optional<sol::table> range = args["range"];
  if (!range.has_value()) {
    return field_ref{};
  }

  const sol::object counter = (*range)["count"];
  if (counter.valid()) {
    if (!counter.is<field_ref>()) {
      utils::error{}("originator step '{}': range.count must be a field reference — the element count is READ from a "
                     "buffer, and a plain number goes into range = {{ from, to }}",
                     step_name);
    }
    const auto field = counter.as<const field_ref&>();
    if (!valid_count_field(field)) {
      utils::error{}("originator step '{}': range.count names '{}.{}', which is not a single-component integer — a "
                     "fractional element count is a silent truncation",
                     step_name, field.buffer_name(), field.field_name());
    }
    if ((*range)[1].valid() || (*range)[2].valid()) {
      utils::error{}("originator step '{}': range names both a count field and explicit bounds; a counted range "
                     "starts at zero and ends where the counter says",
                     step_name);
    }
    begin = 0;
    return field;
  }

  begin = (*range)[1].get_or<size_t>(0);
  end = (*range)[2].get_or<size_t>(end);
  return field_ref{};
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

  // БАЗОВЫЕ ФУНКЦИИ ДВИЖКА в таблице `base`, а не своя копия в каждом скрипте: скрипт генератора
  // хеширует ТЕМ ЖЕ, чем хеширует остальной движок (`base.prng64_2` вместо рукописного splitmix).
  // Запрету выше это не противоречит — у них нет своего состояния, число выводится из аргументов.
  // Опираться нельзя только на `base.perf` и `base.script_stack`: это диагностика, и решение,
  // зависящее от времени, сломало бы и воспроизводимость, и равенство «параллельно == последовательно».
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
  //
  // Конструктора нет ни у одного из трёх типов: собранный из lua пустой `field` не падает (аксессор
  // проверяет базу), а МОЛЧА читает нули и молча теряет записи — это хуже, чем его отсутствие.
  env_.new_usertype<field_ref>(
    "field", sol::no_constructor,
    "get", &field_get,
    "set", &field_set,
    "count", &field_ref::count,
    "name", &field_ref::field_name,
    "buffer_name", &field_ref::buffer_name,
    "writable", &field_ref::writable,
    "components", &field_components);

  // Объявленный вызов очереди. Собрать его можно только через originator.queue.<инструмент>{...},
  // где привязки проверяются на месте. Всё, что видно здесь, — диагностика: тип отвечает на «что я
  // объявил», а менять в нём нечего, поэтому `label` отдан только на чтение.
  env_.new_usertype<queue_call>(
    "queue_call", sol::no_constructor,
    "label", sol::readonly(&queue_call::label),
    "aperture", &queue_call_aperture,
    "count", &queue_call::range_count);

  env_.new_usertype<script_buffer_view>(
    "buffer_view", sol::no_constructor,
    "count", &script_buffer_view::count,
    "field", &script_buffer_view::field,
    "writable", &script_buffer_view::writable,
    "name", &script_buffer_view::name,
    "extent", &view_extent,
    "clear", &script_buffer_view::clear);
}

void script_host::bind_tools() {
  for (const auto* reserved : reserved_api_names) {
    if (tools_->find(reserved) != nullptr) {
      utils::error{}("originator: a tool named '{}' would shadow originator.{} and its queue namesake", reserved, reserved);
    }
  }

  sol::table api(lua_, sol::create);
  api.set_function("run", &script_host::run_tool, this);
  // Средний уровень: devils_script над плотным буфером. Форма вызова та же, что у инструмента, —
  // скрипт не выбирает ни потоки, ни апертуру.
  api.set_function("run_script", &script_host::run_program, this);
  // Существует потому, что примитивы (FastNoise2, jc_voronoi) — ОТДЕЛЬНАЯ цель сборки: скрипт может
  // законно спросить, доступен ли инструмент, и выбрать другой путь.
  api.set_function("tool_exists", &script_host::has_tool, this);

  install_shortcuts(api, api["run"]);

  api["queue"] = make_queue_api();

  env_["originator"] = api;
}

// Ярлык на каждый зарегистрированный инструмент: `originator.value_noise{ outputs = {...} }`
// читается лучше, чем `run("value_noise", ...)`, и остаётся ровно тем же вызовом.
//
// Делается это ЗАМЫКАНИЕМ В LUA, а не функтором из C++, и причина — в комментарии к биндингам выше:
// ярлык всего лишь подставляет константное имя первым аргументом, то есть он замыкание над строкой,
// а замыкание в lua не стоит ни одного C++-типа и потому не может столкнуться именем ни с чем.
// Захватывается сама функция `run`, а не таблица: тогда ярлык продолжает работать, даже если тело
// шага переприсвоит `originator.run`.
void script_host::install_shortcuts(sol::table& api, const sol::object& run) {
  static constexpr std::string_view factory_source =
    "return function(run, name) return function(args) return run(name, args) end end";

  const auto factory_result = lua_.safe_script(factory_source, env_, sol::script_pass_on_error, "originator/shortcut");
  if (!factory_result.valid()) {
    const sol::error err = factory_result;
    utils::error{}("originator: could not build the tool shortcut factory: {}", err.what());
  }

  const sol::protected_function factory = factory_result;
  for (size_t i = 0; i < tools_->size(); ++i) {
    const auto& name = tools_->at(i).name;
    const auto made = factory(run, name);
    if (!made.valid()) {
      const sol::error err = made;
      utils::error{}("originator: could not build the shortcut for tool '{}': {}", name, err.what());
    }
    api[name] = made.get<sol::object>();
  }
}

sol::object script_host::run_tool(const std::string& tool_name, const sol::table args, sol::this_state s) {
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
  // Немедленный вызов разрешает счётчик СРАЗУ: возвращаться в дирижёра тут и не надо, он уже здесь.
  // Смысл счётчика — в очереди, где за числом вернуться некуда.
  const auto counter = read_range(args, current_step_, begin, end);
  if (counter.valid()) {
    bool clamped = false;
    end = read_count_field(counter, outputs.empty() ? 0 : outputs.front().count(), clamped);
  }

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

  // Учёт снимает стенные часы ВОКРУГ диспатча: всё, что до него, — разбор таблицы аргументов, и он
  // принадлежит композиции, а не работе.
  const auto started = profile_ != nullptr ? now_us() : 0;
  const auto measured = [&](const size_t elements) {
    if (profile_ == nullptr) return;
    account(tool_name, uint64_t(now_us() - started), elements, touched_fields(inputs, outputs), tool->shape,
            fitness_of(*tool, inputs, outputs));
  };

  if (tool->shape == aperture::reduce) {
    const double value = dispatch_reduce(*tool, inputs, params, seed, begin, end, current_step_, pool_);
    measured(end > begin ? end - begin : 0);
    return sol::make_object(s, value);
  }

  dispatch(*tool, inputs, outputs, params, seed, begin, end, current_step_, pool_);
  measured(end > begin ? end - begin : 0);
  return sol::nil;
}

void script_host::run_program(const sol::table args) {
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
  const auto counter = read_range(args, current_step_, begin, end);
  if (counter.valid()) {
    bool clamped = false;
    end = read_count_field(counter, outputs.empty() ? 0 : outputs.front().count(), clamped);
  }

  // ГОДНОСТЬ ПРОГРАММЫ ЗНАЕТ ТОЛЬКО ПЕРЕВОД, поэтому под учётом она переводится, даже если этот вызов
  // остаётся на CPU: иначе на вопрос «уедет ли» ответить нечем. Цена перевода записывается ОТДЕЛЬНО —
  // это цена первого чанка, а не цена работы, и складывать её с ней значило бы соврать про обе.
  uint64_t translation = 0;
  auto fitness = device_fitness::no_body;
  if (profile_ != nullptr && !outputs.empty() && !device_representable(inputs, outputs)) {
    // Род поля решает РАНЬШЕ перевода, и спрашивать перевод здесь нельзя: он откажет тем же самым, но
    // громко — а диагностический прогон не имеет права выглядеть как череда ошибок.
    fitness = device_fitness::narrow;
  } else if (profile_ != nullptr && !outputs.empty()) {
    const auto translation_started = now_us();
    std::string signature;
    for (const auto& binding : inputs) {
      signature.append(binding.field_name());
      signature.push_back(':');
      signature.append(to_string(binding.type().base));
      signature.push_back(';');
    }
    signature.push_back(kind == script_program::result_kind::predicate ? '?' : '#');
    signature.append(to_string(outputs.front().type().base));
    const auto& form = acquire_device_form(*program_name, signature, inputs, outputs.front(), kind);
    translation = uint64_t(now_us() - translation_started);
    fitness = form.declared() ? device_fitness::ready : device_fitness::no_body;
  }

  const auto started = profile_ != nullptr ? now_us() : 0;
  dispatch_script(program, inputs, outputs, params, seed, begin, end, current_step_, pool_);
  if (profile_ != nullptr) {
    account(*program_name, uint64_t(now_us() - started), end > begin ? end - begin : 0,
            touched_fields(inputs, outputs), program.shape(), fitness, 0, false, translation);
  }
}

bool script_host::has_tool(const std::string& tool_name) const {
  return tools_->find(tool_name) != nullptr;
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
  call.count_from = read_range(args, current_step_, call.range_begin, call.range_end);

  // Апертура и привязки проверяются ЗДЕСЬ, на объявлении, а не при запуске очереди. Проверка та же
  // самая (check_queue делает её ещё раз для очередей, собранных из C++), но сообщение приходит из
  // той строки lua, где вызов написан, — а исполняется очередь в одном месте, и по ней уже не
  // видно, кто из элементов её собрал.
  //
  // У `scatter` есть объявляемое исключение, и спрашивать о нём надо у ИНСТРУМЕНТА: независимость
  // записей от порядка — свойство конкретного алгоритма, а не рода адресации. Правило здесь ровно то
  // же, что у `check_queue`; второго набора условий не появляется.
  const bool admitted = fits_in_queue(call.shape) ||
                        (call.shape == aperture::scatter && tool->order_free_writes);
  if (!admitted) {
    utils::error{}("originator step '{}': tool '{}' has aperture '{}', which the queue does not take: {}",
                   current_step_, tool_name, to_string(call.shape), queue_rejection_reason(call.shape));
  }

  // ЧАНКОВАННЫЙ scatter охраняется носителем ключа, как и у одиночного вызова: раз апертура в очередь
  // теперь попадает, охранять стало что.
  if (call.shape == aperture::scatter) {
    const sol::optional<std::string> declared = args["key_support"];
    const auto support = declared.has_value() ? parse_key_support(*declared) : key_support::global;
    if (declared.has_value() && support == key_support::count) {
      utils::error{}("originator step '{}': tool '{}' got unknown key_support '{}', expected chunk_local or global",
                     current_step_, tool_name, *declared);
    }
    const auto key_check = check_key_support(*tool, support, current_chunked_, current_step_);
    if (!key_check.allowed) {
      utils::error{}("originator {}", key_check.message);
    }
  }

  // У вызова со счётчиком диапазон до исполнения неизвестен, поэтому проверяется ЁМКОСТЬ приёмника —
  // та же граница, по которой зажимается превышение.
  const size_t checked_end = call.count_from.valid() ? (call.outputs.empty() ? 0 : call.outputs.front().count())
                                                     : call.range_end;
  const auto check = check_dispatch(*tool, call.inputs, call.outputs, call.range_begin, checked_end, current_step_);
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
  call.count_from = read_range(args, current_step_, call.range_begin, call.range_end);

  const size_t checked_end = call.count_from.valid() ? (call.outputs.empty() ? 0 : call.outputs.front().count())
                                                     : call.range_end;
  const auto check =
    check_script_dispatch(program, call.inputs, call.outputs, call.params, call.range_begin, checked_end, current_step_);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  // УСТРОЙСТВЕННАЯ ФОРМА — ПЕРЕВОД, и получить её иначе нельзя. Это и есть смысл того, что
  // `run_script` принимает ds, а не шейдер: что считается на этом шаге, решает автор данных, поэтому
  // он приносит программу, а в GLSL её превращает движок по AST, который отдаёт сам `ds`.
  //
  // Непереводимая конструкция — не ошибка: это объявленный ОТКАЗ, по которому очередь остаётся на
  // CPU, а устройственный план называет причину вместо того, чтобы молчать.
  if (!call.outputs.empty()) {
    std::string signature;
    for (const auto& binding : call.inputs) {
      signature.append(binding.field_name());
      signature.push_back(':');
      signature.append(to_string(binding.type().base));
      signature.push_back(';');
    }
    signature.push_back(kind == script_program::result_kind::predicate ? '?' : '#');
    signature.append(to_string(call.outputs.front().type().base));
    const auto translation_started = profile_ != nullptr ? now_us() : 0;
    call.device = acquire_device_form(*program_name, signature, call.inputs, call.outputs.front(), kind);
    if (profile_ != nullptr) {
      pending_translation_us_ += uint64_t(now_us() - translation_started);
    }
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
  api.set_function("run", &script_host::declare_tool, this);
  api.set_function("run_script", &script_host::declare_program, this);

  // Ярлыки те же, что у немедленного вызова, и это важнее краткости: одна и та же строка аргументов
  // читается одинаково в обоих неймспейсах, а отличие ровно одно — когда работа исполняется.
  // Ставятся ВСЕ инструменты, включая непригодные: `originator.queue.group_by{...}` обязан сказать,
  // почему scatter не пускается, а не упасть на «attempt to call a nil value».
  install_shortcuts(api, api["run"]);

  // Сама очередь — ВЫЗОВ таблицы: `originator.queue{ ... }`. Таблица нужна как неймспейс, вызов —
  // как исполнение, и метаметод позволяет иметь и то, и другое под одним именем.
  sol::table meta(lua_, sol::create);
  meta.set_function("__call", &script_host::execute_queue, this);
  api[sol::metatable_key] = meta;

  return api;
}

queue_call script_host::declare_tool(const std::string& tool_name, const sol::table args) {
  auto call = make_tool_call(tool_name, args);
  ++queue_records_;
  return call;
}

queue_call script_host::declare_program(const sol::table args) {
  auto call = make_script_call(args);
  ++queue_records_;
  return call;
}

sol::object script_host::execute_queue(const sol::table self, const sol::table args, sol::this_state s) {
  (void)self;

  computation_queue queue;
  queue.name = current_step_;

  // Список элементов читается по НАИБОЛЬШЕМУ целому ключу, а не по `#`. На таблице с дыркой длина в
  // lua не определена (`{ a, nil, b }` законно даёт и 1, и 3), поэтому по `#` пропущенный элемент мог
  // бы просто исчезнуть — а очередь, посчитавшая на один проход меньше, отличается от правильной
  // только результатом, которого никто не ждал. Здесь дырка обязана падать громко, как и любое
  // значение, объявлением не являющееся.
  //
  // Заодно отклоняется незнакомый ключ: `outputs = {...}` вместо `output` иначе означал бы очередь
  // без объявленной границы, и жаловалась бы она не на опечатку, а на отсутствие выхода.
  size_t highest = 0;
  for (const auto& pair : args) {
    if (pair.first.get_type() == sol::type::number) {
      const int64_t key = pair.first.as<int64_t>();
      if (key >= 1) {
        highest = std::max(highest, size_t(key));
      }
      continue;
    }

    const sol::optional<std::string> name = pair.first.as<sol::optional<std::string>>();
    if (!name.has_value() || (*name != "output" && *name != "resident" && *name != "device")) {
      utils::error{}("originator step '{}': the queue got an unknown key '{}'; a queue takes its elements as a list, "
                     "names what comes back to the host in 'output', what stays on the device in 'resident', and "
                     "where it is computed in 'device'",
                     current_step_, name.value_or(std::string(sol::type_name(s, pair.first.get_type()))));
    }
  }

  queue.calls.reserve(highest);
  for (size_t i = 1; i <= highest; ++i) {
    const sol::object entry = args[i];
    if (!entry.is<queue_call>()) {
      utils::error{}("originator step '{}': queue element {} of {} is '{}', not a declared call — elements come from "
                     "originator.queue.<tool>{{...}}, and a hole in the list is a LOST PASS, not an empty slot",
                     current_step_, i, highest, sol::type_name(s, entry.get_type()));
    }
    queue.calls.push_back(entry.as<const queue_call&>());
    ++queue_consumed_;
  }

  queue.output = read_field_list(args, "output");
  queue.resident = read_field_list(args, "resident");

  // ГДЕ СЧИТАЕТСЯ — ОБЪЯВЛЕНИЕ КОНФИГА (§6.4), а не свойство машины. Все машины берут одну ветку, а
  // та, что исполнить её не может, ОТКАЗЫВАЕТ ГРОМКО: молча посчитав на CPU очередь, решённую на
  // GPU, генератор выдал бы другой мир под тем же зерном.
  const sol::optional<bool> on_device = args["device"];
  queue.on_device = on_device.value_or(false);

  // ОЧЕРЕДЬ УЧИТЫВАЕТСЯ ОДНОЙ ЗАПИСЬЮ: она и исполняется целиком. Годность у неё — годность САМОГО
  // СЛАБОГО элемента: на устройство уезжает вся очередь или ничего, поэтому один непереносимый вызов
  // держит её на CPU, и приписывать ей годность по большинству значило бы завысить долю.
  auto fitness = device_fitness::ready;
  size_t elements = 0;
  size_t by_fitness[device_fitness::count] = {};
  std::vector<field_ref> inputs;
  std::vector<field_ref> outputs;
  for (const auto& call : queue.calls) {
    const auto call_fitness =
      call.tool != nullptr
        ? fitness_of(*call.tool, call.inputs, call.outputs)
        : (!device_representable(call.inputs, call.outputs)
             ? device_fitness::narrow
             : (call.device.declared() ? device_fitness::ready : device_fitness::no_body));
    fitness = std::max(fitness, call_fitness);
    by_fitness[call_fitness] += 1;
    elements += call.range_count();
    inputs.insert(inputs.end(), call.inputs.begin(), call.inputs.end());
    outputs.insert(outputs.end(), call.outputs.begin(), call.outputs.end());
  }

  const auto started = profile_ != nullptr ? now_us() : 0;
  const auto report = queue.on_device ? run_on_device(queue) : run_queue(queue, pool_);
  if (profile_ != nullptr) {
    {
      profile_record record;
      record.step = current_step_;
      record.label = "queue";
      record.microseconds = uint64_t(now_us() - started);
      record.elements = elements;
      record.fields = touched_fields(inputs, outputs);
      for (const auto& [name, bytes] : record.fields) {
        record.bytes += bytes;
      }
      record.shape = aperture::pointwise;
      record.fitness = fitness;
      record.translation_microseconds = pending_translation_us_;
      pending_translation_us_ = 0;
      record.queue_size = queue.calls.size();
      std::copy(std::begin(by_fitness), std::end(by_fitness), std::begin(record.queue_fitness));
      record.on_device = report.on_device;
      profile_->add(std::move(record));
    }
  }

  sol::table result(lua_, sol::create);
  result["calls"] = report.calls;
  result["passes"] = report.passes;
  // `fused` отдаётся телу шага наравне с остальным: тело — единственное место, где видно, СЛИЛОСЬ ли
  // то, что автор писал подряд, а по числу обходов одного этого не понять (обходов столько же и
  // тогда, когда группа развалилась на одиночек).
  result["fused"] = report.fused;
  // Сколько вызовов упёрлось в ёмкость. Тело шага обязано это читать: отказать нельзя (на устройстве
  // бросить нечем), а тихо обрезанный проход по результату не отследить.
  result["clamped"] = report.clamped;
  // Путь ПОДТВЕРЖДАЕТСЯ отчётом: объявить `device = true` мало — тело шага обязано иметь возможность
  // увидеть, что он и был взят.
  result["device"] = report.on_device;
  return sol::make_object(s, result);
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

void script_host::set_device_executor(queue_executor* executor) noexcept {
  device_executor_ = executor;
}

void script_host::set_profile(execution_profile* profile) noexcept {
  profile_ = profile;
}

void script_host::account(std::string label,
                          const uint64_t microseconds,
                          const size_t elements,
                          std::vector<std::pair<std::string, size_t>> fields,
                          const aperture::values shape,
                          const device_fitness::values fitness,
                          const size_t queue_size,
                          const bool on_device,
                          const uint64_t translation_microseconds) {
  if (profile_ == nullptr) {
    return;
  }

  profile_record record;
  record.step = current_step_;
  record.label = std::move(label);
  record.microseconds = microseconds;
  record.elements = elements;
  record.fields = std::move(fields);
  for (const auto& [name, size] : record.fields) {
    record.bytes += size;
  }
  record.translation_microseconds = translation_microseconds;
  record.shape = shape;
  record.fitness = fitness;
  record.queue_size = queue_size;
  record.on_device = on_device;
  profile_->add(std::move(record));
}

queue_report script_host::run_on_device(const computation_queue& queue) {
  if (device_executor_ == nullptr) {
    utils::error{}("originator step '{}': the queue is declared to run on a device, but no device executor is "
                   "installed — the path is a decision of the config, so a machine that cannot take it refuses "
                   "instead of quietly computing something else",
                   current_step_);
  }

  std::string refusal;
  if (!device_executor_->can_run(queue, refusal)) {
    utils::error{}("originator step '{}': the queue is declared to run on a device and cannot: {}",
                   current_step_, refusal);
  }
  return device_executor_->run(queue);
}

const translated_form& script_host::acquire_device_form(const std::string_view& program_name,
                                                       const std::string_view& signature,
                                                       const std::span<const field_ref>& inputs,
                                                       const field_ref& output,
                                                       const script_program::result_kind kind) {
  for (const auto& entry : device_forms_) {
    if (entry.name == program_name && entry.signature == signature) {
      return entry.form;
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

  std::vector<translated_field> fields;
  fields.reserve(inputs.size());
  for (const auto& binding : inputs) {
    fields.push_back(translated_field{std::string(binding.field_name()), binding.type().base});
  }
  const translated_field target{std::string(output.field_name()), output.type().base};

  translated_form form;
  try {
    form = translate_to_glsl(program_name, source->source, fields, target, kind).form;
  } catch (const std::exception& error) {
    // Перевод падает на том, чего он ещё не умеет (случайность, списки, `ctx_save`). Это законный
    // ответ, а не дефект: очередь считается на CPU, а причина едет дальше вместе с объявлением.
    form = translated_form::refused(error.what());
  }

  device_forms_.push_back(device_form_entry{std::string(program_name), std::string(signature), std::move(form)});
  return device_forms_.back().form;
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

    // Шаг меряется ЦЕЛИКОМ, а не суммой своих вызовов: разница между ними и есть время композиции,
    // то есть та часть, которую не переносит никуда никакой перенос.
    const auto step_started = profile_ != nullptr ? now_us() : 0;
    if (profile_ != nullptr) {
      profile_->begin_step(context.name);
    }

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

    // Шаг закрывается ПОСЛЕ всех проверок: провалившийся шаг не является измерением, и запись о нём
    // сюда не доходит — исключение уносит управление раньше.
    if (profile_ != nullptr) {
      profile_->end_step(uint64_t(now_us() - step_started));
    }
  };
}

} // namespace originator
} // namespace devils_engine
