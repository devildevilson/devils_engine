# `cardgame`

`cardgame` — проектный слой однопользовательского карточного приключения. Сейчас здесь реализован
первый headless/animated срез боевой модели: детерминированный и возобновляемый combat pipeline,
типизированные эффекты, damage routing, retaliation, follow-up и devils_script-адаптеры.

Это не законченная игра и пока не полный боевой вертикальный срез. В коде ещё нет внешнего автомата
приключения, гексагонального мира, колоды/руки, полноценного состава партий, загрузки описаний карт,
настоящего registry правил и завершения боя.

Документ описывает:

- предполагаемый процесс всей игры;
- границы между движком, проектом и контентом;
- фактический контракт текущего combat kernel;
- известные временные решения и следующие точки проектирования.

## Общая модель игры

Верхнеуровневый процесс видится как два вложенных уровня: приложение и текущий забег.

```text
Boot
  -> InitialLoading
  -> MainMenu
       -> NewRunPreparation -> Run
       -> SaveBrowser       -> Run
       -> MetaContent
            (gallery / achievements / unlocked content)
       -> Exit

Run
  -> HexWorld
       -> Combat
       -> Shop
       -> NarrativeEvent
       -> RewardResolution
       -> FloorTransition
  -> RunSummary
  -> MainMenu
```

`Run` владеет авторитетным состоянием приключения: seed, этажом, картой, положением, партией,
колодами, инвентарём, валютой, эффектами забега и текущим незавершённым событием. Конкретная сцена
показывает и редактирует это состояние через типизированные intents/results, но не становится его
единственным владельцем.

Логическое состояние игры и загружаемый `simul::runtime_state_resource` связаны, но не обязаны
соответствовать один к одному:

- runtime state задаёт UI script, allowlist ресурсов и scene manifest;
- проектный автомат задаёт допустимые переходы и авторитетный gameplay cursor;
- несколько локальных экранов текстового события могут использовать один runtime state и собственный
  возобновляемый event cursor;
- loading является явной границей подготовки представления, но не заменяет gameplay transition.

### Цикл гексагонального мира

Рекомендуемая авторитетная последовательность одного перехода:

```text
HexChoice
  -> validate destination / item intent
  -> MoveCommitted
  -> run effects before/after movement
  -> rebuild reachable/unavailable tiles
  -> TileEventStart
  -> Combat | Shop | NarrativeEvent
  -> TileEventResult
  -> RewardResolution
  -> run tile-event completion effects
  -> boss/floor/run completion check
  -> HexChoice | FloorTransition | RunSummary
```

Бой, магазин и текстовое событие должны возвращать типизированный результат внешнему автомату.
Например, бой возвращает победу/поражение/отступление и факты для наград, а не самовольно меняет
гекс-сцену.

Награда может быть выбрана внутри события, однако её авторитетное применение лучше свести в общий
`RewardResolution`. Так боевые, событийные и этажные награды проходят одинаковые модификаторы забега,
логируются и сохраняются в одной точке.

Профиль игрока — галерея, достижения и разблокированный контент — является отдельным долговечным
артефактом. Его не следует смешивать с сохранением конкретного забега.

## Границы ответственности

| Слой | Чем владеет |
|---|---|
| `libs/simul` | общий lifecycle/loading runtime state, presentation barrier, возобновляемый `turn_pipeline`, broker и host-инфраструктура |
| `libs/resolve` | pointer-free provenance, bounded work/frontier, детерминированный порядок, damage route/outcome primitives и жёсткий retaliation lineage |
| `libs/act` + devils_script | общий resource/compiler seam и выполнение скомпилированных скриптов |
| `cardgame` | автомат приключения, порядок боя, партии, карты, цели, стихии, статусы, смерть, follow-up, награды и project-specific presentation vocabulary |
| Контентные ресурсы | карты, персонажи, враги, encounters, этажи, tile events, магазины, reward tables и конкретные rule programs |

