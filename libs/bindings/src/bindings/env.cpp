#include "env.h"

#include <queue>
#include <vector>

#include "utils/prng.h"
#include "utils/dice.h"
#include "utils/time-utils.hpp"
#include "utils/core.h"
#include "shared.h"

namespace devils_engine {
namespace bindings {
constexpr auto whitelisted = {
  "assert",
  "error",
  "warn",
  "ipairs",
  "next",
  "pairs",
  "pcall",
  "xpcall",
  "print",
  "select",
  "tonumber",
  "tostring",
  "type", // тайп будет еще свой дополнительно
  "unpack",
  "_VERSION",
  "xpcall",

  // возможно мне это не нужно, но на всякий случай оставлю
  "rawequal",
  "rawget",
  "rawset",
  "setmetatable",
  "getmetatable"

  // появится собственная реализация require + функция которая ответит на вопрос загружен ли модуль
};

constexpr auto safe_libraries = {
  "coroutine", 
  "string", 
  "table", 
  "math",
  "utf8",
};

sol::environment create_env(sol::state_view s) {
  auto env = sol::environment(s, sol::create);
  env["_G"] = env;
  s["security_env"] = env;

  for (const auto &name : whitelisted) {
    env[name] = s[name];
  }

  for (const auto &name : safe_libraries) {
    sol::table copy(s, sol::create);
    const sol::table t = s[name];
    for (const auto &pair : t) {
      copy[pair.first] = pair.second;
    }
    env[name] = copy;
  }

  env["math"]["random"] = sol::nil;
  env["math"]["randomseed"] = sol::nil;

  sol::table os(s, sol::create);
  os["clock"] = s["os"]["clock"];
  os["date"] = s["os"]["date"];
  os["difftime"] = s["os"]["difftime"];
  os["time"] = s["os"]["time"];
  env["os"] = os;

  return env;
}

static constexpr double pack_u32f32(const uint32_t a, const float b) {
  struct s { uint32_t u32; float f32; };
  return std::bit_cast<double>(s{a, b});
}

static constexpr double pack_u32u32(const uint32_t a, const uint32_t b) {
  struct s { uint32_t u32_1; uint32_t u32_2; };
  return std::bit_cast<double>(s{a, b});
}

static constexpr double pack_f32f32(const float a, const float b) {
  struct s { float f32_1; float f32_2; };
  return std::bit_cast<double>(s{a, b});
}

static constexpr std::tuple<uint32_t, float> unpack_u32f32(const double cont) {
  struct s { uint32_t u32; float f32; };
  const auto s1 = std::bit_cast<s>(cont);
  return std::make_tuple(s1.u32, s1.f32);
}

static constexpr std::tuple<uint32_t, uint32_t> unpack_u32u32(const double cont) {
  struct s { uint32_t u32_1; uint32_t u32_2; };
  const auto s1 = std::bit_cast<s>(cont);
  return std::make_tuple(s1.u32_1, s1.u32_2);
}

static constexpr std::tuple<float, float> unpack_f32f32(const double cont) {
  struct s { float f32_1; float f32_2; };
  const auto s1 = std::bit_cast<s>(cont);
  return std::make_tuple(s1.f32_1, s1.f32_2);
}

static constexpr bool test() {
  uint32_t a = 125;
  float b = 524;
  const auto d = pack_u32f32(a, b);
  const auto [a1, b1] = unpack_u32f32(d);
  return a == a1 && b == b1;
}

static_assert(test());

static constexpr int64_t u64_to_s64(const uint64_t a) {
  return std::bit_cast<int64_t>(a);
}

static constexpr uint64_t s64_to_u64(const int64_t a) {
  return std::bit_cast<uint64_t>(a);
}

static uint64_t prng64_raw(const uint64_t val) {
  return utils::splitmix64::value(utils::splitmix64::init(val));
}

static int64_t prng64(const int64_t val) {
  const auto val1 = s64_to_u64(val);
  return u64_to_s64(prng64_raw(val1));
}

static int64_t prng64_2(const int64_t value1, const int64_t value2) {
  const auto val1 = s64_to_u64(value1);
  const auto val2 = s64_to_u64(value2);
  //auto s = utils::xoroshiro128starstar::state{ { prng64_raw(val1), prng64_raw(val2) } };
  //return u64_to_s64(utils::xoroshiro128starstar::value(s));
  return u64_to_s64(utils::mix(prng64_raw(val1), prng64_raw(val2)));
}

static double prng64_normalize(const int64_t value) {
  const auto val = s64_to_u64(value);
  return utils::prng_normalize(val);
}

static sol::table create_array(const double num, const sol::object &obj, sol::this_state s, sol::this_environment e) {
  const size_t size = num;
  sol::environment env = e;
  auto t = env.create(size, 0);
  if (!obj.valid()) return t;

  if (obj.get_type() == sol::type::table) {
    for (size_t i = 0; i < size; ++i) {
      const size_t index = DEVILS_ENGINE_TO_LUA_INDEX(i);
      t[index] = env.create(30, 0);
    }
  } else {
    for (size_t i = 0; i < size; ++i) {
      const size_t index = DEVILS_ENGINE_TO_LUA_INDEX(i);
      t[index] = obj;
    }
  }

  return t;
}

static sol::table create_table(const sol::object arr_size, const sol::object hash_size, sol::this_environment e) {
  sol::environment env = e;
  const int32_t narr = arr_size.is<int32_t>() ? arr_size.as<int32_t>() : 100;
  const int32_t nhash = hash_size.is<int32_t>() ? hash_size.as<int32_t>() : 100;
  return env.create(narr, nhash);
}

static void num_queue(const double first_count, const sol::function prepare_function, const sol::function queue_function, sol::this_state s) {
  if (first_count < 0) utils::error("Bad count value {}, function: {}", first_count, "num_queue");
  if (std::abs(first_count) < DEVILS_ENGINE_EPSILON) return;

  sol::state_view lua = s;
  std::queue<double> queue;
  const auto push_func = [&queue] (const double data) { queue.push(data); };
  sol::object lua_push_func = sol::make_object(lua, push_func);

  const size_t size = first_count;
  for (size_t i = 0; i < size; ++i) {
    prepare_function(DEVILS_ENGINE_TO_LUA_INDEX(i), lua_push_func);
  }

  while (!queue.empty()) {
    const double data = queue.front();
    queue.pop();
          
    queue_function(data, lua_push_func);
  }
}

static void queue(const double first_count, const sol::function prepare_function, const sol::function queue_function, sol::this_state s) {
  if (first_count < 0) utils::error("Bad count value {}, function: {}", first_count, "num_queue");
  if (std::abs(first_count) < DEVILS_ENGINE_EPSILON) return;

  sol::state_view lua = s;
  std::queue<sol::object> queue;
  const auto push_func = [&queue] (const sol::object data) { queue.push(data); };
  sol::object lua_push_func = sol::make_object(lua, push_func);

  const size_t size = first_count;
  for (size_t i = 0; i < size; ++i) {
    prepare_function(DEVILS_ENGINE_TO_LUA_INDEX(i), lua_push_func);
  }

  while (!queue.empty()) {
    const sol::object data = queue.front();
    queue.pop();
          
    queue_function(data, lua_push_func);
  }
}

static void num_random_queue(const int64_t seed, const double first_count, const sol::function prepare_function, const sol::function queue_function, sol::this_state s) {
  if (first_count < 0) utils::error("Bad count value {}, function: {}", first_count, "num_queue");
  if (std::abs(first_count) < DEVILS_ENGINE_EPSILON) return;
        
  sol::state_view lua = s;
  std::vector<double> queue;
  queue.reserve(first_count * 2);
  const auto push_func = [&queue] (const double data) { queue.push_back(data); };
  sol::object lua_push_func = sol::make_object(lua, push_func);
  auto rnd_state = utils::xoshiro256starstar::init(s64_to_u64(seed));
        
  for (size_t i = 0; i < first_count; ++i) {
    prepare_function(DEVILS_ENGINE_TO_LUA_INDEX(i), lua_push_func);
  }
        
  while (!queue.empty()) {
    const size_t rand_index = utils::interval(queue.size(), rnd_state);
    const double data = queue[rand_index];
    queue[rand_index] = queue.back();
    queue.pop_back();
          
    queue_function(data, lua_push_func);
  }
}

static void random_queue(const int64_t seed, const double first_count, const sol::function prepare_function, const sol::function queue_function, sol::this_state s) {
  if (first_count < 0) utils::error("Bad count value {}, function: {}", first_count, "num_queue");
  if (std::abs(first_count) < DEVILS_ENGINE_EPSILON) return;
        
  sol::state_view lua = s;
  std::vector<sol::object> queue;
  queue.reserve(first_count * 2);
  const auto push_func = [&queue] (const sol::object data) { queue.push_back(data); };
  sol::object lua_push_func = sol::make_object(lua, push_func);
  auto rnd_state = utils::xoshiro256starstar::init(s64_to_u64(seed));
        
  for (size_t i = 0; i < first_count; ++i) {
    prepare_function(DEVILS_ENGINE_TO_LUA_INDEX(i), lua_push_func);
  }
        
  while (!queue.empty()) {
    const size_t rand_index = utils::interval(queue.size(), rnd_state);
    const sol::object data = queue[rand_index];
    queue[rand_index] = queue.back();
    queue.pop_back();
          
    queue_function(data, lua_push_func);
  }
}

static std::string_view platform() {
#ifdef _WIN32
  return "windows";
#else
  return "linux";
#endif
}

static std::string_view project() {
  return std::string_view(DEVILS_ENGINE_PROJECT_NAME);
}

static sol::variadic_results perf(const sol::function &f, const sol::variadic_args &args, sol::this_state s) {
  const auto t = std::chrono::steady_clock::now();
  const auto res = f(sol::as_args(args));
  const auto dur = std::chrono::steady_clock::now() - t;
  const size_t mcs = std::chrono::duration_cast<std::chrono::microseconds>(dur).count();
  sol_lua_check_error(s, res);
  sol::variadic_results final_res(res.begin(), res.end());
  final_res.push_back(sol::make_object(s, int64_t(mcs)));
  return final_res;
}

// возможно для этих двух функций использовать другой prng?
static std::tuple<int64_t, int64_t> interval(const int64_t max, const int64_t state) {
  //auto s = utils::splitmix64::state{{s64_to_u64(state)}};
  auto s = utils::xoshiro256starstar::init(s64_to_u64(state));
  const auto res = utils::interval(max, s);
  // поправить индекс для Луа ??? да интервал я использую исключительно для массивов ... имеет смысл
  return std::make_tuple(DEVILS_ENGINE_TO_LUA_INDEX(res), u64_to_s64(s.s[0]));
}

static std::tuple<double, int64_t> dice(const double count, const double upper_bound, const int64_t state) {
  //auto s = utils::splitmix64::state{{s64_to_u64(state)}};
  auto s = utils::xoshiro256starstar::init(s64_to_u64(state));
  const auto res = utils::dice_accumulator(count, upper_bound, s);
  return std::make_tuple(double(res), u64_to_s64(s.s[0]));
}

// полезно будет иметь функцию для получения стака луа, при этом нужно что то безопасное
// вообще сорс потребуется в будущем подменить
// желательно понять где функция находится в каком файле
// но у меня скорее всего будет загрузка не по файлу а по тексту скрипта
static sol::table script_stack(sol::this_state s) {
  const auto folder = utils::project_folder();

  lua_Debug info;
  int level = 0;
  sol::table t = sol::state_view(s).create_table();
  while (lua_getstack(s.L, level, &info)) {
    lua_getinfo(s.L, "nSl", &info);
    sol::table tbl = t[DEVILS_ENGINE_TO_LUA_INDEX(level)].get_or_create<sol::table>();
    tbl["level"] = level;
    auto source_str = std::string(info.source, info.srclen);
    const size_t start = source_str.find(folder);
    if (start != std::string::npos) {
      if (start == 0) {
        source_str = source_str.substr(folder.size());
      } else {
        source_str = source_str.substr(0, start) + source_str.substr(start + folder.size());
      }
    }
    tbl["source"] = source_str;
    tbl["currentline"] = info.currentline;
    const char *name = info.name ? info.name : "<unknown>";
    tbl["name"] = std::string(name);
    tbl["what"] = std::string(info.what);

    ++level;
  }

  return t;
}

// еще дополнительно желательно вылетать если у нас случайно произошел бесконечный цикл
// как сделать?

void basic_functions(sol::table t) {
  sol::table base = t.get_or("base", sol::nil);
  if (!base.valid()) base = t.create_named("base");
  base.set_function("pack_u32f32", &pack_u32f32);
  base.set_function("pack_u32u32", &pack_u32u32); 
  base.set_function("pack_f32f32", &pack_f32f32);
  base.set_function("unpack_u32f32", &unpack_u32f32);
  base.set_function("unpack_u32u32", &unpack_u32u32);
  base.set_function("unpack_f32f32", &unpack_f32f32);
  base.set_function("prng64", &prng64);
  base.set_function("prng64_2", &prng64_2);
  base.set_function("prng64_normalize", &prng64_normalize);
  base.set_function("interval", &interval);
  base.set_function("dice", &dice);
  base.set_function("prng32", &shared::prng);
  base.set_function("prng32_2", &shared::prng2);
  base.set_function("prng32_normalize", &shared::prng_normalize);
  base.set_function("create_array", &create_array);
  base.set_function("create_table", &create_table);
  base.set_function("num_queue", &num_queue);
  base.set_function("queue", &queue);
  base.set_function("num_random_queue", &num_random_queue);
  base.set_function("random_queue", &random_queue);
  base.set_function("perf", &perf);
  base.set_function("script_stack", &script_stack);
  base.set("platform", platform());
  base.set("project", project());

  // тут нужно еще добавить функции: найти и загрузить ресурс, найти список ресурсов, 
  // какой ресурс загружен, какой модуль загружен, список загруженный модулей,
  // 
}
}
}

void sol_lua_check_error(sol::this_state s, const sol::function_result &res) {
  if (res.status() == sol::call_status::ok) return;
  sol::error err = res;
  //utils::error("{}\n", err.what());
  devils_engine::utils::println(err.what());
  luaL_error(s, "Catched lua error");
}

void sol_lua_check_error(const sol::function_result &res) {
  if (res.status() == sol::call_status::ok) return;
  sol::error err = res;
  devils_engine::utils::error("{}\n", err.what());
}

void sol_lua_warn_error(const sol::function_result &res) {
  if (res.status() == sol::call_status::ok) return;
  sol::error err = res;
  devils_engine::utils::warn("{}\n", err.what());
}