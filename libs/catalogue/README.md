# catalogue

`libs/catalogue` — маленький utility-слой вокруг обычных C++-функций. Он даёт
трассировку/интроспекцию, старый простой `call_log` и типизированное перенаправление
effect-вызовов в deterministic MT strategies. Collect/elect и первый live ECS pipeline уже работают.

Главная идея: в коде должна остаться обычная функция с обычной сигнатурой, но
вызов проходит через маленькую constexpr-обертку. Эта обертка знает имя функции,
имена аргументов и типы. Независимые runtime-политики решают (а) как наблюдать
вызов и (б) как отложить effect-вызов до нужной pipeline-фазы.

Сейчас в библиотеке два живых слоя:

- **introspection** — constexpr-обёртка над обычной функцией для трассировки
  входа/выхода, debug-замера времени, dry-run и статистики по последним вызовам
  (см. ниже);
- **`call_log`** — generic однослотовый per-source journal; в live tile pipeline он хранит
  только выбранный action для последующего FSM/звука. Typed effect bodies живут в `mt::executor`.
- **`deferred.h`** — same-signature `fn_deferred_ptr`, collect/elect strategies, deterministic
  `record_scope` provenance и phase-driven executors.

Ранний byte-buffer/replay/RPC прототип (`common.h`, `core.{h,cpp}`,
`channel_data.h`, `registry.{h,cpp}`, `rpc_function.h`, `demo.{h,cpp}`) **перенесён
в `exclude/libs/catalogue/`** (2026-07-16): он тянул уже выпиленный из движка
`zpp_bits`, не имел живых потребителей и путал с настоящим направлением.
Этот прототип не является направлением развития библиотеки. Catalogue не владеет
replay, RPC, netcode, on-disk форматом или восстановлением состояния. Если они
понадобятся, это будет отдельный owning/serialization слой с собственными
контрактами; он может наблюдать catalogue-вызовы, но catalogue от него не зависит.

## call_log — контейнер отложенных вызовов

`call_log` (`catalogue/call_log.h`, header-only) делает ровно две вещи: (а)
записать вызов (`call_record{fn, primary, target}`) в слот по индексу инициатора
и (б) исполнить контейнер позже (`dispatch(fn)` в порядке возрастания индекса).
Это локальный in-memory обход, а не replay-подсистема.

- **Плотный per-index store** ⇒ MT-запись без локов (разные инициаторы — разные
  слоты, инвариант map-фазы) + детерминированный обход без сортировки.
- **Generic и без зависимостей на act/ECS** (catalogue ниже них по зависимостям):
  участники приезжают сырыми `uint`; `call_log` не знает их смысла. Это не storage для
  разнородных typed effects и не replay-лог.

## Deferred MT strategy — первый рабочий срез

Gameplay-effect остаётся обычной C++-функцией. Для devils_script регистрируется
не прямой указатель на неё, а generated wrapper pointer с той же сигнатурой:

```cpp
void add_strength(entity_id actor, int amount);

using strength_strategy = catalogue::mt::preset::parallel_collect<0>;
struct strength_effects;
using strength_domain = catalogue::mt::domain<strength_effects, strength_strategy>;

using add_strength_deferred = strength_domain::fn_traits<
  &add_strength,
  "add_strength",
  "actor",
  "amount"
>;

catalogue::mt::executor<strength_strategy> executor;
strength_domain::set_executor(&executor);
system.register_function<add_strength_deferred::fn_deferred_ptr>("add_strength");

executor.begin_record(source_capacity, max_effects_per_source, deferred_call_budget);
// worker:
{
  catalogue::mt::record_scope source(entity_id, dense_source_index);
  script.process(&vm); // effect calls only record owned arguments
}
// pool barrier:
executor.seal();
executor.commit(); // or dispatch independent groups through the pool + finish_commit()
```

