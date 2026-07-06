#ifndef DEVILS_ENGINE_CATALOGUE_INTROSPECTION_H
#define DEVILS_ENGINE_CATALOGUE_INTROSPECTION_H

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtl/phmap.hpp>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/string_id.h"
#include "devils_engine/utils/type_traits.h"
#include "devils_engine/catalogue/logging.h" // лог-домены/уровни: trace-уровень домена авто-эскалирует интроспекцию

namespace devils_engine {
namespace catalogue {

struct argument_view {
  std::string_view name;
  std::string_view type;
  std::string_view value;
  bool printable = false;
};

struct call_info {
  utils::id domain = utils::invalid_id;
  utils::id function = utils::invalid_id;
  std::string_view function_name;
  std::string_view return_type;
  std::string_view file;
  uint32_t line = 0;
  std::span<const argument_view> arguments;
};

// Режим интроспекции — задаёт, СКОЛЬКО работы делать на вызов. Заменяет виртуальный
// introspection_interface: разное поведение — невиртуальный switch (см. detail::run/emit_*),
// а тяжёлые arg_views строятся ЛЕНИВО только для dump.
enum class introspection_mode : uint8_t {
  off,        // ничего (быстрый путь, как без интроспекции)
  logging,    // на выходе: имя функции + elapsed (без call_info-args, без file:line)
  statistics, // function_id + elapsed → statistics_store (без вывода, без args)
  tracing,    // enter/exit + file:line + elapsed (без args)
  dump        // ВСЁ: enter/exit + file:line + аргументы (дорого, редко)
};

// Накопитель статистики ПО ФУНКЦИИ (ключ = function_id): агрегаты за всё время
// (count/total/min/max/last) + кольцо последних N замеров (окно — в конструкторе) для графика.
// Один поток (домен вызывается с одного потока-актора). Раньше был statistics_introspection
// (виртуальный) — теперь просто хранилище, в которое пишет режим statistics.
class statistics_store {
public:
  struct function_record {
    utils::id function = utils::invalid_id;
    std::string_view name;
    std::string_view file; // место последнего вызова (для «прыжка к источнику»)
    uint32_t line = 0;
    uint64_t call_count = 0;
    uint64_t total_mcs = 0;
    uint64_t min_mcs = std::numeric_limits<uint64_t>::max();
    uint64_t max_mcs = 0;
    uint64_t last_mcs = 0;
    std::vector<uint64_t> samples; // кольцо; cursor — следующая позиция, filled — сколько реально лежит
    size_t cursor = 0;
    size_t filled = 0;

    double average_mcs() const noexcept { return call_count != 0 ? double(total_mcs) / double(call_count) : 0.0; }
    double recent_average_mcs() const noexcept;                 // среднее по кольцу
    void ordered_samples(std::vector<uint64_t>& out) const;     // последние filled в хроно-порядке (для графика)
  };

  explicit statistics_store(size_t window = 128) noexcept;
  void record(utils::id function, std::string_view name, std::string_view file, uint32_t line, uint64_t elapsed_mcs);

  const function_record* find(utils::id function) const noexcept;
  double average_mcs(utils::id function) const noexcept;
  size_t function_count() const noexcept { return records_.size(); }
  size_t count() const noexcept { return total_calls_; }
  size_t window() const noexcept { return window_; }
  void reset() noexcept;

