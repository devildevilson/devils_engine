#include "devils_engine/originator/script_host.h"

#include <bit>
#include <chrono>
#include <cstring>

#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

namespace {
// Что видит скрипт генератора. Список намеренно короче UI-окружения: ни math.random, ни os.time, ни
// файловой системы — всё, из чего мог бы просочиться недетерминизм.
constexpr std::string_view whitelisted_globals[] = {
  "assert", "error", "ipairs", "next", "pairs", "pcall", "xpcall", "print", "select",
  "tonumber", "tostring", "type", "unpack", "rawequal", "rawget", "rawset",
  "setmetatable", "getmetatable", "_VERSION",
};

constexpr std::string_view safe_libraries[] = {"coroutine", "string", "table", "math", "utf8"};

char script_host_registry_key = 0;

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
    }
  }
  return result;
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

  for (const auto& name : whitelisted_globals) {
    env_[name] = lua_[name];
  }

  for (const auto& library : safe_libraries) {
    sol::table copy(lua_, sol::create);
    const sol::table original = lua_[library];
    for (const auto& pair : original) {
      copy[pair.first] = pair.second;
    }
    env_[library] = copy;
  }

  // Источник недетерминизма номер один: скрипт не имеет своего генератора случайности. Зерно
  // приходит от шага, а случайность считается инструментами как функция от индекса.
  env_["math"]["random"] = sol::nil;
  env_["math"]["randomseed"] = sol::nil;

  lua_pushlightuserdata(lua_.lua_state(), this);
  lua_rawsetp(lua_.lua_state(), LUA_REGISTRYINDEX, &script_host_registry_key);

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

  self->instruction_counter_ += 1000;

  const bool instructions_exceeded =
    self->budget_.instruction_limit != 0 && self->instruction_counter_ >= self->budget_.instruction_limit;
  const bool time_exceeded =
    self->budget_.wall_time_us != 0 && uint64_t(now_us() - self->start_time_us_) >= self->budget_.wall_time_us;

  if (!instructions_exceeded && !time_exceeded) {
    return;
  }

  lua_getinfo(L, "Sl", ar);
  luaL_error(L, "originator step '%s': script budget exceeded at %s:%d",
             self->current_step_.c_str(), ar->short_src, ar->currentline);
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

  env_.new_usertype<script_buffer_view>(
    "buffer_view",
    "count", [](const script_buffer_view& self) { return self.count(); },
    "field", [](const script_buffer_view& self, const std::string& name) { return self.field(name); },
    "writable", [](const script_buffer_view& self) { return self.writable(); },
    "name", [](const script_buffer_view& self) { return std::string(self.name()); },
    "clear", [](const script_buffer_view& self) { self.clear(); });
}

void script_host::bind_tools() {
  sol::table api(lua_, sol::create);

  const auto run = [this](const std::string& tool_name, const sol::table args, sol::this_state s) -> sol::object {
    const auto* tool = tools_->find(tool_name);
    if (tool == nullptr) {
      utils::error{}("originator step '{}': no tool named '{}'", current_step_, tool_name);
    }

    const auto inputs = read_field_list(args, "inputs");
    const auto outputs = read_field_list(args, "outputs");
    const auto params = read_parameters(args);

    // Зерно ходит через lua как знаковое целое: у lua нет беззнакового 64-битного типа, а значение
    // здесь непрозрачный токен, а не число, которым считают.
    const sol::optional<int64_t> explicit_seed = args["seed"];
    const uint64_t seed = explicit_seed.has_value() ? std::bit_cast<uint64_t>(*explicit_seed) : current_seed_;

    // Откуда берётся диапазон по умолчанию, решает АПЕРТУРА. У scatter выход — структура другого
    // размера (смещения групп, суммы по корзинам), поэтому её размер задаёт число корзин, а не
    // число обрабатываемых элементов: считать по нему было бы тихой обработкой первых N элементов.
    const bool count_from_inputs = tool->shape == aperture::scatter || outputs.empty();
    size_t begin = 0;
    size_t end = 0;
    if (count_from_inputs) {
      end = inputs.empty() ? 0 : inputs.front().count();
    } else {
      end = outputs.front().count();
    }

    const sol::optional<sol::table> range = args["range"];
    if (range.has_value()) {
      begin = (*range)[1].get_or<size_t>(0);
      end = (*range)[2].get_or<size_t>(end);
    }

    if (tool->shape == aperture::reduce) {
      const double value = dispatch_reduce(*tool, inputs, params, seed, begin, end, current_step_, pool_);
      return sol::make_object(s, value);
    }

    dispatch(*tool, inputs, outputs, params, seed, begin, end, current_step_, pool_);
    return sol::nil;
  };

  api.set_function("run", run);

  // Ярлык на каждый зарегистрированный инструмент: originator.value_noise{ outputs = {...} }
  // читается лучше, чем run("value_noise", ...), и остаётся ровно тем же вызовом.
  for (size_t i = 0; i < tools_->size(); ++i) {
    const std::string name = tools_->at(i).name;
    api.set_function(name, [this, name, run](const sol::table args, sol::this_state s) {
      return run(name, args, s);
    });
  }

  // Средний уровень: devils_script над плотным буфером. Форма вызова та же, что у инструмента, —
  // скрипт не выбирает ни потоки, ни апертуру.
  api.set_function("run_script", [this](const sol::table args) {
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

    const sol::optional<int64_t> explicit_seed = args["seed"];
    const uint64_t seed = explicit_seed.has_value() ? std::bit_cast<uint64_t>(*explicit_seed) : current_seed_;

    size_t begin = 0;
    size_t end = outputs.empty() ? 0 : outputs.front().count();
    const sol::optional<sol::table> range = args["range"];
    if (range.has_value()) {
      begin = (*range)[1].get_or<size_t>(0);
      end = (*range)[2].get_or<size_t>(end);
    }

    dispatch_script(program, inputs, outputs, params, seed, begin, end, current_step_, pool_);
  });

  api.set_function("tool_exists", [this](const std::string& name) { return tools_->find(name) != nullptr; });
  api.set_function("aperture_of", [this](const std::string& name) -> sol::optional<std::string> {
    const auto* tool = tools_->find(name);
    if (tool == nullptr) {
      return sol::nullopt;
    }
    return std::string(to_string(tool->shape));
  });

  env_["originator"] = api;
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

    auto step = make_step_table(context);

    const bool limited = budget_.instruction_limit != 0 || budget_.wall_time_us != 0;
    if (limited) {
      instruction_counter_ = 0;
      start_time_us_ = now_us();
      lua_sethook(lua_.lua_state(), &script_host::instruction_hook, LUA_MASKCOUNT, 1000);
    }

    const auto result = entry->function(step);

    if (limited) {
      lua_sethook(lua_.lua_state(), nullptr, 0, 0);
    }

    if (!result.valid()) {
      const sol::error err = result;
      utils::error{}("originator step '{}': body failed: {}", context.name, err.what());
    }
  };
}

} // namespace originator
} // namespace devils_engine