Не следует переносить `combat_group`, карточные категории, таблицы стихий, target priority или
`action_report` в общие библиотеки только потому, что сейчас они хорошо изолированы. Возможное выделение
`effect_program` или stage-ledger имеет смысл после появления второго реального потребителя с тем же
контрактом.

## Карта текущего кода

| Файл | Ответственность |
|---|---|
| `include/cardgame/combat.h` | все основные боевые data types, cursors, reports, presentation commands и API `combat` |
| `src/combat.cpp` | combat FSM, effect resolver, damage routing, retaliation, follow-up fixture и presentation checkpoints |
| `include/cardgame/effect_program.h` | target query/snapshot, targeter и explicit binding |
| `src/effect_program.cpp` | детерминированная материализация целей одного beat |
| `include/cardgame/follow_up.h` | категории execution, enabler и порядок участников party pass |
| `src/follow_up.cpp` | детерминированная заморозка порядка follow-up |
| `include/cardgame/combat_script.h` | transient DS scopes и project compiler/provider |
| `src/combat_script.cpp` | регистрация DS building blocks, report readers и emitters |
| `resources/core/scripts` | первые resource-backed combat/follow-up/retaliation программы |
| `*_test.cpp`, `headless_smoke.cpp` | сценарные тесты kernel, scripts, resume и animated/headless identity |

`combat.h` и `combat.cpp` пока намеренно держат первый срез рядом, но уже стали точками
декомпозиционного давления. Разбивать их следует по проектным понятиям, не меняя поведение:

- authoritative combat model и lookup участников;
- action/turn orchestration;
- effect execution/resolution;
- damage/retaliation policy;
- presentation adapter;
- snapshot/save adapter.

Аналогично `combat_script.cpp` со временем естественно разделяется на compiler registration,
combat-effect emit scopes, execution-report readers, follow-up scopes и retaliation scopes.

## Авторитетное и transient состояние

`combat_state` и `combat_cursor` являются авторитетными. Текущий snapshot сохраняет:

- `combat_state`;
- cursor `simul::turn_pipeline`;
- текущий `resolution_work`;
- ожидающий `player_intent`;
- монотонные allocators execution/root/instance/effect-call.

Presentation task IDs, barrier и outbox являются производным transient-состоянием. При `load()` они
сбрасываются, а cursor продолжает работу с последней детерминированной границы. Контракт
`turn_pipeline` требует заранее переводить cursor в шаг, следующий после barrier: тогда потерянная
анимация не приводит к повторному gameplay commit.

Текущий `combat::snapshot` — C++-форма состояния для проверки resume, но ещё не формат сохранения на
диске. Долговечный save забега должен дополнительно иметь:

- версию схемы;
- внешний `run_state` и его cursor;
- discriminator активной вложенной сцены;
- snapshot вложенного боя/события, если оно не завершено;
- content/config/build fingerprint;
- slot metadata для save browser.

Нужна отдельная политика объёма snapshot. Сейчас активный `resolution_work` одновременно хранится в
`combat`, а законченные executions копируются в `action_report`; для короткого kernel это просто и
безопасно, но перенос полного ledger в долговечный save может стать дорогим.

## Боевой автомат

Текущий `combat` использует `simul::turn_pipeline<combat_cursor>`. Внешний pipeline ничего не знает о
картах: все gameplay-фазы и их порядок принадлежат `cardgame`.

Устоявшаяся последовательность:

| № | `combat_group` | Смысл |
|---:|---|---|
| 1 | `turn_begin` | начать ход, подготовить enemy intent/countdown |
| 2 | `awaiting_action` | стабильная точка ожидания player intent |
| 3 | `action_begin` | открыть action cycle, применить перехват/кражу карты |
| 4 | `card_effects` | выполнить программу выбранной карты |
| 5 | `card_player_party_follow_ups` | frozen pass партии источника |
| 6 | `card_enemy_party_follow_ups` | frozen pass противоположной партии |
| 7 | `player_actor_state_tick` | обработать состояние актора, сыгравшего карту |
| — | `action_countdown` | отдельная граница продвижения enemy countdown |
| 8 | `enemy_execution` | выполнить готовый enemy intent |
| 9 | `enemy_action_enemy_party_follow_ups` | follow-up партии источника enemy action |
| 10 | `enemy_action_player_party_follow_ups` | follow-up противоположной партии |
| 11 | `enemy_actor_state_tick` | обработать состояние актора enemy action |
| 12 | `action_end` | завершить обычный player action cycle |
| 13 | `turn_end` | принудительно дренировать готовые enemy intents и начать новый ход |

`action_countdown` является явной границей между группами 7 и 8, а не четырнадцатой группой эффектов.

Особые случаи:

- карта, украденная в `action_begin`, получает `executed=false`, не запускает card effects и party
  follow-ups, но всё равно проходит tick актора и countdown;
- карта вроде `quick_strike` может не продвигать countdown;
- enemy execution, вызванный в конце хода, получает отдельный монотонный action token;
- forced enemy cycle возвращается в `turn_end` и не притворяется обычным player `action_end`;
- один и тот же порядок работает в headless и animated режимах.

`combat_phase::battle_over` уже объявлен как точка покоя, но переходы победы/поражения и типизированный
`combat_result` пока не реализованы.

## Execution, beat и authored effect

Иерархия данных:

```text
effect_program
  -> effect_beat[]
       -> authored_effect[]
            -> effect_ref
            -> target policy
            -> authored_effect_report
                 -> emitted typed instances
                 -> typed outcomes
```

Один execution — разыгранная карта, enemy ability или отдельный follow-up. Он имеет собственный
`execution_report`: id, actor, selected target, `executed`, category mask и диапазоны authored effects.

Один beat — презентационно и логически совместная порция authored effects:

1. до cue материализуются все target snapshots beat;
2. каждый authored effect получает собственный snapshot по умолчанию;
3. ненулевой binding key явно переиспользует snapshot;
4. повторное использование binding с несовместимым query является ошибкой данных;
5. authored cues разделяют один gameplay barrier;
6. после barrier скрипты выполняются в стабильном authored order;
7. один aggregated result строится на authored effect;
8. смерть проверяется после каждого typed outcome и запрещает цели участвовать только в следующем beat.

Поэтому два hits одного authored effect, два effects одного beat и два последовательных beats — разные
контракты. Их нельзя автоматически сворачивать в один суммарный damage.

## Цели

Базовые targeters:

- `target`;
- `random_target`;
- `all_targets`.

Project policy сначала формирует отфильтрованный список допустимых целей в стабильном порядке, затем
`materialize_target_sets()` замораживает конкретные snapshots. Random selection получает
детерминированную entropy; физический порядок выполнения потоков на выбор не влияет.

Текущий combat всё ещё 1-v-1:

- `player_entity = 1`;
- `enemy_entity = 2`;
- `combat_state` хранит ровно `player` и `enemy`;
- `party_members()` возвращает не более одного живого участника;
- обычная атакующая карта автоматически выбирает единственного противника.

Follow-up использует явные DS selectors:

- original opponent, если он всё ещё допустим;
- первый frozen priority opponent;
- original-or-priority fallback;
- actor исходной карты/ability для защитной реакции.

Emitter вызывается только внутри `follow_up_target_scope`, поэтому цель является частью
материализованного authored effect. Умершая или отсутствующая original target не подменяется скрыто:
fallback выбирает сам скрипт.

Для настоящей партии требуется проектное хранилище участников, сторон/фракций и стабильная target
priority policy. Это не задача `libs/resolve`.

## Типизированный effect envelope

Один `resolution_work` хранит отдельные typed stores:

- attack;
- healing;
- shield;
- attribute damage;
- status/effect requests.

