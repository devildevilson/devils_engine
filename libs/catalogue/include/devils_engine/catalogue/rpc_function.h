#ifndef DEVILS_ENGINE_CATALOGUE_RPC_FUNCTION_H
#define DEVILS_ENGINE_CATALOGUE_RPC_FUNCTION_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include "common.h"
#include "channel_data.h"
#include "devils_engine/utils/type_traits.h"
#include "devils_engine/utils/hash.h" // murmur_hash3_32
#include "zpp_bits.h"

namespace devils_engine {
namespace catalogue {
struct mutator {
  uint32_t id;
  uint32_t payload_size;
  uint8_t* payload;
};

template <typename Core_T, auto f, utils::template_string_t Name, utils::template_string_t... fargs>
struct rpc_function {
  using channel_t = Core_T;
  using fn_t = decltype(f);
  using read_fn_t = registry::info::invoke_fn;
  using reg_fn_t = void(*)();
  static constexpr auto name = Name.sv();
  static constexpr uint32_t id = utils::murmur_hash3_32(name);
  static constexpr std::array<std::string_view, sizeof...(fargs)> argument_names{ fargs.sv()... };

  template <typename Traits>
  struct internal;

  template <typename Ret, typename... Args>
  struct internal<Ret(*)(Args...)> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write(Args... args);
    static void call(Args... args);
    static void log(Args... args);
    static void reg();
  };

  template <typename Ret, typename... Args>
  struct internal<Ret(*)(Args...) noexcept> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write(Args... args) noexcept;
    static void call(Args... args) noexcept;
    static void log(Args... args) noexcept;
    static void reg();
  };

  using detail_t = internal<fn_t>;

  static constexpr read_fn_t read = &detail_t::read;
  static constexpr fn_t write = &detail_t::write;
  static constexpr fn_t call = &detail_t::call;
  static constexpr fn_t log = &detail_t::log;
  static constexpr reg_fn_t reg = &detail_t::reg;
};

template <typename Core_T, auto f, utils::template_string_t Name>
struct rpc_function {
  using channel_t = Core_T;
  using fn_t = decltype(f);
  using read_fn_t = registry::info::invoke_fn;
  using reg_fn_t = void(*)();
  static constexpr auto name = Name.sv();
  static constexpr uint32_t id = utils::murmur_hash3_32(name);

  template <typename Traits>
  struct internal;

  template <typename Ret>
  struct internal<Ret(*)()> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write();
    static void call();
    static void log();
    static void reg();
  };

  template <typename Ret>
  struct internal<Ret(*)() noexcept> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write() noexcept;
    static void call() noexcept;
    static void log() noexcept;
    static void reg();
  };

  using detail_t = internal<fn_t>;

  static constexpr read_fn_t read = &detail_t::read;
  static constexpr fn_t write = &detail_t::write;
  static constexpr fn_t call = &detail_t::call;
  static constexpr fn_t log = &detail_t::log;
  static constexpr reg_fn_t reg = &detail_t::reg;
};

// теперь от движка ожидаю списка вида
// 1. атомарные мутаторы
// 2. атомарный инпут
// 3. мета функции
// 4. сервис (ack(tick)?)
//inline void add_gold(uint32_t entityid, int gold) {}
//inline void add_exp(uint32_t entityid, int exp) noexcept {}
//inline void spawn_projectile(uint32_t entityid, uint32_t tick, uint32_t type) {}
//inline void send_message(uint32_t playerid, std::string msg) {}
//inline void ack(uint32_t tickid) {}
//inline void move(uint32_t entityid, vec2 dir) {}
// для spawn_projectile во первых потребуется обертка для скрипта
// а во вторых еще надо подумать как именно вызовется это дело
//
//using add_gold_rpc_t = rpc_function<mutator_channed_data, "add_gold", &add_gold>;
//using add_exp_rpc_t = rpc_function<mutator_channed_data, "add_exp", &add_exp>;
//
//
//inline void reg_all() {
//  add_gold_rpc_t::reg();
//  add_exp_rpc_t::reg();
//}

