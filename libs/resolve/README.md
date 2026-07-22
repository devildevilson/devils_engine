# `libs/resolve`: детерминированное разрешение массовых gameplay-взаимодействий

## Зачем нужна отдельная библиотека

`resolve` — это движковое ядро для систем, в которых одно исходное взаимодействие создаёт много
типизированных экземпляров работы, каждый экземпляр может породить последствия, а итог должен оставаться
одинаковым при разном числе worker-потоков.

Первый потребитель — сложный расчёт урона:

1. собрать отдельные экземпляры атак или попаданий;
2. вычислить модификаторы величины: стихии, резисты, броню, усиления;
3. провести значение через щиты и другие маршруты;
4. применить итог к HP;
5. породить реакции, возвратный урон, смерть, gameplay-outcome и presentation-feedback;
6. повторять, пока явно построенная цепочка не закончится.

Та же форма встречается не только в пошаговых играх. В FPS весь resolver обычно проходит несколько
границ за один simulation tick, а интерфейс позже показывает агрегированный feedback. В пошаговой игре
между смысловыми границами можно показать анимацию и продолжить только после её gameplay/finished
checkpoint. Различается драйвер, но не правила расчёта.

Библиотека намеренно не знает:

- что такое карта, ход, стихия, конкретный резист или таблица реакций;
- какие ECS-компоненты содержат HP, щит и эффекты;
- где пошаговая FSM ставит animation checkpoint;
- как выглядят частицы, цифры урона, звук и combat log;
- какой скриптовый backend вычисляет модификаторы.

Этим владеет проект. `resolve` задаёт форму работы, provenance, детерминированное MT-исполнение,
продвижение по frontier и жёсткое правило retaliation.

## Текущий статус

Первый низкоуровневый срез реализован как header-only библиотека:

| Файл | Ответственность |
|---|---|
| `work.h` | идентификаторы, provenance, причины, лимиты и семантический порядок |
| `journal.h` | bounded MT-запись, seal, проверка provenance и назначение id |
| `runner.h` | группировка по write-target и параллельный запуск независимых групп |
| `frontier.h` | owning-состояние `begin/advance`, общие бюджеты и host-paced progression |
| `retaliation.h` | дедупликация `(triggering instance, rule)` и запрет рекурсии retaliation |
| `damage.h` | нейтральные damage payload/outcome/route и minimum-HP commit guard |
| `resolve.h` | общий include |

`resolve_pipeline_test` проверяет identity при 1/4 workers, порядок внутри цели, overflow, frontier,
неоднозначный provenance, retaliation и бессмертие без воскрешения.

Первый реальный потребитель теперь находится в `subprojects/cardgame`: один primary hit начинает owned
`frontier_state`, элементальная реакция становится child, thorns проходит через `retaliation_journal`, а
authoritative outcome хранит общий `damage_route`. Project turn/presentation FSM по-прежнему задаёт порядок
`hit → reaction subtree → retaliation → next hit` и сериализует barrier-state для resume.

Это ещё не готовый универсальный `damage_system`. Следующие слои должны вырасти из следующего реального
потребителя: typed stage registry/callbacks, outcome/child scratch, ECS adapter и структурный death/effect
commit. Не стоит вшивать в библиотеку карточную последовательность стадий.

## Термины

### Work instance

Один атомарный экземпляр разрешаемой работы. Например, три удара одной карты — три разных work instance,
а не один вызов с суммарным уроном. Это принципиально для щитов, on-hit, смерти между ударами и thorns.

### Root

Исходный экземпляр, принятый resolver-ом. Producer заранее присваивает ему стабильный семантический
`root` token. Это не физический индекс atomic append. Обычная схема producer-а:

```text
root = source_index * source_capacity + local_ordinal
```

`source_index` берётся из детерминированного worklist, а `local_ordinal` — из порядка действий одного
producer-а. Значение `0` зарезервировано как invalid.

### Child

Последствие другого экземпляра. У child есть:

- тот же `root`;
- `parent = id` непосредственной причины;
- `generation = parent.generation + 1`;
- собственные source, target, lane и local ordinal;
- причина (`reaction`, `retaliation`, `periodic`, `effect` и т.д.).

Использовать нужно `make_child_header()`: он также правильно наследует retaliation-lineage.

### Frontier

Текущий детерминированный набор work instance, готовый к одной смысловой обработке. После barrier все
созданные children собираются, сортируются, получают id и становятся следующим frontier.

