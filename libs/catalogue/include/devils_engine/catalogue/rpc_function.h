#ifndef DEVILS_ENGINE_CATALOGUE_RPC_FUNCTION_H
#define DEVILS_ENGINE_CATALOGUE_RPC_FUNCTION_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include "common.h"
#include "channel_data.h"
#include "devils_engine/utils/type_traits.h"
#include "zpp_bits.h"

namespace devils_engine {
namespace catalogue {
template <typename Core_T, utils::template_string_t Name, auto f>
struct rpc_function {
  using channel_t = Core_T;
  using fn_t = decltype(f);
  using read_fn_t = registry::info::invoke_fn;
  using reg_fn_t = void(*)();
  static constexpr auto name = Name.sv();
  static constexpr uint32_t id = utils::murmur_hash3_32(name);

  template <auto fn>
  struct internal;

  template <typename... Args, void(*fn)(Args...)>
  struct internal<fn> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write(Args... args);
    static void call(Args... args);
    static void log(Args... args);
    static void reg();
  };

  template <typename... Args, void(*fn)(Args...) noexcept>
  struct internal<fn> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write(Args... args) noexcept;
    static void call(Args... args) noexcept;
    static void log(Args... args) noexcept;
    static void reg();
  };

  template <void(*fn)()>
  struct internal<fn> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write();
    static void call();
    static void log();
    static void reg();
  };

  template <void(*fn)() noexcept>
  struct internal<fn> {
    static void read(const function_buffer_header& header, const std::span<uint8_t>&);
    static void write() noexcept;
    static void call() noexcept;
    static void log() noexcept;
    static void reg();
  };

  using detail_t = internal<f>;

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


template <typename Core_T, utils::template_string_t Name, auto f>
template <typename... Args, void(*fn)(Args...)>
void rpc_function<Core_T, Name, f>::internal<fn>::read(const function_buffer_header& header, const std::span<uint8_t>& data) {
  // куда то нужно еще присунуть тик??? вообще что на счет тупо запихать тик в качестве входных данных?
  auto in = zpp::bits::in(data, zpp::bits::options::endian::network{});
  std::invoke(fn, detail::read_bytes<Args>(in)...);
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