// во многих случаях tick мне в функциях вообще ни к чему
// я должен просто гарантировать что у меня последовательно вызовутся функции тик за тиком
// ....
// функции я по идее должен упаковать в какой то класс, чтобы было проще иметь доступ к контексту
// тупиковый путь?



namespace detail {
  using in_t = zpp::bits::in<const std::span<uint8_t>, zpp::bits::options::endian::network>;
  using out_t = zpp::bits::out<std::span<uint8_t>, zpp::bits::options::endian::network>;

  template <typename T>
  auto read_bytes(in_t& in) -> std::remove_cvref_t<T> {
    using plain_type = std::remove_cvref_t<T>;
    plain_type val;
    in(val);
    return val;
  }
}

// энумы в бинарнике можно хранить как числа, а вот в текстовом виде нужно распаковать
template <size_t ID, typename T>
std::span<const uint8_t> read(const std::span<const uint8_t>& cur_buffer, T& tuple) {
  using cur_t = std::tuple_element_t<ID, T>;

  if constexpr (std::is_arithmetic_v<cur_t> || std::is_enum_v<cur_t>) {
    if (cur_buffer.size() < sizeof(cur_t)) return std::span();

    cur_t val = *reinterpret_cast<cur_t*>(cur_buffer.data());
    std::get<ID>(tuple) = val;
    return std::span(cur_buffer.begin() + sizeof(cur_t), cur_buffer.end());
  }

  if constexpr (std::is_same_v<cur_t, std::string_view>) {
    if (sizeof(uint32_t) > cur_buffer.size()) return std::span();
    uint32_t size = *reinterpret_cast<uint32_t*>(cur_buffer.data());
    if (sizeof(uint32_t) + size > cur_buffer.size()) return std::span();
    auto str = reinterpret_cast<const char*>(&cur_buffer[sizeof(uint32_t)]);
    std::get<ID>(tuple) = std::string_view(str, size);
    return std::span(cur_buffer.begin() + (sizeof(uint32_t) + size), cur_buffer.end());
  }

  return std::span();
}

template <size_t ID, typename T>
std::span<uint8_t> write(const T& tuple, const std::span<uint8_t>& cur_buffer) {
  using cur_t = std::tuple_element_t<ID, T>;

  if constexpr (std::is_arithmetic_v<cur_t> || std::is_enum_v<cur_t>) {
    if (cur_buffer.size() < sizeof(cur_t)) return std::span();

    const auto val = std::get<ID>(tuple);
    std::memcpy(cur_buffer.data(), &val, sizeof(cur_t));
    return std::span(cur_buffer.begin() + sizeof(cur_t), cur_buffer.end());
  }

  // вообще пока в памяти хранится можно как значение стринг_вью скопировать
  // не, лучше пусть так сразу будет
  if constexpr (std::is_same_v<cur_t, std::string_view>) {
    const auto val = std::get<ID>(tuple);
    if (sizeof(uint32_t) + val.size() > cur_buffer.size()) return std::span();
    uint32_t size = val.size();
    std::memcpy(cur_buffer.data(), &size, sizeof(uint32_t));
    std::memcpy(cur_buffer.data() + sizeof(uint32_t), &val.data(), val.size());
    return std::span(cur_buffer.begin() + (sizeof(uint32_t) + size), cur_buffer.end());
  }

  std::tuple<int, float> abc;
  std::span<uint8_t> data;
  utils::static_for<std::tuple_size_v<decltype(abc)>>([&](const auto index) {
    if (data.empty()) return; // ошибка
    data = read<index>(data, abc);
  });

  return std::span();
}

// тут наверное еще tuple и индекс
// даже не еще 
//template <typename>
//std::span<const uint8_t> read<double>(const std::span<const uint8_t>& cur_buffer) {
//  if (cur_buffer.size() < sizeof(double)) return std::span();
//
//
//
//  return std::span(cur_buffer.begin() + sizeof(double), cur_buffer.end());
//}