Frontier — граница данных, а не обязательный визуальный кадр. За один simulation tick FPS-драйвер может
пройти много frontier. Пошаговый драйвер может остановиться между ними или между более крупными стадиями.

### Lane

Числовая полоса семантического порядка, определённая конкретным resolver-ом. Например, проект может
развести `reaction`, `retaliation`, `death` и `continuation` по lanes. Само ядро не назначает значения и
не считает, что меньший lane автоматически является конкретной gameplay-фазой.

### Target group

Все записи одного frontier, которые пишут одну gameplay-цель. Внутри target group работа идёт строго
последовательно по instance id. Разные target groups могут выполняться параллельно.

### Outcome

Владеющая данными запись о фактически применённом результате: сколько было запрошено, модифицировано,
поглощено, применено, предотвращено guard-ом, жива ли цель и какие children разрешено создать.
Outcome — authoritative gameplay fact. Presentation-feedback строится из outcome, но не заменяет его.

## Главный MT-контракт

Одна стадия resolver-а имеет следующую форму:

```text
deterministic producer/worklist
        │
        ▼
begin_record(fixed capacity)
        │
        ▼
parallel record into distinct physical slots
        │
        ▼
worker barrier
        │
        ▼
seal: validate → semantic sort → assign instance ids
        │
        ▼
group by write target
        │
        ▼
parallel target groups; serial inside one target
        │
        ▼
barrier → deterministic child/outcome merge
        │
        ▼
advance(next frontier) or finish
```

### Почему сериализация внутри одной цели обязательна

Два попадания в одну сущность почти никогда не коммутативны:

- первое может разбить щит, второе пойдёт в HP;
- первое может убить цель;
- первое может сменить стихию или форму;
- trigger может быть одноразовым;
- минимум HP и другие commit guards зависят от уже применённого значения.

Попытка параллельно менять один `health` потребует сложной транзакционной модели и всё равно должна будет
восстановить gameplay-порядок. Поэтому единица безопасного параллелизма — write-target, а не hit.

### Что разрешено target-group callback

Callback одной группы может:

- читать immutable snapshot стадии;
- читать и последовательно менять компоненты своей `target`;
- писать в заранее принадлежащие ему outcome/scratch slots;
- записывать типизированные children в bounded MT journal.

Он не может:

- напрямую менять другую entity;
- делать structural ECS operation (`create/remove component/entity`);
- запускать вложенный `pool.compute()/wait()`;
- читать состояние, одновременно изменяемое другой target group;
- использовать порядок прихода worker-ов как gameplay-порядок.

Если обработка цели A должна повредить B, callback создаёт child с `target = B`. Этот child применяется на
следующей явно выбранной границе. Redirect, cleave, chain lightning, lifesteal и thorns следуют тому же
правилу.

### Source-derived значения

Если формула попадания зависит от силы source, а source может одновременно получить урон или эффект,
нельзя бездумно читать живой mutable component во время target-group phase. Проект выбирает один из двух
явных контрактов:

1. захватить нужное значение в owned payload до параллельного commit;
2. построить read-only stage snapshot, неизменяемый до barrier.

Первый вариант обычно лучше для уже объявленной атаки: её сила не меняется задним числом из-за соседней
группы. Второй подходит для правил, которые по дизайну читают состояние в начале конкретной стадии.

## Семантический порядок и идентификаторы

Физический индекс в `journal` выдаётся через atomic cursor и не имеет gameplay-смысла. После barrier
`seal()` сортирует записи по:

```text
(generation, root, parent, lane, source, local_ordinal, target, cause)
```

Только после сортировки назначается монотонный `instance_id`. Поэтому 1 и 8 workers получают одинаковые
id и одинаковый within-target order.

Producer обязан сделать этот provenance tuple уникальным. Две записи с полностью одинаковым ключом — не
«ничья», а ошибка producer-а: библиотека громко отвергает ambiguous provenance. Payload намеренно не
участвует в сортировке, иначе gameplay-порядок незаметно зависел бы от внутренних битов данных.

`root` и `instance_id` — разные вещи:

- `root` известен до MT record и связывает всё дерево последствий;
- `instance_id` появляется только после seal и уникально обозначает конкретный экземпляр;
- `parent` всегда ссылается на уже назначенный `instance_id` причины.

## Frontier и драйверы игры

`frontier_state<Item>` хранит текущие owned records, следующий id, число уже созданных jobs, номер
frontier и latched fault. Основные операции:

```cpp
resolve::frontier_state<damage_work> state;
resolve::begin(state, roots, limits);

// Проект обрабатывает state.current и после barrier получает sealed children.
resolve::advance(state, children, limits);
```

`advance()` не запускает pool и не знает о кадре. Это сделано специально.

### FPS / real-time

Gameplay host может сделать примерно так:

```text
begin roots collected for tick
while state.active:
    process one project stage/frontier
    advance sealed children
publish feedback after gameplay work
```

Это политика host-а. В ядре нет отдельного фундаментального `run_to_completion`: такая функция была бы
лишь коротким циклом над тем же `advance()` и скрывала бы полезные границы профилирования и fault-report.

### Turn-based

Пошаговая FSM хранит собственный stage cursor и вызывает ту же обработку на нужном gameplay checkpoint:

```text
start animation
    → gameplay checkpoint
    → process/commit resolver stage
    → send calculated result to presentation
    → finished checkpoint
    → move outer combat FSM cursor
```

Resolver не ждёт render thread сам. Ожиданием владеет `simul::presentation_barrier`/project host. Headless
режим не регистрирует presentation task и проходит тот же commit немедленно, поэтому animated/headless
должны давать идентичный gameplay state.

### Почему один frontier не задаёт весь порядок карточного боя

Обычный breadth-first обход children недостаточен для правил вида:

```text
primary hit
  → его elemental reaction subtree
  → его retaliation subtree
  → death boundary
  → только затем следующий primary hit
```

Такой порядок задаёт project stage cursor/continuation plan. `frontier_state` обеспечивает
детерминированный набор на одной границе, но не переставляет проектные стадии. Особенно важно не разрешать
все initial hits одновременно, если первый hit может изменить щит, резист, стихию или смерть для второго.

## Retaliation: жёсткое правило движка

Retaliation включает thorns, return damage и другие ответы «получи урон в ответ на этот экземпляр».

Контракт зафиксирован и не является project option:

1. одна retaliation-rule может сработать не более одного раза на один triggering instance;
2. retaliation не может реагировать на retaliation;
3. никакой потомок retaliation не может снова породить retaliation, даже если промежуточный child помечен
   как `reaction`, `effect` или `custom`;
4. multi-hit может вызвать retaliation по одному разу на каждый hit, потому что у hits разные
   `instance_id`.

Дедупликационный ключ:

```text
(triggering_instance_id, retaliation_rule_id)
```

Значит, два разных эффекта thorns на одной цели могут иметь разные `rule_id` и каждый ответить один раз.
Дважды обнаруженный один и тот же эффект будет схлопнут.

`retaliation_lineage` наследуется всеми descendants. Проверять только
`cause == retaliation` недостаточно: иначе цепочка `retaliation → reaction → retaliation` обошла бы запрет.

Retaliation создаётся только из уже sealed trigger с валидным `instance_id`. Попытка использовать
неназначенный trigger возвращает `invalid_trigger`. Overflow и превышение per-trigger/generation budget
являются ошибкой resolution, а не тихим drop.

Дедупликация выполняется до назначения id. Поэтому повторное обнаружение не оставляет дыр в gameplay-id и
не меняет id последующих children. Если несколько producer paths законно сообщают одну rule, их
`local_ordinal` всё равно должны различаться: уникальность provenance сохраняется до дедупликации rule key.

`retaliation_journal::seal_ordered()` оставляет deduplicated records без id, чтобы project continuation
мог передать их в `frontier_state::advance()` и включить в общие frontier/total budgets. Обычный `seal()`
остаётся convenience-путём для отдельной project stage и назначает id сам.

## Общая damage-модель

`damage_payload<Scalar, Kind>` содержит минимально общие данные:

- project-defined `Kind` — стихия или другой вид;
- исходную `amount`;
- channel (`primary`, `reaction`, `retaliation`, `periodic`, `cost`, `custom`);
- project-defined bit tags.

Channel не заменяет cause provenance. Channel описывает правила damage, cause — происхождение work item.

Рекомендуемая стадийность:

### 1. Gather

Скрипт карты, оружие, explosion query или periodic system создаёт owned damage work. Здесь фиксируются
source, target, базовая величина, kind/channel/tags и необходимые source-derived значения.

### 2. Prepare / modifiers

Чистые или deferred callbacks собирают вклады резистов, vulnerability, armor, критов и иных правил.
Вклады сначала записываются в журнал, затем применяются в стабильном порядке. Скрипт не должен прямо
писать HP из MT record phase.

### 3. Route