Реализованные имена — `mt::domain<IdentityTag, Strategy>` и `fn_deferred_ptr`.
`IdentityTag` определяет независимый static executor binding/lifecycle, `Strategy`
является переиспользуемой policy. Поэтому несколько gameplay-доменов могут применять
один `preset::parallel_collect<0>`, не разделяя executor. Нейтральные presets:
`parallel_collect<KeyArg>`, `serial_elect<KeyArg, Conflict>` и
`structural_elect<KeyArg, Conflict>`; нестандартная комбинация по-прежнему задаётся
напрямую через `collect`/`elect`. Отдельный namespace нужен: существующий
`catalogue::domain<auto>` — value-domain интроспекции. Контракт:

- wrapper в record-фазе копирует функцию и owned представление аргументов в
  executor стратегии, не вызывает gameplay-тело и не мутирует `world`;
- executor устанавливается до параллельного прохода, стабилен до barrier и
  проходит фазы `begin_record → record → seal/barrier → commit`;
- отсутствие executor-а/неверная фаза — громкая ошибка, не immediate fallback;
- trace-domain и deferred execution-domain — ортогональные оси. Одна и та же deferred
  функция может независимо трассироваться; trace не выбирает MT-поведение;
- `collect` группирует по ключу. Независимые key-группы можно commit-ить
  параллельно, внутри группы вызовы идут последовательно в стабильном порядке;
- `elect` выбирает победителя детерминированным rank и обычно исполняется позже
  в serial lane; `world.create/delete` и structural component changes всегда
  можно направить в отдельную `serial_structural` lane;
- optional conflict policy дополняет elect. `conflict::target_not_source`
  гасит key-группу, если этот key также является source в той же record-фазе;
  это политика для каскадных/симметричных взаимодействий, а не свойство всех elect;
- `join` — будущая стратегия, она не нужна первому срезу.

`record_scope` — единственная ambient часть: короткоживущий `thread_local`
контекст текущего worker-вызова с `{source_id, dense_source_index,
next_local_ordinal}`. Он не содержит world/executor и восстанавливается RAII при
выходе. Благодаря этому wrapper сохраняет исходную C++-сигнатуру, а ordinal
общий для эффектов ВСЕХ execution-domain внутри одного script pass.

Детерминированный порядок не зависит от worker scheduling. Atomic ticket используется
только как физический append-index плотного journal-а и не имеет gameplay-семантики.
Каждый вызов получает детерминированный глобальный sequence id
`source_index * sequence_capacity + local_ordinal`; это монотонная нумерация
логических source-слотов, не arrival order. Базовый ключ
collect: `(group_key, source_id, local_sequence, function_id)`; дополнительные
варианты порядка задаёт strategy. Переполнение sequence/journal — ошибка, не drop.

Текущий `call_log` по-прежнему хранит один last-write-wins вызов на source-index;
в tile_frontier это только action journal для FSM/звука. `mt::executor` от него НЕ зависит:
он заранее размеряет плотный journal по явному `deferred_call_budget`, append-ит только
фактические вызовы и после barrier восстанавливает semantic order. Поэтому большой
`world.index_capacity()`/разреженный `source_index` участвует только в вычислении id
и не умножает размер storage. Один source должен присутствовать в worklist ровно один
раз; дубли исправляются в producer/scheduler, journal их не ищет и не дедуплицирует.

Несколько истинных effect-веток одного ds-скрипта записываются независимо. Если
`add_strength` попал в collect, а `eat_prey` проиграл elect, strength всё равно
commit-ится. Соседство блоков не создаёт транзакцию. All-or-nothing effect groups
нужно когда-нибудь описать явно; часто зависимая реакция лучше выражается
отдельным post-commit gameplay-скриптом.

`devils_script::on_effect` не является MT-hook. Это gameplay-реакция на запись
вызова (например, подготовить проверку ачивки), и она сама может проходить через
catalogue strategy. Она не гарантирует, что elect-вызов победил и его тело уже
исполнилось. Реакция на успешный commit проверяет фактические компоненты/состояние
в следующем gameplay-шаге.