  template <typename F>
  void for_each(F&& f) const { for (const auto& [id, rec] : records_) f(rec); }

private:
  size_t window_;
  size_t total_calls_ = 0;
  gtl::flat_hash_map<utils::id, function_record> records_;
};

// Конфиг интроспекции домена: режим + лог-домен (префикс/маршрут для logging/tracing/dump) +
// накопитель (для statistics). Ставится в domain<D>::set_introspection; владеет им вызывающий.
struct introspection {
  introspection_mode mode = introspection_mode::off;
  uint32_t log_domain = 0;
  statistics_store* stats = nullptr;
};

namespace detail {

struct argument_value_buffer {
  std::array<char, 64> chars{};
};

inline std::string_view shrink_placeholder(argument_value_buffer& buffer, const std::string_view type) {
  constexpr std::string_view prefix = "devils_engine::";
  constexpr std::string_view dots = "...";
  constexpr size_t capacity = std::tuple_size_v<decltype(buffer.chars)>;

  const auto write_wrapped = [&] (const std::string_view src) -> std::string_view {
    if (src.size() + 2 > capacity) return {};
    char* out = buffer.chars.data();
    *out++ = '<';
    for (const char c : src) *out++ = c;
    *out++ = '>';
    return std::string_view(buffer.chars.data(), size_t(out - buffer.chars.data()));
  };

  if (const auto full = write_wrapped(type); !full.empty()) return full;

  char* out = buffer.chars.data();
  size_t pos = 0;
  while (pos < type.size()) {
    if (type.substr(pos, prefix.size()) == prefix) {
      pos += prefix.size();
      continue;
    }

    if (out == buffer.chars.data() + capacity) break;
    *out++ = type[pos++];
  }

  const size_t without_engine_size = size_t(out - buffer.chars.data());
  if (without_engine_size + 2 <= capacity) {
    for (size_t i = without_engine_size; i > 0; --i) {
      buffer.chars[i] = buffer.chars[i - 1];
    }
    buffer.chars[0] = '<';
    buffer.chars[without_engine_size + 1] = '>';
    return std::string_view(buffer.chars.data(), without_engine_size + 2);
  }

  constexpr size_t payload_capacity = capacity - 2;
  constexpr size_t prefix_capacity = payload_capacity - dots.size();
  const size_t copy_count = std::min(without_engine_size, prefix_capacity);
  for (size_t i = copy_count; i > 0; --i) {
    buffer.chars[i] = buffer.chars[i - 1];
  }
  out = buffer.chars.data();
  *out++ = '<';
  out += copy_count;
  for (const char c : dots) *out++ = c;
  *out++ = '>';
  return std::string_view(buffer.chars.data(), size_t(out - buffer.chars.data()));
}

template <typename T>
std::string_view buffered_number(argument_value_buffer& buffer, const T value, bool& printable) {
  const auto res = std::to_chars(buffer.chars.data(), buffer.chars.data() + buffer.chars.size(), value);
  if (res.ec != std::errc{}) {
    printable = false;
    return {};
  }

  printable = true;
  return std::string_view(buffer.chars.data(), size_t(res.ptr - buffer.chars.data()));
}

template <typename T>
std::string_view stringify_arg(argument_value_buffer& buffer, const T& value, bool& printable) {
  using U = std::remove_cvref_t<T>;
  printable = true;

  if constexpr (std::is_same_v<U, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, char>) {
    return buffered_number(buffer, value, printable);
  } else if constexpr (std::is_floating_point_v<U>) {
    return buffered_number(buffer, value, printable);
  } else if constexpr (std::is_enum_v<U>) {
    return buffered_number(buffer, static_cast<std::underlying_type_t<U>>(value), printable);
  } else if constexpr (std::is_same_v<U, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<U, std::string_view>) {
    return value;
  } else if constexpr (std::is_convertible_v<T, std::string_view>) {
    return std::string_view(value);
  } else {
    printable = false;
    return shrink_placeholder(buffer, utils::type_name<U>());
  }
}

// Эффективный режим = базовый (in.mode) ЭСКАЛИРОВАННЫЙ лог-уровнем домена: если лог-домен
// на trace → минимум tracing (авто-связка логгирования и трассировки функций); dump (выше)
// сохраняется. Так `app.set_log_level("gameplay","trace")` включает трассировку функций домена
// поверх perf-статистики. Гейт — relaxed atomic load (near-zero cost).
inline introspection_mode effective_mode(const introspection& in) noexcept {
  const uint8_t base = static_cast<uint8_t>(in.mode);
  const uint8_t floor = logs().enabled(in.log_domain, log_depth::trace) ? static_cast<uint8_t>(introspection_mode::tracing) : 0;
  return static_cast<introspection_mode>(std::max(base, floor));
}

// emit по режиму (в .cpp): вход/выход. Невиртуальный switch вместо виртуалок. Для не-dump
// info.arguments пуст (arg_views не строятся — см. fn_traits ниже).
void emit_enter(const introspection& in, introspection_mode mode, const call_info& info);
void emit_exit(const introspection& in, introspection_mode mode, const call_info& info, uint64_t elapsed_mcs);

// Обёртка вызова: enter (no-op кроме tracing/dump) → таймер → invoke → exit (switch по режиму).
template <typename Ret, typename Invoke>
Ret run(const introspection& in, const introspection_mode mode, const call_info& info, Invoke&& invoke) {
  using clock = std::chrono::steady_clock;
  emit_enter(in, mode, info);
  const auto start = clock::now();
  if constexpr (std::is_void_v<Ret>) {
    std::forward<Invoke>(invoke)();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count();
    emit_exit(in, mode, info, static_cast<uint64_t>(elapsed));
    return;
  } else {
    Ret ret = std::forward<Invoke>(invoke)();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count();
    emit_exit(in, mode, info, static_cast<uint64_t>(elapsed));
    return ret;
  }
}

template <typename T>
struct callable_traits : utils::detail::function_traits_v2<T> {};

template <typename T>
  requires requires { &T::operator(); }
struct callable_traits<T> : utils::detail::function_traits_v2<decltype(&T::operator())> {};

template <typename T>
using member_object_t = std::conditional_t<callable_traits<T>::is_const, const typename callable_traits<T>::member_of&, typename callable_traits<T>::member_of&>;

}

template <auto Domain>
struct domain {
  inline static const introspection* intro_i = nullptr;

