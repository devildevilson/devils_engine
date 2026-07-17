# mood

`libs/mood` - маленькая FSM-система, принимающая структурные `transition_config` либо legacy
текстовые строки. Ее
основная задача - разобрать список переходов, построить быстрый индекс и по
паре `(state, event)` вернуть список переходов-кандидатов в исходном top-down
порядке.

Библиотека намеренно простая: `mood::system` не хранит runtime-состояние актора,
не тикает сам по себе, не знает про animation/audio/gameplay и не применяет
эффекты без явного вызова helper'а. Состояние конкретного актора хранит
вызывающий код.

## Основная Идея

`mood` состоит из двух слоев:

- `mood::system` - тупое быстрое хранилище состояний и переходов;
- `mood::runtime` - helper'ы с conventions: `any_state`, `idle`,
  `on_entry`, `on_exit`, выбор первого валидного перехода и валидация графа.

Такое разделение важно: сама таблица переходов ничего не знает о специальных
именах. Для `system` строка `any_state + attack = hit` - обычное состояние
`any_state`, а смысл wildcard добавляет `runtime::find_with_fallback`.

## Формат Строк

Переход описывается одной строкой:

```text
current_state + event [guard1, guard2] / action1, action2 = next_state
```

Все части кроме `current_state` опциональны:

```text
begin + idle = prepare_weapon
melee_attack + on_entry / play_sound, spawn_fx
initial_state [can_start] = prepare_weapon
initial_state = prepare_weapon
```

Смысл частей:

- `current_state` - из какого состояния переход;
- `+ event` - событие, по которому переход ищется;
- `[guards]` - predicate-функции из `act::registry`;
- `/ actions` - effect-функции из `act::registry` в legacy string syntax;
- `= next_state` - новое состояние.

Переход без `= next_state` считается внутренним: он может выполнить actions, но
не меняет текущее состояние. Так удобно описывать `on_entry`, `on_exit` и
чистые effect-переходы.

Имена сейчас состоят из identifier-токенов: буквы, цифры и `_`. Поэтому имена с
точкой вроде `actor.is_hungry` в строках FSM не подходят; для `mood` нужны
dot-free имена. Это уже видно в `tile_frontier`: guard называется `is_eating`,
хотя GOAP-метрики могут иметь имена с точками.

Resource/config loaders должны разбирать свой нативный формат сразу в
`mood::transition_config{current_state,event,guards,actions,next_state}`. В TAVL строка перехода
не заключена в кавычки, а actions оборачиваются в tuple, чтобы структура была однозначной:

```tavl
transitions = [
  idle + see_enemy [can_see] / (remember_enemy, raise_alarm) = alert
]
```

`mood::system(const act::registry*, std::vector<transition_config>)` строит runtime-индекс напрямую,
не восстанавливая и не парся промежуточные строки. String-конструктор сохранён для C++ fixtures и
совместимости.

## Transition

`mood::system::transition` хранит:

- `full_line` - исходная строка для диагностики;
- `current_state`;
- `event`;
- `next_state`;
- до 8 guard names;
- до 8 action names;
- precomputed hashes для state/event/next;
- resolved `act::predicate_function*` для guards;
- resolved `act::effect_function*` для actions.

Guards и actions резолвятся один раз в конструкторе `mood::system` через общий
`act::registry`. Если guard/action не найден или имеет неправильную категорию,
конструктор падает через `utils::error`. В hot path lookup в registry уже не
нужен.

`transition::is_valid(ctx)` проверяет все guards. `transition::process(ctx)`
выполняет все actions.

## System

`mood::system` строится из `act::registry` и структурных transitions; legacy набор строк также
поддерживается:

```cpp
std::vector<std::string> lines = {
  "any_state + flee = flee",
  "any_state + eat [is_eating] = eating",
  "any_state + wander = wander",
};

mood::system fsm(&registry, std::move(lines));
```

Конструктор делает несколько шагов:

1. валидирует structured config (либо парсит legacy строки) в `transition`;
2. считает `utils::string_hash` для state/event/next;
3. stable-sort'ит переходы по `(state_hash, event_hash)`;
4. строит hash-index `m_index`;
5. проверяет дубли внутри группы;
6. резолвит guards/actions в typed функции из `act::registry`.

Stable sort сохраняет порядок исходных строк внутри одной группы
`(state, event)`. Это принципиально: runtime проверяет guards сверху вниз и
берет первый переход, который прошел.

Основной быстрый доступ:

```cpp
auto group = sys.find_transitions(state_hash, event_hash);
```

Есть строковая обертка:

```cpp
auto group = sys.find_transitions("melee_ready", "attack");
```

Она просто хеширует строки и вызывает основной вариант.

## Runtime Helpers

`mood::runtime` добавляет слой conventions.

Стандартные имена:

- `any_state` - wildcard state;
- `idle` - стандартное "нет внешнего события";
- `on_entry` - pseudo-event входа в состояние;
- `on_exit` - pseudo-event выхода из состояния.

