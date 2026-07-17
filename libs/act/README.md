# act

`libs/act` - общий слой gameplay-функций. Его задача - дать единый реестр,
единый контекст вызова и общий контракт для функций, которые потом используют
разные подсистемы: GOAP в `libs/acumen`, FSM в `libs/mood`, UI/скрипты и другие
gameplay-слои.

Главная идея библиотеки - не конкретная gameplay-логика, а генерализация
функтора. Функция получает общий `act::exec_context`, возвращает значение одной
из фиксированных категорий и может быть реализована разными backend'ами:
native C++, будущим `devils_script`, Lua или другим слоем.

## Упаковщик функций (`packer.h`)

Чтобы геймплейные функции НЕ зависели от `exec_context` (least-privilege: эффекту, которому нужны
`world` + пара сущностей, незачем видеть rng/sink/scratch/весь scope-стек), есть **упаковщик**
`act::pack<&fn>()`. Он адаптирует обычную C++-функцию в `act::function<Ret>`, доставая аргументы из
контекста по ТИПУ параметра:

- **`act::entity_handle` `{world*, entity_id}`** ← `scope[k]` (+ `world` из ctx). k-й по счёту
  entity_handle-аргумент ← k-й элемент scope. Fat-handle живёт только на ВРЕМЯ вызова (в лог/сеть не
  идёт — указатель непереносим), проект реинтерпретирует `world` в свой ECS своим аксессором.
- **`act::rng_source` `{seed, entity, tick}`** ← детерминированные RNG-входы из ctx (`.random(purpose)`
  и `.tick`). Так эффект получает RNG/тик, не открывая весь контекст.
- **`const exec_context&`** ← сам ctx (СПЕЦ-СЛУЧАЙ: особой функции нужен полный контекст).
- **прочее (plain)** ← `call_context.arguments()` позиционно (k-й plain-аргумент ← k-й named value).

Индексы источников считаются в compile-time из позиции параметра ⇒ порядок вычисления аргументов не
важен. Конвенция сигнатур: entity-аргументы обычно идут первыми, дальше скаляры (но биндинг по типу
допускает и смешанный порядок). Пример: `void effect_eat(entity_handle self, entity_handle prey,
rng_source rng)`; `bool is_hungry(entity_handle self)`. Регистрация — `registry_.reg("eat",
act::pack<&effect_eat>())`. Функции с `exec_context` регистрируются как раньше (`native_function`) —
оба пути сосуществуют.

**Арбитраж взаимодейств — НЕ в упаковщике.** `pack` лишь биндит аргументы. Если Fn —
catalogue `fn_deferred_ptr`, его generated-обёртка запишет typed call в collect/elect executor, а
вызывающий pipeline после barrier выполнит parallel или serial commit. `act::interaction` в
tile_frontier пока подсказывает project-коду, какой scope slot заполнить target-ом; сами
arbitration/order/commit lane живут в strategy type. Внешние к ECS сервисы всё ещё должны
сами обеспечивать thread safety или выбирать ST commit policy.

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
container на per-worker `exec_context::scratch->vm`. Именованные значения и списки из
`call_context` связываются с `ctx:arg`/`ctx:list` до process и забираются обратно
после него. String/object/vector marshalling пока не завершён. `lua_function`
остаётся заглушкой.

Lua backend отмечен как guest/UI-слой. Он не должен стать основным backend'ом
для mutating simulation effects: эффекты симуляции должны оставаться
пригодными для dry-run, наблюдения и deterministic deferred apply.

## Config Script Resource

`act::script_resource` владеет общим TAVL-документом `{ret, scope, expr}` и готовым
`devils_script::container`, но не знает конкретный gameplay root scope. На загрузке он делегирует
компиляцию внедрённому `act::script_compiler`.

