# catalogue

`libs/catalogue` сейчас разворачивается в utility-слой для трассировки и
интроспекции вызовов функций.

Главная идея: в коде должна остаться обычная функция с обычной сигнатурой, но
вызов проходит через маленькую constexpr-обертку. Эта обертка знает имя функции,
имена аргументов, типы, домен трассировки и может передать эти данные в
runtime-подключаемый `introspection_interface`.

Старые идеи про binary buffer, replay и RPC остаются в проекте как legacy
prototype. Они не являются текущим основным направлением. Сначала библиотека
должна стать удобным инструментом для:

- трассировки входа/выхода из выбранных функций;
- debug-замера времени выполнения;
- dry-run режима, где вызов описывается, но оригинальная функция не исполняется;
- сбора статистики по последним вызовам.

RPC/serialization стоит вернуться позже, когда станет ясно, какие именно вызовы
и данные реально нужно переносить между процессами или писать в replay.

## Новый Introspection API

Основной заголовок:

```cpp
#include "devils_engine/catalogue/introspection.h"
```

Минимальный пример:

```cpp
enum class trace_domain : uint64_t {
  gameplay = 1,
  ui = 2
};

int add_gold(int amount, int multiplier);

using add_gold_wrap =
  catalogue::outer<trace_domain::gameplay>::inner<
    &add_gold,
    "add_gold",
    "amount",
    "multiplier"
  >;

constexpr auto add_gold_fn = add_gold_wrap::fn_ptr;

catalogue::trace_introspection trace;
catalogue::outer<trace_domain::gameplay>::set_introspection(&trace);

add_gold_fn(10, 2);
```

`add_gold_fn` имеет тип:

```cpp
int (*)(int, int)
```

То есть это настоящий указатель на функцию с конкретными аргументами, а не
универсальный `Args...`-call wrapper. Его можно дальше передавать в шаблоны,
которые ожидают нормальную функцию с нормальной сигнатурой.

## outer / inner

`outer<Domain>` задает область трассировки. Обычно `Domain` - это enum:

```cpp
template <auto Domain>
struct outer;
```

У каждого `outer<Domain>` есть свой runtime pointer:

```cpp
outer<Domain>::set_introspection(ptr);
outer<Domain>::introspection();
```

Это позволяет включать разные политики для разных областей:

- gameplay effects;
- UI host API;
- service/debug functions;
- resource loading;
- sound commands.

`inner<Fn, Name, ArgNames...>` описывает конкретную функцию:

- `Fn` - функция, метод или structural functor;
- `Name` - имя функции для логов и id;
- `ArgNames...` - имена аргументов для debug output.

Публичные поля:

- `fn_ptr` - constexpr указатель на обернутую функцию;
- `function_id` - `utils::string_hash(Name)`;
- `domain_id` - id домена;
- `argument_count` - число аргументов оригинального вызова.

Важно: `fn_ptr` constexpr. `function_id` сейчас не constexpr, потому что
использует общий runtime `utils::string_hash`/rapidhash, а compile-time murmur id
в `utils` намеренно отделен от runtime string ids.

## Что Поддерживается

Свободные функции:

```cpp
int f(int, float);
using wrapped = outer<domain::gameplay>::inner<&f, "f", "a", "b">;
constexpr auto ptr = wrapped::fn_ptr; // int (*)(int, float)
```

`noexcept` свободные функции тоже поддерживаются, но обертка пока не сохраняет
`noexcept` в типе итогового указателя.

Методы:

```cpp
struct wallet {
  int add(int amount);
  int get() const;
};

using add = outer<domain::gameplay>::inner<&wallet::add, "wallet.add", "self", "amount">;
using get = outer<domain::gameplay>::inner<&wallet::get, "wallet.get", "self">;

constexpr auto add_ptr = add::fn_ptr; // int (*)(wallet&, int)
constexpr auto get_ptr = get::fn_ptr; // int (*)(const wallet&)
```

Обычный метод получает объект первым аргументом как `T&`.
`const`-метод получает объект как `const T&`.

Structural functor:

```cpp
struct multiply {
  constexpr int operator()(int a, int b) const { return a * b; }
};

constexpr multiply mul{};

using wrapped = outer<domain::service>::inner<mul, "multiply", "a", "b">;
constexpr auto ptr = wrapped::fn_ptr; // int (*)(int, int)
```

Ограничение первого среза: functor должен быть structural NTTP object с обычным,
неперегруженным `operator()`. Generic lambda / overloaded functor пока не цель.

## Introspection Interface

Контракт:

```cpp
class introspection_interface {
public:
  virtual ~introspection_interface() noexcept = default;

  virtual call_decision enter(const call_info& info) = 0;
  virtual void exit(const call_info& info, uint64_t elapsed_mcs) = 0;
  virtual void skipped(const call_info& info) = 0;
};
```

`enter()` вызывается перед оригинальной функцией.

Если он возвращает `call_decision::execute`, функция исполняется, затем
вызывается `exit()` с временем выполнения в микросекундах.

Если он возвращает `call_decision::skip`, оригинальная функция не исполняется,
вызывается `skipped()`, а результатом становится:

- `void` для `void` функций;
- default-constructed value для non-void функций.

Поэтому dry-run non-void функции должны иметь default-constructible return type.

## call_info И Аргументы

`call_info` содержит:

- `domain`;
- `function`;
- `function_name`;
- `return_type`;
- `file`/`line` reserved-поля;
- `arguments`.

`arguments` - это `std::span<const argument_view>`.

Lifetime важен: span валиден только внутри текущего синхронного вызова
`enter()`/`exit()`/`skipped()`. Если introspection implementation хочет хранить
аргументы дольше, она должна скопировать нужные строки/значения.

`argument_view` содержит:

- имя аргумента;
- имя типа;
- строковое значение;
- флаг `printable`.

В строку сейчас превращаются только простые типы:

- `bool`;
- integral, кроме `char`;
- floating point;
- enum как underlying integer;
- `std::string`;
- `std::string_view`;
- типы, конвертируемые в `std::string_view`.

Структуры, ресурсы, сложные объекты и ссылки на доменные сущности не
разворачиваются. Они попадают в лог как `<opaque>`. Это намеренно: `catalogue`
не должен в первом срезе превращаться в общий serializer.

## Готовые Реализации

`trace_introspection`

Пишет через `utils::info`:

- `enter 'name'`;
- `exited 'name', took N mcs`;
- `skipped 'name'`.

`timing_introspection`

Пишет через `utils::info` время выполнения и printable-аргументы:

```text
'add_gold' took 12 mcs (amount=5, multiplier=3)
```

`dry_run_introspection`

Всегда возвращает `call_decision::skip`. Удобен для preview/audit сценариев,
когда нужно увидеть, какой вызов был бы сделан, но не мутировать состояние.

`statistics_introspection<Capacity>`

Хранит rolling buffer последних `Capacity` завершенных вызовов и умеет считать
среднее время по `function_id`:

```cpp
catalogue::statistics_introspection<128> stats;
outer<domain::gameplay>::set_introspection(&stats);

// ...

const double avg = stats.average_mcs(add_gold_wrap::function_id);
```

## Текущие Ограничения

- `outer<Domain>::intro_i` - raw pointer без ownership. Владелец обязан
  гарантировать lifetime introspection object.
- Runtime-переключение introspection pointer пока без синхронизации. Для
  межпоточного hot-swap нужен atomic или внешний lifecycle barrier.
- Исключения пока не обрабатываются отдельно: если оригинальная функция бросит,
  `exit()` не будет вызван.
- `noexcept` оригинальной функции не сохраняется в типе wrapper pointer.
- Аргументы форматируются только для базовых типов; сложные структуры
  deliberately opaque.
- `file`/`line` в `call_info` зарезервированы, но пока не заполняются.
- Старый RPC/buffer код остается рядом и не является стабильным public API.

## Старый RPC / Replay Прототип

В библиотеке все еще есть файлы:

- `common.h`;
- `core.h`;
- `channel_data.h`;
- `registry.h`;
- `rpc_function.h`;
- `demo.h`.

Они описывают ранний подход:

- buffer из headers + payload;
- registry для поиска function reader по id;
- статические channels;
- consumers для demo/debug/network;
- `rpc_function` с `call/write/log/read/reg`.

Эта линия не удалена, потому что часть идей пригодится позже для replay/RPC.
Но текущий фокус другой: сначала нужен надежный seamless wrapper для обычных
функций, методов и простых functor'ов, а уже затем можно решать, какие вызовы
сериализовать и в каком формате.

## Как Об Этом Думать

`catalogue` должен стать маленьким техническим слоем вокруг вызовов:

```text
обычная функция -> constexpr wrapper pointer -> runtime introspection policy
```

Он не должен становиться главным gameplay registry вместо `act`, главным
resource storage вместо `demiurg` или главным netcode. Его полезная роль сейчас:
наблюдать, измерять, временно запрещать и статистически описывать выбранные
вызовы без переписывания вызывающего кода под тяжелый framework.