template <typename Core_T, auto f, utils::template_string_t Name, utils::template_string_t... fargs>
template <typename Ret, typename... Args>
void rpc_function<Core_T, f, Name, fargs...>::internal<Ret(*)(Args...)>::read(const function_buffer_header& header, const std::span<uint8_t>& data) {
  // куда то нужно еще присунуть тик??? вообще что на счет тупо запихать тик в качестве входных данных?
  auto in = zpp::bits::in(data, zpp::bits::options::endian::network{});
  std::invoke(fn, detail::read_bytes<Args>(in)...);

  // так теперь мы никаких тиков никуда не пихаем
  // тут мы только читаем/записываем мутатор
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...)>
void rpc_function<Core_T, Name, f>::internal<fn>::write(Args... args) {
  channel_t::buffer.headers.push_back({ 1, id, channel_t::buffer.payload.size() });
  auto out = zpp::bits::out(channel_t::buffer.payload, zpp::bits::options::endian::network{});
  out(args...);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...)>
void rpc_function<Core_T, Name, f>::internal<fn>::call(Args... args) {
  std::invoke(fn, std::forward<Args>(args)...);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...)>
void rpc_function<Core_T, Name, f>::internal<fn>::log(Args... args) {
  call(std::forward<Args>(args)...);
  write(std::forward<Args>(args)...);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...)>
void rpc_function<Core_T, Name, f>::internal<fn>::reg() {
  channel_t::registry->reg(id, name, &read);
}


template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...) noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::read(const function_buffer_header& header, const std::span<uint8_t>& data) {
  // куда то нужно еще присунуть тик??? вообще что на счет тупо запихать тик в качестве входных данных?
  auto in = zpp::bits::in(data, zpp::bits::options::endian::network{});
  std::invoke(fn, detail::read_bytes<Args>(in)...);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...) noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::write(Args... args) noexcept {
  channel_t::buffer.headers.push_back({ 1, id, channel_t::buffer.payload.size() });
  auto out = zpp::bits::out(channel_t::buffer.payload, zpp::bits::options::endian::network{});
  out(args...);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...) noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::call(Args... args) noexcept {
  std::invoke(fn, std::forward<Args>(args)...);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...) noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::log(Args... args) noexcept {
  call(std::forward<Args>(args)...);
  write(std::forward<Args>(args)...);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...) noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::reg() {
  channel_t::registry->reg(id, name, &read);
}


template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)()>
void rpc_function<Core_T, Name, f>::internal<fn>::read(const function_buffer_header& header, const std::span<uint8_t>& data) {
  // куда то нужно еще присунуть тик??? вообще что на счет тупо запихать тик в качестве входных данных?
  //auto in = zpp::bits::in(data, zpp::bits::options::endian::network{});
  std::invoke(fn);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)()>
void rpc_function<Core_T, Name, f>::internal<fn>::write() {
  channel_t::buffer.headers.push_back({ 1, id, channel_t::buffer.payload.size() });
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)()>
void rpc_function<Core_T, Name, f>::internal<fn>::call() {
  std::invoke(fn);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)()>
void rpc_function<Core_T, Name, f>::internal<fn>::log() {
  call();
  write();
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)()>
void rpc_function<Core_T, Name, f>::internal<fn>::reg() {
  channel_t::registry->reg(id, name, &read);
}


template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)() noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::read(const function_buffer_header& header, const std::span<uint8_t>& data) {
  // куда то нужно еще присунуть тик??? вообще что на счет тупо запихать тик в качестве входных данных?
  std::invoke(fn);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)() noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::write() noexcept {
  channel_t::buffer.headers.push_back({ 1, id, channel_t::buffer.payload.size() });
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)() noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::call() noexcept {
  return std::invoke(fn);
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)() noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::log() noexcept {
  call();
  write();
}

template <typename Core_T, utils::template_string_t Name, auto f>
template <void(*fn)() noexcept>
void rpc_function<Core_T, Name, f>::internal<fn>::reg() {
  channel_t::registry->reg(id, name, &read);
}

}
}

#endif