`effect_instance_ref` и `outcome_ref` задают единый семантический порядок без толстого `variant`.
Знак не меняет тип операции:

- `attack(-N)` остаётся атакой/уроном;
- `healing(-N)` остаётся лечением;
- `shield(-N)` остаётся изменением щита.

Это важно для подписок, статистики, presentation и card wording: category определяется совершённой
операцией, а не знаком получившейся дельты.

DS `combat_effect_scope` работает в две фазы:

1. authored program/target scope уже подготовил frozen цели;
2. после gameplay marker `each_target` ставит `combat_target_scope`, а `emit_*` только записывает
   bounded pointer-free instances в `resolution_work`.

`emit_*` не разрешает эффект синхронно и не возвращает outcome. Прямое изменение HP/щита/статусов из
скрипта запрещено, потому что обошло бы routing, death checks, reports и presentation.

Сейчас зарегистрированы:

- `emit_attack`;
- `emit_healing`;
- `emit_shield`;
- `emit_attribute_damage`;
- `emit_status`;
- bounded `emit_retaliation_attack`;
- `emit_follow_up_attack`;
- `emit_follow_up_shield`.

Следующие emitters следует добавлять из требований реального набора карт: draw/discard/exhaust,
боевые ресурсы, перемещение/позиции, изменение инициативы и другие операции пока не должны
угадываться заранее.

## Damage routing, реакции и retaliation

Attack создаёт один `unrouted` damage root. Он один раз:

1. собирает modifiers и resistance;
2. сохраняет `damage_preparation`;
3. создаёт не более двух bounded provenance children в фиксированном порядке:
   shield leaf, затем residual health leaf.

Только routed leaves меняют stats и становятся outcomes. Shield-only hit не создаёт синтетический
нулевой HP outcome. Elemental continuation открывается один раз от terminal shield/HP leaf, поэтому
resistance не применяется повторно.

Каждый committed damage leaf, включая shield, health, zero, negative, reaction и periodic, предлагает
себя подписанным retaliation rules. Жёсткая C++-граница только одна:

- один `(triggering instance, rule)` может сработать не более одного раза;
- retaliation и любой descendant его lineage не могут retaliate.

Решение действительно испустить ответ принадлежит rule script. Текущий `thorns_retaliation` читает
trigger outcome и вызывает bounded `emit_retaliation_attack`.

Retaliation — immediate continuation конкретного instance внутри текущего execution/report. Он получает
отдельный вложенный attack cue/result/finished, но не получает новый action, countdown, ActorStateTick
или follow-up window. Его outcome не дублируется во внешнем authored-effect presentation result.

## `action_report` и follow-up

Один большой action sequence имеет append-only ledger из полных `resolution_work`. Для каждого этапа
открывается segment:

```text
execution
  -> source_party_follow_ups
  -> opposing_party_follow_ups
  -> actor_state_tick
  -> reset
```

При открытии segment фиксируется длина входного prefix. Этап:

- читает только executions, завершённые до его начала;
- добавляет ноль или больше executions в собственный output range;
- не видит собственный output;
- после seal делает output доступным следующему этапу.

Таким образом, для player action:

- card effects создают report для player-party pass;
- enemy-party pass видит card report и завершённые player follow-ups;
- ActorStateTick видит card + player follow-ups + enemy follow-ups;
- после ActorStateTick report сбрасывается;
- возможный последующий enemy execution начинает новый report.

Порядок участников party pass замораживается из:

```text
(combat seed, action token, side domain, stable entity id)
```

Разные worker/контейнерные порядки не меняют gameplay order. Живость участника повторно проверяется
непосредственно перед правилом.

Сейчас registry временный: для каждого живого участника C++ создаёт один optional attack listener и
ищет `scripts/follow_up_attack`. Реальные component/resource rule lists, порядок нескольких правил
одного актора, charges/cooldowns и различие «правила нет»/«объявленный ресурс потерян» ещё не готовы.