Модифицированное значение проходит через щиты, barriers, перенаправление и HP. `damage_route` сохраняет
разницу между:

- `requested`;
- `modified`;
- `shield_absorbed`;
- `hp_before`;
- `proposed_hp_after`;
- `committed_hp_after`;
- `lethal_prevented`.

Эта детализация нужна триггерам и UI. Например, «получить волю за лично предотвращённый урон» не следует
восстанавливать из одной финальной дельты HP.

### 4. Commit guards

Бессмертие вида «этот урон не может опустить HP ниже 1» — commit guard, а не резист. Сначала считается
обычный proposed result, затем guard меняет только `committed_hp_after` и записывает предотвращённую часть.

`apply_minimum_hp_guard()` дополнительно гарантирует, что guard не лечит и не воскрешает цель, уже бывшую
ниже floor до текущего instance. При `hp_before == 0` floor `1` остаётся эффективным floor `0`.

Project обязан явно решить:

- действует ли guard на `cost`/self-damage;
- расходуется ли одноразовое бессмертие на hit;
- когда удаляется компонент guard;
- считается ли предотвращённая часть для triggers.

### 5. Outcomes and children

После commit создаются typed outcomes. Из фактического outcome, а не из намерения, выводятся:

- elemental reaction work;
- retaliation work;
- lifesteal/heal work;
- on-damage/on-shield-break события;
- death candidate;
- presentation feedback.

Если target не существует, immune или уже недействителен, это тоже явный outcome, а не отсутствие записи.

## Эффекты и статусы

Статусные эффекты используют похожую двухчастную форму:

1. собрать `effect_application` instances;
2. сгруппировать по target и последовательно получить outcome:
   `added`, `updated`, `immune`, `invalid_target`, `rejected` и т.п.

Однако конкретное хранение, stacking, refresh, unique-source и immunity принадлежат проекту. Не нужно
класть боевые эффекты в `aesthetics::flag_set`: `flag_set` — простые engine-generic флаги с countdown, а
боевой статус обычно имеет source, stacks, potency, duration model, dispel category и callbacks.

Создание отсутствующего effect-container является structural ECS operation. Его либо создают заранее на
combat entities, либо записывают structural command и применяют в поздней ST-фазе после worker barrier.

## Death и structural changes

Worker target groups не удаляют entity и компоненты. Рекомендуемая схема:

1. damage commit доводит HP до итогового значения и пишет death candidate outcome;
2. после barrier проект стабильно сортирует candidates;
3. отдельная death stage выполняет on-death rules;
4. только затем serial structural lane удаляет/заменяет entity или компоненты;
5. все relation readers всё равно проверяют `world.exists(full_versioned_entityid)`.

Это позволяет другим hits в строго определённом порядке увидеть состояние «HP <= 0, death pending», если
таково правило игры, и не инвалидирует ECS iteration посреди MT-фазы.

## Бюджеты и ошибки

`resolution_limits` задаёт deterministic limits:

- roots;
- всего jobs;
- jobs в одном frontier;
- generation depth;
- retaliation rules на trigger.

Здесь намеренно нет wall-time budget. Если на одной машине цепочка размажется на два ticks из-за времени,
а на другой уложится в один, gameplay/replay-порядок начнёт зависеть от производительности. Для
авторитетного resolver-а это недопустимо.

Превышение лимита:

- не отбрасывает хвост;
- не переключается на serial fallback;
- не продолжает цепочку в следующем кадре автоматически;
- переводит resolution в loud fault/exception, который host обязан диагностировать.

Для production UI fault можно превратить в корректный abort матча или diagnostic screen, но нельзя делать
частичный «лучший возможный» commit без отдельного project rule.

Capacity MT-журнала задаётся до record phase. Это не обязательно максимальный heap allocation на всю игру:
storage переиспользуется между стадиями и растёт только при увеличении configured high-water mark.

## Почему не `catalogue::mt` и не `aesthetics::message_buffer`

`catalogue::mt` остаётся правильным механизмом deferred вызова конкретных gameplay-функций:

- ds/native effect регистрируется без смены сигнатуры;
- стратегия решает collect/elect и commit lane;
- executor живёт в памяти одного pipeline pass.

Но catalogue journal специально не является сериализуемой очередью причинно связанных damage jobs,
replay artifact или resolver cursor. `resolve` может использовать catalogue callbacks внутри prepare stage,
но владеет отдельными typed work/outcome/frontier.

