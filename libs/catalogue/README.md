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
  catalogue::domain<trace_domain::gameplay>::fn_traits<
    &add_gold,
    "add_gold",
    "amount",
    "multiplier"
  >;

constexpr auto add_gold_fn = add_gold_wrap::fn_ptr;

catalogue::trace_introspection trace;
catalogue::domain<trace_domain::gameplay>::set_introspection(&trace);

add_gold_fn(10, 2);
```

`add_gold_fn` имеет тип:

```cpp
int (*)(int, int)
```

То есть это настоящий указатель на функцию с конкретными аргументами, а не
универсальный `Args...`-call wrapper. Его можно дальше передавать в шаблоны,
которые ожидают нормальную функцию с нормальной сигнатурой.

## domain / fn_traits

`domain<Domain>` задает область трассировки. `Domain` - это `auto` template
parameter, поэтому можно использовать и enum, и простой `constexpr size_t`.

```cpp
template <auto Domain>
struct domain;
```

Пример с числовыми доменами:

```cpp
namespace domains {
constexpr size_t gameplay = 1;
constexpr size_t service = 2;
}

using wrapped = catalogue::domain<domains::gameplay>::fn_traits<&f, "f", "a">;
```

Пример с enum:

```cpp
enum class domains : uint64_t {
  gameplay = 1,
  service = 2
};

using wrapped = catalogue::domain<domains::gameplay>::fn_traits<&f, "f", "a">;
```

У каждого `domain<Domain>` есть свой runtime pointer:

```cpp
domain<Domain>::set_introspection(ptr);
domain<Domain>::introspection();
```

Это позволяет включать разные политики для разных областей:

- gameplay effects;
- UI host API;
- service/debug functions;
- resource loading;
- sound commands.

`fn_traits<Fn, Name, ArgNames...>` описывает конкретную функцию внутри домена:

- `Fn` - функция, метод или structural functor;
- `Name` - имя функции для логов и id;
- `ArgNames...` - имена аргументов для debug output.

Публичные поля:

- `fn_ptr` - constexpr указатель на обернутую функцию;
- `loc_fn_t` - маленький functor, который захватывает `std::source_location` в
  конструкторе и вызывает функцию через forwarding `operator()`;
- `function_id` - `utils::murmur_hash64A(Name)`, compile-time id;
- `domain_id` - id домена;
- `argument_count` - число аргументов оригинального вызова.
- `argument_names` - `std::array<std::string_view, ...>` с именами аргументов,
  заданными в `ArgNames...`.

Важно: `fn_ptr` и `function_id` constexpr. Для function id здесь используется
compile-time murmur64 из `utils/type_traits.h`, а не runtime `utils::string_hash`.

Если `domain<Domain>::set_introspection(nullptr)`, wrapper не собирает
`call_info`, не форматирует аргументы и вызывает оригинальную функцию напрямую.
Для вызывающего кода это должно вести себя как обычный function pointer call.

Если нужно получить файл и строку места вызова, используй `loc_fn_t`:

```cpp
using update_fn_t = wrapped::loc_fn_t;
update_fn_t{}(arg1, arg2);
```

Для методов объект передается первым аргументом:

```cpp
using tick_fn_t = catalogue::domain<domain::gameplay>::fn_traits<&actor::tick, "actor.tick", "self", "dt">::loc_fn_t;
tick_fn_t{}(actor_ref, dt);
```

## Что Поддерживается

Свободные функции:

```cpp
int f(int, float);
using wrapped = catalogue::domain<domain::gameplay>::fn_traits<&f, "f", "a", "b">;
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

using add = catalogue::domain<domain::gameplay>::fn_traits<&wallet::add, "wallet.add", "self", "amount">;
using get = catalogue::domain<domain::gameplay>::fn_traits<&wallet::get, "wallet.get", "self">;

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

using wrapped = catalogue::domain<domain::service>::fn_traits<mul, "multiply", "a", "b">;
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
- `file`/`line` - место вызова, если использован `loc_fn_t`; пусто/0 для
  обычного `fn_ptr`;
- `arguments`.

`arguments` - это `std::span<const argument_view>`.

Lifetime важен: span валиден только внутри текущего синхронного вызова
`enter()`/`exit()`/`skipped()`. Если introspection implementation хочет хранить
аргументы дольше, она должна скопировать нужные строки/значения.

`argument_view` содержит:

- имя аргумента;
- имя типа;
- строковое значение как `std::string_view`;
- флаг `printable`.

Для чисел `catalogue` использует локальные fixed buffers и `std::to_chars`.
Для строк `value` смотрит на исходный `std::string`/`std::string_view`.
Отдельная `std::string` на каждый аргумент не создается; если значение нужно
хранить после callback, introspection implementation должна скопировать его сама.

В строку сейчас превращаются только простые типы:

- `bool`;
- integral, кроме `char`;
- floating point;
- enum как underlying integer;
- `std::string`;
- `std::string_view`;
- типы, конвертируемые в `std::string_view`.

Структуры, ресурсы, сложные объекты и ссылки на доменные сущности не
разворачиваются. Они попадают в лог как заглушка вида `<имя_типа>`. Заглушка
пишется в тот же локальный fixed buffer: если полное имя не помещается, сначала
убираются вхождения `devils_engine::`, затем имя усекается с `...`. Это
намеренно: `catalogue` не должен в первом срезе превращаться в общий serializer.

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
catalogue::domain<domain::gameplay>::set_introspection(&stats);

// ...

const double avg = stats.average_mcs(add_gold_wrap::function_id);
```

## Текущие Ограничения

- `domain<Domain>::intro_i` - raw pointer без ownership. Владелец обязан
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

## Техдолг

- Добавить registry функций внутри домена по `function_id`, чтобы можно было
  перечислять доступные wrapped-функции, искать метаданные по id и позже
  использовать это для debug UI/RPC/replay.

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