`find_with_fallback(sys, state, event)` сначала ищет точную группу
`(state, event)`, а если ее нет - группу `(any_state, event)`.

`step(sys, state, event, ctx)`:

1. берет candidates через fallback;
2. проверяет guards top-down;
3. возвращает первый валидный переход;
4. не выполняет actions.

Результат описан `step_outcome`:

- `transitioned` - переход найден и guards прошли;
- `blocked` - candidates были, но guards все отсеяли;
- `no_transition` - переходов нет даже через `any_state`.

`step()` намеренно чистый в смысле FSM-мутаций: он решает, но не применяет.
Это позволяет думать в любом порядке, а применять изменения в детерминированной
apply-фазе.

## Apply Transition

`apply_transition(sys, cur_state, taken, ctx)` выполняет выбранный переход:

```text
on_exit(old_state) -> actions transition -> on_entry(new_state)
```

`on_exit` и `on_entry` запускаются только если переход реально меняет состояние,
то есть у него есть `next_state`.

Для каждого `on_exit`/`on_entry` берется первый переход в группе, у которого
прошли guards. Если подходящего перехода нет, ничего не выполняется.

Функция возвращает hash нового состояния. Если переход внутренний и не имеет
`next_state`, возвращается `utils::invalid_id`, а вызывающий код должен оставить
старое состояние.

Типичный внешний loop может выглядеть так:

```text
event -> step -> apply_transition
потом несколько idle step/apply до стабильного состояния с cap итераций
```

Cap нужен, потому что idle-переходы могут образовать цикл.

## Graph Validation

`mood::validate(sys)` - debug/load-time диагностика графа.

Она собирает множества:

- состояний с исходящими переходами;
- состояний, которые являются target'ами.

Потом предупреждает через `utils::warn`:

- о target-состояниях без исходящих переходов;
- о состояниях с исходящими переходами, которые никогда не достижимы как
  target.

`any_state` учитывается как convention и не считается обычным недостижимым
состоянием.

Для подозрительных имен есть простая fuzzy-подсказка через расстояние
Левенштейна: `did you mean ...`.

Валидация не падает на terminal/unreachable состояния, потому что часть из них
может быть легитимной: начальное состояние, terminal state, debug-only state.
Это предупреждения, а не ошибки.

## Связь С act

`mood` не хранит свои `std::function`. Все guards и actions берутся из
`libs/act`:

- guard = `act::predicate_function`;
- action = `act::effect_function`;
- вызов идет через общий `act::exec_context`.

Это делает `mood` совместимым с `acumen` и другими gameplay-системами: один
`act::registry` может одновременно кормить GOAP-метрики, GOAP-actions и FSM
guards/actions.

В `subprojects/tile_frontier` это используется так:

- GOAP выбирает высокоуровневое действие (`flee`, `eat`, `wander`);
- это имя становится event для `mood`;
- `mood` переводит actor FSM в одноименное состояние;
- состояние используется как слой для animation/visual/audio behavior.

## Что Уже Умеет

На данный момент `libs/mood` умеет:

- принимать structured FSM transitions и legacy C++ строки;
- хранить states/events/transitions;
- хранить до 8 guards и до 8 actions на переход;
- резолвить guards/actions через `act::registry`;
- строить быстрый индекс `(state_hash, event_hash) -> span transitions`;
- отдавать transitions в исходном порядке внутри группы;
- искать переходы по precomputed hash или по строке;
- проверять guards top-down;
- различать `transitioned`, `blocked`, `no_transition`;
- делать fallback с конкретного состояния на `any_state`;
- исполнять выбранный переход с `on_exit` и `on_entry`;
- валидировать граф и предупреждать о тупиках/недостижимых состояниях.

Текущее покрытие в `tests/utils_general_test.cpp` проверяет:

- парсинг переходов с events, guards, actions и next state;
- поиск переходов по строкам;
- сохранение порядка переходов внутри группы;
- доступ по precomputed hash;
- `on_entry`/`on_exit` как обычные pseudo-events для `system`;
- `step()` с успешным переходом;
- `apply_transition()`;
- fallback на `any_state`;
- `no_transition`;
- вызов `validate()`.

`tests/fsm_config_test.cpp` отдельно проверяет owner-level `mood::fsm_resource`: native TAVL rows,
guards, parenthesized actions и прямое построение структурированного `mood::system`.

## Что Еще Не Сделано

Система уже закрывает свою основную задачу. Возможные будущие улучшения:

- добавить более явные ошибки/лимиты для переполнения 8 guards/actions;
- добавить тесты на `blocked`, `on_exit`/`on_entry` guards и internal
  transitions без `next_state`;
- добавить helper для стандартного idle loop с cap итераций;
- определить serialization/debug view для текущего состояния FSM актора;
- улучшить диагностику parser errors и показывать позицию токена.

Граница библиотеки сейчас такая: `mood` отвечает за таблицу FSM-переходов,
быстрый доступ к candidates и маленький runtime-helper слой. Хранение текущего
состояния актора, планирование событий, применение intents, анимация и gameplay
эффекты остаются уровнем выше.
