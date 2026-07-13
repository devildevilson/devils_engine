# act

`libs/act` - общий слой gameplay-функций. Его задача - дать единый реестр,
единый контекст вызова и общий контракт для функций, которые потом используют
разные подсистемы: GOAP в `libs/acumen`, FSM в `libs/mood`, UI/скрипты и другие
gameplay-слои.

Главная идея библиотеки - не конкретная gameplay-логика, а генерализация
функтора. Функция получает общий `act::exec_context`, возвращает значение одной
из фиксированных категорий и может быть реализована разными backend'ами:
native C++, будущим `devils_script`, Lua или другим слоем.

## Базовые Типы

`common.h` содержит минимальные gameplay-типы:

- `entity_id` - тонкий handle сущности, сейчас `uint32_t`;
- `real_t` - число симуляции, сейчас `double`;
- `vec3` - простой вектор из трех `real_t`.

`entity_id` намеренно не является fat handle. Тип сущности должен определяться
компонентами в ECS или контекстом вызывающего слоя, а по сети/диск должен ехать
простой id.

`real_t` вынесен в один typedef: если позже потребуется жесткий детерминизм,
его можно заменить на fixed-point тип в одном месте.

## Категории Функций

Функции разделены по типу возврата. Это важная часть контракта:

- `void` -> `category::effect`;
- `bool` -> `category::predicate`;
- `real_t` -> `category::number`;
- `utils::id` -> `category::string`;
- `entity_id` -> `category::object`.

Типизированная база выглядит так:

```cpp
template <typename RetT>
struct function : function_base {
  virtual RetT invoke(const exec_context& ctx, call_context& call) const = 0;
  RetT invoke(const exec_context& ctx) const; // совместимый вызов без args/lists
};
```

Категория одновременно кодирует смысл функции:

- `effect` - потенциально mutating действие;
- `predicate`, `number`, `string`, `object` - pure-запросы к контексту.

Отдельного поля purity/signature сейчас нет: тип возврата уже задает ожидаемую
категорию использования.

## Native Functions

Реализованный backend сегодня - `native_function<RetT>`.

Он хранит один из двух сырых function pointer:

```cpp
using legacy_fn_t = RetT (*)(const exec_context&);
using fn_t = RetT (*)(const exec_context&, call_context&);
```

Это сделано без `std::function`, потому что часть вызовов находится в горячем
пути: например `acumen` может дергать predicates при пересчете GOAP-состояния и
поиске плана.

Native function также может хранить короткое описание. `describe(ctx, out)`
отдает это описание в callback, если оно задано. Это заготовка под UI:
tooltip'ы, объяснение "почему нельзя", предпросмотр эффекта или разбор
предиката.

## Script и Lua Backends

В коде есть классы:

- `script_function<RetT>`;
- `lua_function<RetT>`.

`script_function<bool/real_t/void>` исполняет скомпилированный `devils_script`
container на per-worker `exec_context::vm`. Именованные значения и списки из
`call_context` связываются с `ctx:arg`/`ctx:list` до process и забираются обратно
после него. String/object/vector marshalling пока не завершён. `lua_function`
остаётся заглушкой.

Lua backend отмечен как guest/UI-слой. Он не должен стать основным backend'ом
для mutating simulation effects: эффекты симуляции должны оставаться
dry-run/log/replay-friendly.

## Registry

`act::registry` - таблица функций по `fn_id`.

`fn_id` сейчас равен `utils::string_hash(name)`. При регистрации:

```cpp
act::registry reg;
reg.reg("actor.is_hungry",
        std::make_unique<act::native_function<bool>>(&predicate_is_hungry));
```

Реестр владеет функциями через `std::unique_ptr<function_base>`.

Доступ бывает generic:

```cpp
const function_base* f = reg.get(id);
```

и типизированный:

```cpp
const predicate_function* p = reg.predicate(id);
const effect_function* e = reg.effect(id);
```

Типизированный доступ проверяет категорию и возвращает `nullptr`, если функция
существует, но имеет другой тип возврата.

Потоковый контракт текущего реестра:

- `reg()` вызывается в однопоточной фазе загрузки/сборки;
- после этого `get()` и типизированные lookup'и можно читать параллельно;
- потребители вроде `acumen` и `mood` обычно резолвят функции один раз при
  сборке своих таблиц и кешируют typed pointer.

При повторной регистрации того же hash реестр падает через assert. Сейчас это
ловит и повтор имени, и hash collision.

## Exec Context

`act::exec_context` - immutable-контекст вызова gameplay-функции.

Он содержит:

- `scope[max_scope]` - фиксированный стек сущностей без аллокаций;
- `scope_count`;
- `w` - opaque pointer на мир или его срез;
- `rng_seed`, `rng_entity`, `rng_tick`;
- `sink` - optional `effect_sink`;
- `vm` - backend scratchpad для будущего script runtime.

`scope[0]` обычно означает primary actor, `scope[1]` - target. Есть helpers:

```cpp
ctx.primary();
ctx.secondary();
```