Тот же type-erased seam использует `acumen::goap_resource` для co-parse metric predicates и action
effects прямо из TAVL event stream. Проект реализует только конкретные
`parse<Ret, RootScope>`-инстанциации; в `tile_frontier` это `script_environment` + `entity_scope`.
Таким образом схемы/ресурсы принадлежат owner-библиотекам, а доступ к ECS остаётся игровым кодом.

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
- `scratch` - ссылка на принадлежащий executor/worker `act::execution_scratch`.

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

`execution_scratch` объединяет reusable `devils_script::context` и `call_context`. Scalar-аргументы
`call_context` занимают фиксированные 8 inline slots — столько же, сколько стандартный стек аргументов
ds; списки сохраняют динамическую capacity между вызовами. Глобального/TLS pool нет: подсистема
встраивает этот блок в собственный per-worker scratch. Например, `acumen::execution_scratch` добавляет
A*-контейнер и solution cache.

## Generic stats initialization

Плоский numeric aggregate не требует конструктора. Проект выбирает один из двух явных путей:

- `initialize_stats<stats>()` делает `stats{}` и сохраняет C++ default member initializers;
- `initialize_stats<stats>(callback)` проходит отражённые поля и задаёт каждое значение callback-ом.

`make_stats<stats>(...)` остаётся обычной aggregate initialization, а `register_stats` независимо от
способа создания публикует типизированный ds-scope (`stats.health`, `stats = { add_health(…) }`).

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
переносить в deterministic apply pipeline и наблюдать без скрытых мутаций.

После решения 2026-07-17 первый typed MT-путь реализован через catalogue
`fn_deferred_ptr` + strategy executor. Нужно решить, остаётся ли `effect_sink`
generic act-сеамом рядом с ним или заменяется для зарегистрированных ds-effects.
Catalogue при этом не является replay/serialization слоем.

## Value

`act::value` - маленький POD tagged union для generic-границ.

Он не является основным типом возврата gameplay-функций: возвраты остаются
типизированными через `function<RetT>`. `value` нужен там, где требуется
универсальный payload:

- аргументы эффекта в `effect_sink::emit`;
- будущий маршалинг аргументов в script backend;
- generic trace/debug границы.

Долговременный replay/network payload, если понадобится, принадлежит отдельному
owning/serialization слою, а не `act::value` или catalogue автоматически.

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

В текущем `subprojects/tile_frontier` GOAP выбирает `acumen::action`, а worker сразу
вызывает `registry_.effect(fn)` под `catalogue::mt::record_scope`. Это безопасно: в registry
лежат `fn_deferred_ptr`-обёртки, которые только записывают typed arguments. Тела
исполняются после barrier: collect-группы параллельно, structural elect однопоточно.

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

`subprojects/tile_frontier` использует один `act::registry` для actor simulation:

- регистрирует perception predicates;
- регистрирует deferred effects вроде `flee`, `chase`, `eat`, `wander`;
- собирает на этом же registry `acumen` и `mood`;
- записывает выбранные действия в catalogue strategy executors и commit'ит их по lane-ам.

## Что Уже Умеет

На данный момент `libs/act` умеет:

- задавать общий `entity_id`, `real_t`, `vec3`;
- описывать gameplay-функции по категории возврата;
- хранить generic `function_base`;
- вызывать typed `function<RetT>::invoke(ctx)`;
- передавать общий mutable `call_context` с named in/out args, result и lists в native и ds backend;
- переиспользовать per-worker `execution_scratch` без локального call-frame в hot path;
- регистрировать numeric aggregate через scope API (`stats.abc`, `stats = { add_abc }`) без префиксов;
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
- согласовать `effect_sink` с catalogue typed strategy executor и не дублировать
  два механизма deferred apply;
- определить typed marshalling для `intent` и `effect_sink` payload; стабильный
  replay/network format вынести в отдельный слой при реальной потребности.

Граница библиотеки сейчас такая: `act` отвечает за общий контракт
gameplay-функций, их регистрацию, типизацию и контекст вызова. Конкретная
gameplay-логика, парсинг конфигов, скриптовый runtime и применение мутаций мира
должны жить в более высоких слоях.