  static void set_introspection(const introspection* ptr) noexcept {
    intro_i = ptr;
  }

  static const introspection* introspection_config() noexcept {
    return intro_i;
  }

  template <auto Fn, utils::template_string_t Name, utils::template_string_t... ArgNames>
  struct fn_traits {
    using fn_t = decltype(Fn);
    using traits = detail::callable_traits<fn_t>;
    using return_t = typename traits::result_type;
    using class_t = typename traits::member_of;

    static constexpr std::string_view name = Name.sv();
    static constexpr utils::id function_id = utils::murmur_hash64A(name);
    static constexpr utils::id domain_id = static_cast<utils::id>(Domain);
    static constexpr size_t argument_count = traits::argument_count;
    static constexpr std::array<std::string_view, sizeof...(ArgNames)> argument_names{ArgNames.sv()...};

    template <size_t N>
    struct argument_pack {
      std::array<argument_view, N> views{};
      std::array<detail::argument_value_buffer, N> buffers{};
    };

    template <size_t I>
    static consteval std::string_view argument_name() {
      if constexpr (I < argument_names.size()) return argument_names[I];
      else return {};
    }

    template <size_t I, typename T, size_t N>
    static void make_argument_view(argument_pack<N>& pack, const T& value) {
      bool printable = false;
      const std::string_view str = detail::stringify_arg(pack.buffers[I], value, printable);
      pack.views[I] = argument_view{
        argument_name<I>(),
        utils::type_name<std::remove_cvref_t<T>>(),
        str,
        printable
      };
    }

    template <typename... Args, size_t... I>
    static auto make_argument_views_impl(std::index_sequence<I...>, const Args&... args) {
      argument_pack<sizeof...(Args)> pack{};
      (make_argument_view<I>(pack, args), ...);
      return pack;
    }

    template <typename... Args>
    static auto make_argument_views(const Args&... args) {
      return make_argument_views_impl<Args...>(std::index_sequence_for<Args...>{}, args...);
    }

    template <typename... Args>
    static return_t invoke_free(Args&&... args) {
      if constexpr (std::is_void_v<return_t>) {
        std::invoke(Fn, std::forward<Args>(args)...);
        return;
      } else {
        return std::invoke(Fn, std::forward<Args>(args)...);
      }
    }

    template <typename Obj, typename... Args>
    static return_t invoke_member(Obj&& obj, Args&&... args) {
      if constexpr (std::is_void_v<return_t>) {
        std::invoke(Fn, std::forward<Obj>(obj), std::forward<Args>(args)...);
        return;
      } else {
        return std::invoke(Fn, std::forward<Obj>(obj), std::forward<Args>(args)...);
      }
    }

    template <typename... Args>
    static return_t invoke_functor(Args&&... args) {
      if constexpr (std::is_void_v<return_t>) {
        std::invoke(Fn, std::forward<Args>(args)...);
        return;
      } else {
        return std::invoke(Fn, std::forward<Args>(args)...);
      }
    }