`w` намеренно forward-declared как `act::world`. `act` не зависит от
`aesthetics`; конкретный gameplay слой сам знает, как интерпретировать этот
указатель. В `tile_frontier`, например, он указывает на `aesthetics::world`.

RNG в контексте сделан без mutable counter. `random(purpose)` вычисляется из
immutable входов:

```cpp
utils::mix(rng_seed, rng_entity, rng_tick, purpose)
```

Это делает результат независимым от порядка и количества вызовов случайности:
добавление одного draw/predicate не сдвигает остальные случайные значения.

## Effect Sink

`effect_sink` - временный интерфейс для приема mutating effects:

```cpp
virtual void emit(utils::id effect_id, std::span<const value> args) = 0;
```

Если `exec_context::sink == nullptr`, вызов считается dry-run. Такой режим нужен
GOAP-планировщику, UI-preview и любым предиктивным расчетам, где нельзя менять
мир.

Если sink задан, effect должен писать свои намерения/аргументы туда, а не
мутировать скрытое состояние backend'а. Идея в том, чтобы эффекты можно было
логировать, replay'ить и переносить в deterministic apply pipeline.

В комментариях к коду уже отмечено, что реальная реализация sink скорее всего
переедет в `libs/catalogue`; в `libs/act` пока остается только интерфейс.

## Value

`act::value` - маленький POD tagged union для generic-границ.

Он не является основным типом возврата gameplay-функций: возвраты остаются
типизированными через `function<RetT>`. `value` нужен там, где требуется
универсальный payload:

- аргументы эффекта в `effect_sink::emit`;
- будущий маршалинг аргументов в script backend;
- лог/replay/network границы.

Поддерживаемые виды:

- none;
- boolean;
- integer;
- number;
- vector;
- handle;
- string hash.

## Intent

`act::intent` - компактный шов между thinking-слоем и ECS/apply-фазой.

GOAP, FSM или script не обязаны мутировать мир сразу. Они могут выдать intent,
а ECS-система позже отсортирует intents и применит их детерминированно.

Текущие виды:

- `move_to`;
- `turn_to`;
- `call_function`;
- `fsm_event`.

У intent есть:

- `actor`;
- payload;
- `source_action` - provenance, то есть id действия, которое породило intent.

В `tests/tile_frontier` GOAP выбирает `acumen::action`, затем первый action
плана превращается в `act::intent_kind::call_function`, сортируется по actor id
и исполняется в apply-фазе через `registry_.effect(fn)`.

## Текущие Потребители

`libs/acumen` использует `act` так:

- `state_metric.name` резолвится в `predicate_function`;
- `action.name` резолвится в `effect_function`;
- `compute_state(ctx)` вызывает predicates;
- сам GOAP возвращает символический план, не мутируя мир.

`libs/mood` использует `act` так:

- FSM guard резолвится в `predicate_function`;
- FSM action резолвится в `effect_function`;
- runtime step вызывает их через общий `exec_context`.

`tests/tile_frontier` использует один `act::registry` для actor simulation:

- регистрирует perception predicates;
- регистрирует effects вроде `flee`, `chase`, `eat`, `wander`;
- собирает на этом же registry `acumen` и `mood`;
- прогоняет выбранные действия через `act::intent`.

## Что Уже Умеет

На данный момент `libs/act` умеет:

- задавать общий `entity_id`, `real_t`, `vec3`;
- описывать gameplay-функции по категории возврата;
- хранить generic `function_base`;
- вызывать typed `function<RetT>::invoke(ctx)`;
- передавать общий mutable `call_context` с named in/out args, result и lists в native и ds backend;
- регистрировать native C++ functions;
- исполнять `script_function<bool/real_t/void>` поверх devils_script;
- отдавать typed lookup из `registry`;
- хранить optional description и вызывать `describe`;
- отделять immutable `exec_context` от mutable данных конкретного вызова;
- задавать deterministic counter-free random через `ctx.random(purpose)`;
- различать dry-run и effect mode через `exec_context::sink`;
- описывать generic effect arguments через `act::value`;
- передавать thinking результат в apply-фазу через `act::intent`.

## Что Еще Не Сделано

Основной каркас готов, но большая часть ценности `act` появится после следующих
шагов:

- завершить string/object/vector marshalling `call_context` ↔ devils_script;
- консолидировать регистрацию native building blocks в ds при сохранении act как фасада источников;
- определить формат конфигов, которые регистрируют gameplay-функции в
  `act::registry`;
- добавить загрузку этих функций из модулей/ресурсов;
- реализовать полноценный `describe` для script backend;
- решить, нужен ли number unit/tag для денег, дистанций, процентов и других
  несовместимых числовых смыслов;
- уточнить роль Lua: оставить UI/guest backend или дать ему ограниченный доступ
  к pure functions;
- перенести реальную реализацию `effect_sink` в replay/catalogue слой;
- определить стабильный serialization/logging формат для `intent` и
  `effect_sink` payload.

Граница библиотеки сейчас такая: `act` отвечает за общий контракт
gameplay-функций, их регистрацию, типизацию и контекст вызова. Конкретная
gameplay-логика, парсинг конфигов, скриптовый runtime и применение мутаций мира
должны жить в более высоких слоях.