`ActorStateTick` пока только наблюдает полный report и пишет trace. Будущий фиксированный порядок
DoT -> negative -> positive programs должен создавать обычные typed executions/outcomes, а не напрямую
менять stats.

## devils_script resources

`combat_effect_script_compiler` поддерживает четыре root scopes:

| Scope | Назначение |
|---|---|
| `combat_effect` | emit typed instances по frozen target snapshot |
| `execution_report` | read-only обход execution metadata, instances и outcomes |
| `follow_up_rule` | прочитать frozen action-report prefix, выбрать цель и построить authored beats |
| `retaliation_rule` | прочитать один triggering damage outcome и при необходимости испустить response |

Текущие shipped resources:

| Resource | Назначение |
|---|---|
| `scripts/scripted_strike` | DS attack на каждую цель |
| `scripts/scripted_guard` | DS shield на self |
| `scripts/thorns_retaliation` | первый resource-owned retaliation decision |
| `scripts/report_probe` | проверка read-only report scope |
| `scripts/follow_up_attack` | первый follow-up с original-or-priority targeting |

Authored recipe хранит стабильный hash ресурса. Внешний `combat_effect_script_provider` разрешает его
в уже загруженный compiled container. Поскольку snapshot не владеет container/pointers, fresh и resumed
combat должны получать совместимый внешний catalogue.

## Presentation

Gameplay публикует только project-specific `presentation_command`:

- `start` открывает cue;
- gameplay checkpoint разрешает authoritative commit;
- `result` несёт агрегированные outcome values;
- finished checkpoint разрешает перейти к следующему шагу.

Presentation subjects сейчас различают player/enemy attack, elemental reaction, returned damage,
shield damage, healing, shield, attribute damage и effect.

В animated режиме barrier защищён watchdog. В headless режиме checkpoints не регистрируются, поэтому
тот же cursor проходит commit сразу. Gameplay state, reports и ids обязаны совпадать между режимами.

Реального broker/scene adapter пока нет: тесты забирают outbox напрямую и эмулируют checkpoints.

## Результаты architecture review

Существующее ядро следует сохранить в `cardgame`, но окружить его внешним проектным слоем.
Рекомендуемая структура следующего этапа:

```text
cardgame_application
  owns application_state + loading transitions

run_session
  owns run_state + run_cursor + save/checkpoint policy

scene controllers
  HexWorld / Combat / Shop / NarrativeEvent / Reward / Summary
  consume intents and return typed results

combat_session
  adapts encounter snapshot to the existing combat kernel
  returns combat_result to run_session
```

Нужные типизированные границы:

- `start_combat(encounter_snapshot)` и `combat_result`;
- `start_shop(shop_snapshot)` и `shop_result`;
- `start_event(event_id, event_cursor)` и `event_result`;
- `reward_request[]` и единый `reward_resolution`;
- `scene_transition_request` с generation/token для защиты от stale completion.

Не рекомендуется строить один гигантский enum со страницей каждого события. Внешний автомат должен
знать, что активна `NarrativeEvent`, а конкретный event graph хранит свой локальный cursor.

Также не следует начинать реализацию внешней оболочки до первого содержательного дизайна карт целиком.
Карты должны выявить недостающие combat building blocks, но общий `run_session` и typed scene result
можно проектировать независимо от конкретных чисел баланса.

## Результаты code review и текущий долг

### Блокирует следующий крупный вертикальный срез

1. Бой не завершается: `battle_over` недостижим, нет victory/defeat policy и `combat_result`.
2. Модель участников жёстко 1-v-1; party passes пока проверяют порядок, но не настоящую партию.
3. Карты и enemy attack собраны C++ `card_kind`/`switch`; нет resource schema
   `card -> beats -> authored effects`.
4. Follow-up использует один C++ fixture вместо registry/component storage.
5. `ActorStateTick` не исполняет status programs.
6. `player_intent` не содержит card instance, selected targets, payment и choices.