    template <typename... Args>
    static return_t call_free(Args&&... args) {
      return call_free_at(std::source_location{}, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static return_t call_free_at(const std::source_location loc, Args&&... args) {
      const introspection* in = intro_i;
      if (in == nullptr) return invoke_free(std::forward<Args>(args)...);
      const introspection_mode mode = detail::effective_mode(*in);
      if (mode == introspection_mode::off) return invoke_free(std::forward<Args>(args)...);
      auto invoke = [&]() -> return_t { return invoke_free(std::forward<Args>(args)...); };
      // arg_views строим ЛЕНИВО только для dump (дорогой stringify); остальным режимам — пустой span.
      if (mode == introspection_mode::dump) {
        auto arg_views = make_argument_views(args...);
        const call_info info{ domain_id, function_id, name, utils::type_name<return_t>(), loc.file_name(), loc.line(),
          std::span<const argument_view>(arg_views.views.data(), arg_views.views.size()) };
        return detail::run<return_t>(*in, mode, info, invoke);
      }
      const call_info info{ domain_id, function_id, name, utils::type_name<return_t>(), loc.file_name(), loc.line(), {} };
      return detail::run<return_t>(*in, mode, info, invoke);
    }

    template <typename Obj, typename... Args>
    static return_t call_member(Obj&& obj, Args&&... args) {
      return call_member_at(std::source_location{}, std::forward<Obj>(obj), std::forward<Args>(args)...);
    }

    template <typename Obj, typename... Args>
    static return_t call_member_at(const std::source_location loc, Obj&& obj, Args&&... args) {
      const introspection* in = intro_i;
      if (in == nullptr) return invoke_member(std::forward<Obj>(obj), std::forward<Args>(args)...);
      const introspection_mode mode = detail::effective_mode(*in);
      if (mode == introspection_mode::off) return invoke_member(std::forward<Obj>(obj), std::forward<Args>(args)...);
      auto invoke = [&]() -> return_t { return invoke_member(std::forward<Obj>(obj), std::forward<Args>(args)...); };
      if (mode == introspection_mode::dump) {
        auto arg_views = make_argument_views(obj, args...);
        const call_info info{ domain_id, function_id, name, utils::type_name<return_t>(), loc.file_name(), loc.line(),
          std::span<const argument_view>(arg_views.views.data(), arg_views.views.size()) };
        return detail::run<return_t>(*in, mode, info, invoke);
      }
      const call_info info{ domain_id, function_id, name, utils::type_name<return_t>(), loc.file_name(), loc.line(), {} };
      return detail::run<return_t>(*in, mode, info, invoke);
    }

    template <typename... Args>
    static return_t call_functor(Args&&... args) {
      return call_functor_at(std::source_location{}, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static return_t call_functor_at(const std::source_location loc, Args&&... args) {
      const introspection* in = intro_i;
      if (in == nullptr) return invoke_functor(std::forward<Args>(args)...);
      const introspection_mode mode = detail::effective_mode(*in);
      if (mode == introspection_mode::off) return invoke_functor(std::forward<Args>(args)...);
      auto invoke = [&]() -> return_t { return invoke_functor(std::forward<Args>(args)...); };
      if (mode == introspection_mode::dump) {
        auto arg_views = make_argument_views(args...);
        const call_info info{ domain_id, function_id, name, utils::type_name<return_t>(), loc.file_name(), loc.line(),
          std::span<const argument_view>(arg_views.views.data(), arg_views.views.size()) };
        return detail::run<return_t>(*in, mode, info, invoke);
      }
      const call_info info{ domain_id, function_id, name, utils::type_name<return_t>(), loc.file_name(), loc.line(), {} };
      return detail::run<return_t>(*in, mode, info, invoke);
    }

    template <typename T, typename = void>
    struct maker;

    template <typename Ret, typename... Args>
    struct maker<Ret (*)(Args...)> {
      using pointer_t = Ret (*)(Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename... CallArgs>
        Ret operator()(CallArgs&&... args) const {
          return call_free_at(loc, std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(Args... args) {
        return call_free(std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename... Args>
    struct maker<Ret (*)(Args...) noexcept> {
      using pointer_t = Ret (*)(Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename... CallArgs>
        Ret operator()(CallArgs&&... args) const {
          return call_free_at(loc, std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(Args... args) {
        return call_free(std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...)> {
      using pointer_t = Ret (*)(C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) noexcept> {
      using pointer_t = Ret (*)(C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) const> {
      using pointer_t = Ret (*)(const C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(const C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename Ret, typename C, typename... Args>
    struct maker<Ret (C::*)(Args...) const noexcept> {
      using pointer_t = Ret (*)(const C&, Args...);
      struct loc_fn_t {
        std::source_location loc;

        explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

        template <typename Obj, typename... CallArgs>
        Ret operator()(Obj&& obj, CallArgs&&... args) const {
          return call_member_at(loc, std::forward<Obj>(obj), std::forward<CallArgs>(args)...);
        }
      };

      static Ret call(const C& obj, Args... args) {
        return call_member(obj, std::forward<Args>(args)...);
      }
    };

    template <typename T>
      requires (!std::is_pointer_v<T> && !std::is_member_function_pointer_v<T> && requires { &T::operator(); })
    struct maker<T> {
      template <typename Sig>
      struct impl;

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) const> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) const noexcept> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...)> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      template <typename Ret, typename C, typename... Args>
      struct impl<Ret (C::*)(Args...) noexcept> {
        using pointer_t = Ret (*)(Args...);
        struct loc_fn_t {
          std::source_location loc;

          explicit constexpr loc_fn_t(const std::source_location l = std::source_location::current()) noexcept : loc(l) {}

          template <typename... CallArgs>
          Ret operator()(CallArgs&&... args) const {
            return call_functor_at(loc, std::forward<CallArgs>(args)...);
          }
        };
      };

      using pointer_t = typename impl<decltype(&T::operator())>::pointer_t;
      using loc_fn_t = typename impl<decltype(&T::operator())>::loc_fn_t;

      template <typename... Args>
      static return_t call(Args... args) {
        return call_functor(std::forward<Args>(args)...);
      }
    };

    using pointer_t = typename maker<fn_t>::pointer_t;
    using loc_fn_t = typename maker<fn_t>::loc_fn_t;
    static constexpr pointer_t fn_ptr = &maker<fn_t>::call;
  };
};

}
}

#endif