`aesthetics::message_buffer` предназначен для эфемерной system-to-system коммуникации внутри tick. Его
overflow может быть допустим для некоторых сообщений, но не для authoritative damage chain. Кроме того,
message arrival order не является semantic order.

Коротко:

```text
catalogue deferred call = как отложенно вызвать правило
resolve work/frontier    = что именно причинно разрешается сейчас
message_buffer           = эфемерно сообщить другой системе
presentation feedback    = показать уже вычисленный результат
```

## Presentation и render thread

Render/presentation может исполнять анимацию в своём потоке и присылать два типа checkpoint:

- `gameplay`: момент, когда main должен применить вычисление;
- `finished`: анимация полностью закончена и outer FSM может перейти дальше.

После gameplay checkpoint main вычисляет route/outcome и отправляет presentation новую задачу с уже
известными числами. Resolver не читает render-state, frame alpha или длительность анимации.

Для real-time игры outcomes обычно агрегируются отдельной feedback system: damage numbers, hit marker,
sound, particles. Внутренние frontier не обязаны быть визуально заметны.

Presentation output эфемерен и не сериализуется как authoritative state. После resume он строится заново
из сохранённой gameplay-границы или пропускается согласно контракту внешней FSM.

## Сериализация и resume

Снимок допустим только на явной barrier-границе, не во время parallel record/commit.

Для resume нужны:

- project stage/continuation cursor;
- текущие owned work records или возможность детерминированно восстановить их;
- `next_instance`;
- `total_jobs` и `frontier_index`, если они участвуют в budgets/debug;
- project state, уже применённый на предыдущей commit boundary.

Transient `journal`, target grouping, worker tasks и presentation barrier в snapshot не входят. Проект может
зарегистрировать подходящий `frontier_state<Item>` в своей схеме или зеркалировать его поля в собственном
serializable aggregate. Нельзя сохранять указатели, spans, callback addresses или физический append order.

Если snapshot сделан после gameplay commit, cursor должен быть заранее переведён на следующую границу,
чтобы resume не применил тот же hit второй раз. Это тот же принцип, который используется в
`simul::turn_pipeline`.

## Как добавлять новый тип resolver-а

Минимальный checklist:

1. Сделать trivially-copyable owned payload без указателей и ссылок.
2. Определить стабильный producer worklist и уникальный `(root/lane/local_ordinal)`.
3. Явно перечислить стадии и их continuation order.
4. Для каждой стадии записать read-set, write-target и structural output.
5. Решить, какие source-derived значения захватываются заранее.
6. Задать fixed deterministic capacities и limits.
7. Создавать cross-target последствия только как children.
8. Получать triggers из committed outcomes.
9. Присваивать каждой retaliation rule стабильный `rule_id`.
10. Отделить authoritative outcome от presentation feedback.
11. Определить snapshot boundaries.
12. Сравнить state/hash/bytes при 1, 2, 4 и 8 workers.

## Типичные ошибки

- Суммировать multi-hit до resolver-а и тем самым потерять per-hit triggers.
- Параллельно писать HP одной цели через atomics: это не восстанавливает gameplay semantics.
- Использовать atomic append index как порядок.
- Разрешить callback цели A напрямую менять B.
- Читать mutable source одновременно с commit другой группы.
- Запустить structural ECS operation внутри target-group callback.
- Считать retaliation только по `cause` и потерять lineage descendants.
- Назначать id до retaliation deduplication.
- Молча drop-ать overflow.
- Делить authoritative work по wall-time/frame budget.
- Делать один безусловный BFS там, где gameplay требует reaction/death до следующего primary hit.
- Смешивать combat status с `aesthetics::flag_set`.
- Давать render thread право менять gameplay state.
- Сериализовать transient worker journal или presentation task.

## Ближайшее развитие

Следующий разумный срез после появления реального engine consumer-а:

1. typed stage adapter над ECS read/write access;
2. reusable per-worker child/outcome scratch без динамических аллокаций в hot path;
3. deterministic merge результатов групп;
4. modifier callback registry с явными before/after slots;
5. serial structural lane для death/effect-container changes;
6. catalogue statistics для gather/prepare/group commit/merge/frontier;
7. 1/2/4/8-worker stress с тысячами roots и несколькими поколениями consequences;
8. второй live consumer и проверка, какие части cardgame stage adapter действительно обобщаются.

API следует расширять от таких потребителей. Не нужно превращать `resolve` в универсальный event bus,
dependency graph, scripting VM или готовую боевую систему со встроенными стихиями.