### Live tile_frontier pipeline

`actor_world_slice` уже использует этот контракт, а не старую ручную arena-проводку:

1. После select/budget-clamp и перед MT cognition слайс биндит два executor-а и вызывает
   `begin_record(world.index_capacity(), max_effects_per_source, selected_count * max_effects_per_source)`.
2. GOAP выбирает action; worker вызывает его `act::effect` под `record_scope`.
   Нативные тела в `act::registry` заменены `fn_deferred_ptr`, поэтому world ещё не меняется.
3. `apply(pool)` seal'ит и dispatch'ит local collect(self) параллельными key-группами.
4. После barrier он seal'ит и commit'ит eat elect(prey) в ST `serial_structural` lane.
5. После мутаций однослотовый action journal двигает FSM и эмитит звук.

`tile_frontier_resume_smoke` проверяет байтовое равенство world при 1 и 4 worker-ах, а также
save/load continuation. Загружаемый `act::script_function<void>` теперь тоже вызывает эти же deferred
building blocks: GOAP action co-parse'ит optional `effect = <ds>`, shipped `flee/chase/think` идут этим
путём, а `tile_frontier_config_effect_smoke` проверяет resource-load и 1-vs-4 identity. Executor
сохраняет несколько effect-вызовов одного source/script pass.

Аргументный storage executor-а живёт только до commit текущего шага и не является
serializer/replay format. Каждый journal slot содержит 128-byte inline payload;
`sizeof(tuple<stored args...>) > 128` и over-alignment отвергаются compile-time,
oversized runtime/fallback heap path отсутствует. Первый срез принимает только free
`void` functions с copyable value-аргументами; `string_view` превращается в owned
`string`, остальные значения копируются. Сам type-erased call object при record не
аллоцируется, но длинное содержимое owned `string` всё ещё может аллоцироваться самим
`std::string`. Ссылки запрещены compile-time; сырые указатели внутри value-handle
допустимы лишь при гарантированном lifetime до commit. Member functions и custom
fixed-storage codecs — следующие расширения.

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

## Техдолг

- Добавить registry функций внутри домена по `function_id`, чтобы можно было
  перечислять доступные wrapped-функции, искать метаданные по id и использовать
  это для debug UI и диагностики strategy buffers.
- Добавить custom fixed-storage codecs (в частности bounded owned string), не меняя
  semantic key `(key, source, local_sequence, function)` и публичный wrapper API.
- Добавить member functions/custom argument codecs, structural ST pipeline
  integration и затем `join`.

## Старый RPC / Replay прототип (перенесён в exclude/)

Ранний byte-buffer/replay/RPC подход жил в `common.h`, `core.{h,cpp}`,
`channel_data.h`, `registry.{h,cpp}`, `rpc_function.h`, `demo.{h,cpp}`: buffer из
headers + payload, registry для поиска reader по id, статические channels,
consumers, `rpc_function` с `call/write/log/read/reg`. Он опирался на `zpp_bits`
(уже удалён из движка) и не имел живых потребителей, поэтому **перенесён в
`exclude/libs/catalogue/`** (см. `exclude/README.md`). Это только исторический
материал. Не возвращать его в live catalogue; replay/RPC при реальной потребности
получат отдельную библиотеку и не должны определять MT-dispatch API.

## Как Об Этом Думать

`catalogue` должен стать маленьким техническим слоем вокруг вызовов:

```text
обычная функция -> constexpr wrapper pointer -> trace policy + execution strategy
```

Он не должен становиться gameplay registry вместо `act`, resource storage вместо
`demiurg`, serializer или replay/netcode. Его полезная роль: наблюдать, измерять и
детерминированно перенаправлять выбранные вызовы, не заставляя gameplay-функции
знать о worker pool, arbitration buffers и pipeline barriers.