### Нужно решить до долговечных saves и большого контента

1. Snapshot копирует тяжёлые `resolution_work` в action ledger; нужен бюджет/compact save policy.
2. `combat_state::trace` растёт без ограничения и входит в authoritative state.
3. `last_resolution()` фактически означает «последний текущий execution»; после follow-up это уже не
   обязательно opening card. API следует назвать точнее или предоставить lookup через sealed report.
4. Неуспешный follow-up script с нулём emitted effects уже расходует execution id. Это детерминированно,
   но семантику gaps нужно либо принять явно, либо выделять id после prepare.
5. `action_report_input()` возвращает transient span во vector. Текущий вызов безопасен, потому что
   скрипт заканчивается до append, но scope/view не должен переживать мутацию ledger.
6. Лимиты resolver/script/follow-up и barrier watchdog пока являются локальными константами, а не
   валидируемой project/content configuration.
7. Presentation subjects местами выводят player/enemy из фиксированных ids; с партиями источник должен
   определяться через side/faction snapshot.
8. `combat.cpp` и `combat_script.cpp` объединяют слишком много причин для изменения.

### Что уже является хорошей основой

- gameplay не зависит от времени прихода presentation checkpoints;
- snapshot не содержит transient pointers и render tasks;
- authored effects, emitted instances и outcomes имеют явные диапазоны и типы;
- target snapshots материализуются до cue;
- signed semantics не теряют исходную категорию;
- damage modifier/resistance применяется один раз до routing;
- retaliation recursion запрещена на общем уровне provenance;
- follow-up видимость данных задана sealed stage boundaries;
- headless/animated и resume проходят один и тот же authoritative путь;
- budget overflow и несовместимые bindings завершаются громкой ошибкой.

## Необходимые инструменты полного проекта

Помимо combat kernel понадобятся:

- внешний application/run FSM;
- versioned run save и отдельный profile save;
- save-slot index/metadata и continue policy;
- hex map generation, adjacency/reachability и boss/floor gates;
- run-effect stages вокруг движения, tile event и наград;
- event graph + choice intents;
- shop inventory и атомарные buy/sell transactions;
- общий reward pipeline;
- content catalogue для cards/decks/characters/enemies/encounters/floors/tiles/events/shops/rewards;
- deck/hand/discard/exhaust, card instances и payment/resource model;
- party/faction storage и target-priority policies;
- combat rule registry, status storage/tick programs и battle completion;
- broker adapters и scene/UI controllers;
- run statistics для summary и achievements;
- headless simulation/balance harness поверх тех же content resources.

## Ближайший порядок работы

1. Зафиксировать audit decisions и не расширять API по предположениям.
2. Спроектировать первый реальный набор карт и выписать требуемые mechanics/building blocks.
3. Сопоставить требования с существующими typed emitters, reports, targets и stages.
4. Добавить только обнаруженные envelope operations и тесты их семантики.
5. Ввести resource schema карт/abilities и убрать C++ `card_kind` recipes.
6. Добавить настоящий rule registry и status programs.
7. Обобщить участников до party/encounter model и завершить `combat_result`.
8. После стабилизации боя подключить внешний `run_session`, scenes/loading и долговечные saves.

## Сборка и тесты

Из корня репозитория:

```bash
cmake --build build --target \
  cardgame_effect_program_test \
  cardgame_follow_up_test \
  cardgame_action_pipeline_test \
  cardgame_typed_effect_test \
  cardgame_combat_script_test \
  cardgame_execution_report_script_test \
  cardgame_headless_smoke \
  resolve_pipeline_test

ctest --test-dir build -R 'cardgame_|resolve_pipeline_test' --output-on-failure
```

Основные проверки покрывают target bindings, follow-up ordering/report visibility, normal/stolen/forced
action cycles, typed signed effects, shield routing, retaliation, script resource lookup, nested
presentation checkpoints, mid-resolution resume и animated/headless identity.
