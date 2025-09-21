#ifndef DEVILS_ENGINE_CATALOGUE_CORE_H
#define DEVILS_ENGINE_CATALOGUE_CORE_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string_view>
#include <string>
#include <span>
#include <array>
#include <gtl/phmap.hpp>
#include "devils_engine/utils/type_traits.h"
#include "zpp_bits.h"

// какая цель этой штуки? создать список важных функций определяющих геймплей
// и записать их в бинарный формат в массив байт
// затем считать этот массив и полностью восстановить последовательность операций в движке
// эта система должна поддерживать tick и записывать данные кусками
// я бы хотел чтобы система записывала данные максимально упакованно (используя varint)
// система по идее не должна никак больше обрабатывать получаемый набор байт
// сжатие и шифровка трафика должна будет происходить на других этапах
// эта система мне нужна в каких случаях:
// 1. запись и прогон демо
// 2. запись снапшота tick и потом передача его по сети на сервер
// 3. часть информации отсюда пойдет в сохранение
// 4. система участвует в rollback вместе с переодическими снапшотами всего состояния
// 5. debug + способ посчитать fps как в кваке1
// 6. можно легко поднять статистику по вызовам функций (по крайней мере те что зарегистрированы)
// 7. отложенное выполнение какого то участка
// 8. сохранить в файл какой то сценарий? (чатжпт подсказывает что например генерация уровня)
// 9. общее журналирование?

// 6 пунктов довольно полезных + 7ой более менее
// какая задача? нужно сделать так чтобы я мог подменять то куда записываются функции.......
// так еще раз: мне по большому счету нужно 2 вещи: сколько записывать в целом и куда....
// нет, конкретно логгирование вообще не должно понимать куда и сколько оно записывает
// оно должно просто записывать
// + к этому должна быть система которая принимает данные 
// их может быть несколько например нетворкинг будет укладывать 30 последних тиков
// запись демо уложит все что записано в файл
// сохранение хотело бы хранить несколько тиков от полного снапшота
// как минимум будет один приемник, даже не так
// у нас есть tick игры, мы бы хотели записать все вызовы функций внутри тика
// и вот это и есть как будто конечные данные которые потребляют все остальные системы

// с демкой небольшие накладки - единственная часть которую сложно воспроизвести без игрока
// это инпут, все остальные функции воспроизводятся легко если есть инпут от игрока
// вообще возможно функции инпута это как раз то что я бы хотел записать 
// остальные функции изменяющие стейт мне особо не нужны...
// 

namespace devils_engine {
namespace catalogue {

// тут наверное zpp out ?
inline void default_invoke(const std::span<uint8_t> &) {}

struct registry {
  struct info {
    using invoke_fn = decltype(&default_invoke);

    size_t id;
    std::string_view name;
    const invoke_fn fn;

    friend bool operator==(const info& a, const info& b) noexcept { return a.name == b.name; }
  };

  gtl::flat_hash_map<size_t, info> funcs;

  void reg(const size_t id, const std::string_view &name, const info::invoke_fn fn);
};

struct tick_buffer_header {
  uint32_t tick;
  uint32_t checksum;
  uint32_t size;
};

struct function_buffer_header {
  uint32_t tick;
  uint32_t id;     // no collision? under ~250 function names uint32 is ok
  uint32_t offset; // enough? packet more than 2^32 bytes?
};

struct buffer {
  std::vector<function_buffer_header> headers;
  std::vector<uint8_t> payload;
};

class consumer {
public:
  virtual ~consumer() noexcept = default;
  virtual void consume(const buffer&) = 0;
};

// так теперь я разобрался и мне реально нужен инструмент который позволит записывать данные в разные каналы
// то есть нужно будет задать видимо структуру в темплейт в которой будет: регистрация, буфер и консумер
// вообще прямо сверх необходимости в том чтобы указать тут консумера нет
template <size_t id> // make unique
struct core {
  static constexpr size_t max_consumer_count = 8;

  static struct registry* registry;
  static struct buffer buffer;
  static std::array<consumer*, max_consumer_count> consumers;
};

template <typename Core_T, utils::template_string_t Name, auto f>
struct rpc_function {
  using channel_t = Core_T;
  //using traits = utils::detail::function_traits_v2<decltype(f)>;
  using fn_t = decltype(f);
  using read_fn_t = registry::info::invoke_fn;
  using reg_fn_t = void(*)();
  static constexpr auto name = Name.sv();
  static constexpr uint32_t id = utils::murmur_hash3_32(name);

  template <auto fn>
  struct internal;

  template <typename... Args, void(*fn)(Args...)>
  struct internal<fn> {
    static void read(const std::span<uint8_t>&) {

    }

    static void write(Args... args) {
      // возьмем core и туда запишем id функции и все аргументы
      // нет, id наверное только запишем в function_buffer_header
      channel_t::buffer.headers.push_back({ 1, id, channel_t::buffer.payload.size() });
      zpp::bits::size_varint{};
    }

    static void call(Args... args) {
      std::cout << "[RPC free] " << name << "\n";
      return std::invoke(fn, std::forward<Args>(args)...);
    }

    static void log(Args... args) {
      call(std::forward<Args>(args)...);
      write(std::forward<Args>(args)...);
    }

    static void reg() {
      channel_t::registry->reg(id, name, &read);
    }
  };

  using detail_t = internal<f>;

  static constexpr read_fn_t read = &detail_t::read;
  static constexpr fn_t write = &detail_t::write;
  static constexpr fn_t call = &detail_t::call;
  static constexpr fn_t log = &detail_t::log;
  static constexpr reg_fn_t reg = &detail_t::reg;
};

void add_gold(uint32_t entityid, int gold) {}
void send_message(uint32_t playerid, const std::string_view &msg) {}

using add_gold_fn_t = rpc_function<core<0>, "add_gold", &add_gold>;
using add_gold1_fn_t = rpc_function<core<1>, "add_gold", &add_gold>;
using send_message_rpc_t = rpc_function<core<1>, "send_message", &send_message>;
constexpr auto id1 = add_gold_fn_t::id;
constexpr auto id2 = send_message_rpc_t::id;
static_assert("add_gold" == add_gold_fn_t::name);

inline void reg_fns() {
  add_gold_fn_t::reg();
}
}
}

#endif