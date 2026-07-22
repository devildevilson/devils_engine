# devils_engine — план развития (обзор)

Документ-обзор: свести воедино два направления мыслей и разложить по скорости достижения, чтобы
СНАЧАЛА увидеть общую картину, а детальное планирование каждой задачи делать отдельно.

Два направления:
- **A. Движковый слой** — оконный менеджмент, настройки, тумблеры потоков, save/rng, трассировка.
- **B. Контракт внешнего проекта** — что проект определяет в C++, а что описывает в конфигах.

---

## Цель проекта (зафиксировано 2026-07-06)

Движок считается состоявшимся, когда внешний проект — это config/script-only + быстрая тонкая
обвязка на C++. Конкретно проект делает ТОЛЬКО:

**C++ (тонкая обвязка):**
1. определить базовые геймплейные функции — `add_gold`, `get_strength`, `each_neighbor` и проч.;
2. определить базовый набор компонентов энтити;
3. зарегистрировать базовые функции создания энтити (каркасы префабов);
4. зарегистрировать функции как строительные блоки в devils_script;
5. задать мелкие новые фичи, которых нет в основном движке.

**НЕ C++ (конфиги tavl + скрипты):**
6. таблица скриптов на основе базовых функций и devils_script;
7. конфиги префабов (наследование: огр = «большой гоблин» + переопределения);
8. описание FSM (mood);
9. описание GOAP AI (acumen);
10. интерфейс на lua (visage).

**Критерий готовности:** generic lifecycle/потоки/окно/реестры живут в devils_engine, а
`tile_frontier/simulation.cpp` остаётся тонким host-adapter. Проектная сцена не переносится в engine:
map/actor/draw/sound policy сворачивается за project-фасадом `tile_frontier_game` рядом с регистрациями
и конфигами. Целевая модель инвертирует прежнее соотношение boilerplate:специфика ≈ 3:1.

**Актуальная оценка на 2026-07-19 (три слоя):**
- реестры по концернам (act/acumen/mood/aesthetics-serial/demiurg/catalogue) — каркасы готовы и
  обкатаны; catalogue MT-декораторы collect/elect и живой tile_frontier commit pipeline
  готовы; config-loaded void actions, единый `act::building_blocks` facade и 128-byte inline payload
  тоже закрыты. Остались общий trace service/UI и расширение
  bounded codecs только под фактические новые сигнатуры.
  Replay/binary-format не входят в ответственность catalogue;
- config/data-first вертикальный срез СОБРАН: FSM и GOAP загружаются из tavl через demiurg, GOAP-метрики
  содержат inline-ds выражения, `libs/prefab` умеет data/list/callback/reference/custom + наследование,
  tile_frontier создаёт actor и food из `prefab/*.tavl`, а generic stats-accessors уже вынесены в
  `libs/act`. App-shell и минимальный system runner обобщены; project gameplay свёрнут за
  `tile_frontier_game`, не перенося map/actor/draw/sound policy в engine;
- devils_script подключён: `act::script_function<bool/number/void>` работает, есть per-worker ds-context,
  `act::call_context` переносит именованные in/out args и списки между native и ds через reusable
  per-worker `execution_scratch`; есть config-only GOAP actions, deterministic DS RNG/target scope и
  натив `spawn_at`. Не закончены string/object/vector-маршалинг и custom codecs, но расширять их надо
  по требованиям spawn/player/cost, а не заранее. Lua остаётся UI/guest backend, не mutating backend.

**Решение по devils_script ↔ act (уточнено 2026-07-13).** `libs/act` НЕ удаляется: это стабильный
типизированный фасад, который скрывает источник функции (прежде всего native ↔ devils_script) от
acumen/mood/UI и других потребителей. Но контракт вызова act должен приблизиться к фактической модели
ds, а не ограничивать её одним C++ return. devils_script умеет автовывод сигнатур
(`register_function<&fn>`), типизированную навигацию по скоупам, итераторы
(`register_function_iter` = each_neighbor), scalar in/out-аргументы, списки с filter/map/sum,
script-in-script и `container::describe`. Отсюда:
- **`ds::system` = единая точка регистрации нативных строительных блоков**, потому что скрипты обязаны
  их видеть. `act::registry` хранит/резолвит публичные gameplay-функции и их категорийный контракт, но
  не требует второй независимой реализации той же нативки;
- **act = фасад источников + проверенный контракт вызова**: имя → категория/сигнатура → native или ds
  implementation; load-time проверки («guard обязан быть предикатом») и кэширование типизированных
  указателей для acumen/mood сохраняются;
- ✅ **контекст разделён на две части (2026-07-13)**: immutable execution context (world/scopes,
  deterministic RNG inputs, tick) и отдельный mutable набор аргументов/результатов вызова.
  `act::call_context` поддерживает до стандартных 8 ds input/out/in-out values inline, result и списки;
  caller владеет `execution_scratch{ds::context,call_context}` и переиспользует capacity на worker-е.
  Подсистемы встраивают этот блок в свой scratch (`acumen::execution_scratch` добавляет A*+cache), а не
  используют глобальный/TLS-раздатчик. `script_function` bind/collect-ит ds args/lists,
  native получает тот же объект. Работают bool/integer/number/entity-id основы; string/object policy и
  `vec3` (>16-byte ds stack slot при double) остаются частью fixed-point/marshalling pass;
- `intent` остаётся границей входа в детерминированную мутацию. Старый `effect_sink` удалён: аудит не
  имеет ни implementation, ни вызовов `emit`: live effects идут через catalogue, а spawn использует
  отдельный project-owned `spawn_sink`, поэтому seam следует удалить, а не адаптировать;
- **GOAP из конфига:** планировщик работает с дискретными состояниями; непрерывное значение проект
  режет ds-предикатами на смысловые полосы (`low_hp = health < 25`, `damaged = health < 50`,
  `healthy = health > 90`). Config effects уже живы; динамические числовые cost formulas не являются
  текущей целью GOAP. tavl — парсер и конфигов, и ds → скрипт в конфиге = просто ещё одно поле;
- **статус интеграции:** `act::building_blocks`, DS RNG/target scopes, config effects и базовый
  `describe` готовы; FSM/GOAP читаются из ресурсов, production fallback и параллельная native-таблица
  удалены. Следующий срез определяется реальным потребителем: player intents либо spawn signatures;
  числовые ds-функции расширять, когда появятся отношения/оценки между двумя сущностями;
- **флаг на горизонте:** ds считает в double (`init_math`, включая тригонометрию); архитектура
  детерминизма требует fixed-point — механизм кастомных арифметических типов в ds уже есть, но
  проверить пригодность встроенной математики надо ДО того, как на ds напишется сотня скриптов.

---

## Исходные пункты (откуда это взялось)

Дословный (слегка перефразированный) список из обсуждения — чтобы дальше по тексту читались ссылки
`A-n` / `B-x` и было видно первоисточник.

### Направление A — движковый слой
- **A-1.** `rng_state` и `timestamp` протащить в файл сохранений — необязательно, но приятно продолжить
  UI-состояние 1:1 после загрузки. В будущем — сериализовать простую lua-таблицу (без функций, с
  корректным резолвом вложенных таблиц).
- **A-2.** Паттерн `local s2 = s1 + 1` (`rng_state + int`) — сразу прокрутить состояние N раз вперёд.
- **A-3.** Текущий оконный менеджмент уйдёт в общие системы devils_engine: пользователь движка не должен
  переопределять всю проводку main-треда с окном и рендер-потоком.
- **A-4.** Сохранить запуск headless + вообще без render-потока + добавить запуск без звукового потока +
  из-за этого увеличивать число worker-потоков. Движковые настройки тоже уйдут в devils_engine. Основные
  настройки (размер окна, качество) — БЕЗ перезапуска exe; выключение сим-потока — перезапуск допустим.
- **A-5.** Расширение `libs/catalogue`: упаковать крупные функции (`update_ui`, `apply_intent`) и в
  рантайме (по аргументам/конфигу) трассировать скорость выполнения (можно по уровням) → графики в UI.

### Направление B — контракт внешнего проекта
Внешний проект определяет:
- **B-а.** Список базовых геймплейных функций (напр. `get_gold`): предикаты, значения, получение
  объекта, эффекты.
- **B-б.** Список компонентов энтити: какие сериализуем для сохранения/сетевого стейта, какие
  восстанавливаем.
- **B-в.** Список систем и конкретные действия над компонентами → пайплайн обработки компонентов.
- **B-г.** Обёртки и доп-фичи для основного движка (напр. дополнительные шаги в симуляциях — это A-5).
- **B-д.** Проект определяет один или несколько простых числовых aggregate-компонентов характеристик;
  devils_engine даёт проверку формы, шаблонные init/accessors и опциональную UI-metadata полей.
- **B-е.** То же для состояний — список флагов, возможно с меткой expiration time.
- **B-ё.** Функции создания конечного энтити (гоблин = N компонентов + дефолты) — часть базовых функций
  + особые значения в конфигах.
- **B-ж.** ??? (ещё пункты) — кандидаты: связи/отношения энтити; события/триггеры; время/календарь;
  что экспонируем в UI + локализация; политика save/replay.

**В конфигах (tavl) — всё остальное:** описание GOAP, FSM-состояния энтити, типы существ (каркас = C++
функция B-ё + список значений), локализация, анимации и прочее. devils_engine даёт инструментарий для
такого описания.

---

## Стержень (общая картина)

Движок — это **набор РЕЕСТРОВ ПО КОНЦЕРНАМ**, все ключуются `string_hash` и резолвятся на загрузке:

| Концерн | Реестр | Статус |
|---|---|---|
| Геймплейные функции (предикат/значение/объект/эффект) | `libs/act` — `act::registry` | `building_blocks`, native + ds bool/number/void, args/lists и describe работают; codecs расширять по потребности, мёртвый effect-sink удалить |
| Компоненты + сериализация | `aesthetics::serial` — `SERIALIZABLE_COMPONENT` | готов, с тестами |
| GOAP | `libs/acumen` | готов, потребляет `act` |
| FSM состояний энтити | `libs/mood` | готов, потребляет `act` |
| Ресурсы/моды | `libs/demiurg` | готов; type-from-path + discovery/manifest override + tavl list pattern + stable `resource_handle` + lua `request`/`require`/`find`/`filter` |
| Trace/log + deferred MT-dispatch gameplay-вызовов | `libs/catalogue` | introspection + collect/elect wrappers/executors + inline journal + config-loaded effects готовы; service/UI pending; effect_sink сюда не относится |
| Replay/netcode/save-лог | отдельный будущий слой | не проектировать через catalogue автоматически |

**Итог модели:** «проект» = **ЗАПОЛНИТЬ реестры** (C++ native: `reg`/`register_type`/
`SERIALIZABLE_COMPONENT`) + **ОПИСАТЬ конфиг-деревья** (data-сторона, tavl). «Инструментарий движка» =
эти registration-API + формат tavl + seam'ы `exec_context`/`intent`. Изобретать новое почти не нужно —
закрыть несколько пробелов и генерализовать app-shell.

---

## Что УЖЕ готово (не переделывать, только «признать контрактом»)

- **B-а** → `act::registry` (типы effect/predicate/number/string/object, native/ds implementations,
  execution context, `intent`, `describe`). Проект декларативно регистрирует building blocks один раз
  через готовый `act::building_blocks`, затем устанавливает их в ds и act registry.
- **B-б** → `aesthetics::serial`. «Восстанавливаем» = derived (не пишем — выводим после load). Это
  водораздел replicated-vs-derived.
- **Карты действий ввода** → `input::events` (именованные действия escape→quit и т.п. уже прокинуты).
- **Классификация репликации** (det/input vs non-det/delta) → спроектирована; проект лишь помечает.
- **Ресайз окна вживую** (без рестарта) → сделано; свопчейн пересоздаётся, проекция от живого размера.

---

## Легенда: две оси

**Усилие:** 🟢 быстро (часы) · 🟡 средне (день-два) · 🔴 крупно (архитектура, несколько сессий).
**Необходимость:** **[необходимо]** — часть целевой модели · **[QoL]** — удобство, можно отложить.

QoL-набор (пока только эти): A-1 (UI-стейт в save), A-2 (`rng_state + int`), A-4-звук
(`sound.enabled` + воркеры). Всё остальное — **[необходимо]**.

---

## 🟢 Быстрые

1. ✅ **`rng_state + int` — прокрутить состояние N раз вперёд** (A-2) — **[QoL, готово]**.
   `sol::overload` на meta-`+`: `(rng,rng)`=микс (есть), `(rng,int)`=advance N. Нюанс: сейчас `prng64` —
   splitmix hash-step, «N вперёд» = N применений (O(N)); для больших N перейти на splitmix-counter
   (`state += gamma·N`). Задокументировать семантику `+int`=advance / `+rng`=mix.

2. ✅ **`sound.enabled` + динамический `worker_threads_reserved`** (A-4, топология) — **[QoL, готово]**.
   Зеркало `render.enabled`: пропустить создание `sound_simulation` + его поток. `reserved` считать от
   числа ВКЛЮЧЁННЫХ движковых потоков, не статиком. Топологические настройки → рестарт движка.

3. ✅ **Признать B-а / B-б явным контрактом** (документация, 0 кода) — **[необходимо, готово]**.
   Зафиксировать в AGENTS/README: «список функций» = регистрация в `act::registry`, «список
   компонентов» = `SERIALIZABLE_COMPONENT` + пометка replicated/derived. Разблокирует остальное.

---

## 🟡 Средние

4. **catalogue perf-tracing** (A-5, B-г) — **[необходимо]** · **приоритет: РАНО**.
   *Автор: заняться пораньше — надо научиться профилировать код и следить за бюджетами.*
   RAII-таймер ПО УРОВНЯМ (вложенные скоупы) → SERVICE-канал catalogue → графики в UI (метрики уже в
   env). Реализуется непосредственно через catalogue domain/introspection + consumer-модель; старый
   неиспользуемый `utils::perf` вынесен в `exclude/`. Это же и B-г «доп-шаги симуляций».
   **Статус 2026-07-13:** базовый доменный logging/trace слой уже работает; `demiurg` подключён как
   отдельный домен (`flow` = ресурсные переходы, `trace` = вход/выход + время крупных функций), actor
   update размечен вложенными catalogue fn-traits; `app.perf_stats()` уже отдаёт rolling samples, а
   shipped Lua UI рисует графики actor-фаз. Остаются общий service-channel/consumer для произвольных
   доменов, бюджеты и перенос остальных подсистем на тот же паттерн. Actor-local UI больше не считать
   незакрытой частью этого пункта.

5. **B-ё — prefab / спавн энтити** — **[необходимо]** · **вертикальный prefab-срез ГОТОВ
   (2026-07-12)**. Он связывает act + компоненты + tavl:
   - **C++ задаёт каркас** (какие компоненты) + значения по умолчанию, которые каркас получает БЕЗ
     явного указания.
   - **tavl-конфиг задаёт конкретный тип префаба и значения**; префаб может наследовать другой
     (огр = «большой гоблин» с переопределением локальных полей).
   - `libs/prefab::prefab_registry<SpawnArgs>` поддерживает data/list/callback/reference/custom,
     field-level inheritance и derived-компоненты через `on_construct`; tile_frontier грузит actor/food
     из `prefab/*.tavl` через demiurg.
   - Низовой script-примитив `spawn_at(prefab,x,y)` готов и покрыт тестом. Следующий узкий slice —
     простые spawn-point entities для еды и script-facing выбор точки; spatial filter/pick, cooldown,
     capacity и универсальный spawner manager остаются отложенным дизайном (п.15).

6. ✅ **B-д — generic stats над проектным агрегатом** — **[необходимо, базовый механизм готов
   2026-07-13]**. Проект объявляет предельно простой агрегат:
   `struct stats { int32_t a; float b; int64_t c; double d; ...; };`; движок даёт шаблонную надстройку,
   а не навязывает универсальный динамический контейнер.
   - `act::numeric_stats_aggregate` проверяет: тип является aggregate, поля рефлексируются, каждое поле
     принадлежит разрешённому набору числовых типов; отдельно решить требования standard-layout /
     trivially-copyable там, где они реально нужны save/network, а не запрещать полезный агрегат молча.
   - `initialize_stats<T>()` сохраняет C++ default member initializers плоского агрегата; overload с
     callback проходит `reflect::for_each` и задаёт каждое поле. Конечный проект явно выбирает удобный
     режим без пользовательского конструктора. `make_stats`/`register_stats` дают aggregate init + scope
     getter + read/add без ручного дублирования полей; механизм вынесен из tile_frontier в `libs/act`.
   - Префиксов нет: `stats.abc`, `stats = { add_abc }`, `combat_stats.abc`. Одинаковые имена полей
     перегружаются ds по типу возвращённого `stat_scope<T>`; два агрегата проверены одновременно.
   - Metadata (display/loc key, unit, «положительное» направление для UI) держать как опциональную
     надстройку над полями, не смешивать с самим POD-подобным агрегатом.

7. **Временные координаты + настраиваемый игровой календарь + отложенные эффекты** (B-ж) —
   **[необходимо]** · **clock/calendar/pause основа готова 2026-07-13; очередь эффектов pending**.
   - **Engine time** — непрерывная движковая шкала, не паузится вместе с gameplay; это же timestamp,
     доступный UI/анимациям/служебным системам.
   - **Presentation time** — real-rate шкала world-анимаций; может паузиться меню независимо от engine/UI.
   - **Game time** — масштабируемая шкала симуляции, останавливается вместе с gameplay systems.
     Gameplay duration/expiry живут здесь. `game_time_scale` задаёт рациональный nominal mapping
     `engine_duration ↔ game_duration` (например 1 реальная секунда = 1 игровая минута). Абсолютные
     timestamps намеренно не конвертируются: паузы делают такое отображение неоднозначным.
     В tile_frontier коэффициент задаётся `time.game_seconds / time.real_seconds` в `app.tavl`, а actor
     gameplay получает уже масштабированную дельту game clock.
   - **Turn** — ортогональная дискретная координата (`turn_index/duration/deadline`): влияет на пошаговые
     эффекты, но не двигает анимации.
   - **Calendar** — отдельный тип timestamp/deadline и неизменяемый после project startup
     `calendar_clock`. `time.calendar.source = game_time|turn` выбирает только источник даты, не удаляя
     вторую координату. Для turn-source настраивается `seconds/days/months/years_per_turn`; month/year
     используют календарную арифметику, короткий месяц clamp-ит день, каждый результат считается от epoch.
   - **Конвенция конфигов:** duration без явного игрового/turn-квалификатора означает nominal real
     (`engine_duration`); gameplay-loader конвертирует её через project `game_time_scale`. Явные
     calendar/game duration и `turns` остаются в своих доменах.
   - Практическое внутреннее разделение game time: счётчик полных `day` + время внутри дня в секундах;
     нормализация использует `hours_per_day`. Секунда/минута/час остаются duration-единицами, а
     day/month/year — календарной проекцией. Так проект может использовать только 24-часовой цикл,
     только дату day/month/year или обе части без обязательного «земного» календаря.
   - `calendar_policy` задаётся проектом: `hours_per_day`, список `days_in_month`, epoch и источник.
     Преобразования `calendar_timestamp ↔ {second,minute,hour,day,month,year}` и границы реализованы;
     пустой список месяцев оставляет только absolute day + time-of-day.
   - `simul::pause_state` разделяет gameplay/presentation pause; компоненты не «паузятся», scheduler
     просто не запускает мутирующие gameplay-фазы. tile_frontier экспонирует `app.set_paused/paused`.
   - Timestamp/duration/deadline типизированы clock-domain (`engine`, `presentation`, `game`, `calendar`), поэтому разные шкалы
     нельзя случайно сравнить/сложить. `calendar_policy` реализует day+seconds и опциональную month/year
     проекцию. Следующий зависимый срез — очередь отложенных эффектов и expiry для п.8.

8. ✅ **B-е — состояния/флаги энтити + expiration** — **[необходимо, ядро готово 2026-07-18]**.
   `aesthetics::flag_set`: сортированный `(hash, remaining game_duration)` — COUNTDOWN вместо
   абсолютной даты (телам ds-блоков и предикатам не нужен доступ ко времени; пауза/scale действуют
   через game-дельту). ds-блоки `set_flag`/`clear_flag`/`has_flag`, sweep-фаза, сериализация,
   живой потребитель sated. Детали — «Дела ближайших сессий» п.2. Остаток: модификаторы с payload,
   calendar/turn-сроки.

9. **A-1 — UI-состояние в save** — **[QoL]**.
   Durable = состояние `ui_rng` (4×uint64) + `ui_timestamp` в main + lua-upvalues. Нужен сериализатор
   «простой lua-таблицы»: без функций, резолв вложенных + защита от циклов, `rng_state` писать как число.
   Ложится на слоистую sink-схему (`dump_<side>` рядом с `dump_world`).

10. **Настройки: водораздел ЖИВЫЕ vs РЕСТАРТ** (A-4) — **[необходимо]**.
    Параметрические (размер окна, качество) — вживую; топологические (какие потоки есть) — рестарт.
    Ресайз уже вживую → обобщить на остальные параметры.

---

## 🔴 Крупные (архитектурная генерализация)

11. ✅ **B-в — декларативный пайплайн систем** — **[необходимо, согласованный первый контракт готов]**.
    Список систем + действия над
    компонентами как упорядоченные фазы над view/query. Сейчас select → MT cognition/record →
    parallel collect → ST structural elect → FSM → integrate живут явной traced-последовательностью
    в tile_frontier; обобщать ownership фаз только после второго живого потребителя.
    **ПРОГРЕСС 2026-07-16 — примитивы движковые:** `elect_buffer`/`collect_buffer`/`interaction_arena`/
    `message_buffer`/`message_registry`/`worklist_system`/`budget_clamp`/`template_system[_mt]` — в
    `libs/aesthetics`; commit-фаза — `simul::commit_calls`. Добавлены **лямбда-системы**
    `aesthetics::make_map_system[_mt]<Comp...>(…, fn)` — map-система ИЗ ЛЯМБДЫ без подкласса (цель «проект
    задаёт обработку в удобной форме»; single + MT; +2 теста в aesthetics_pipeline_test). ОСТАЁТСЯ: (а)
    явный API «список фаз» (система-лист) — упирается в РАЗВИЛКУ per-frame контекста: `basic_system::
    update(size_t time)` слишком узок (системам нужны dt/pool/board), решить `update(time)` + члены-входы
    (текущий паттерн) vs `update(frame_context&)`; (б) обернуть НЕ-map сканы (build_sense_tree=reduce,
    resolve_eating=delete, build_actor_batch=gather, maintain_food=create) — нужны иные shape'ы систем;
    (в) ужать слайс на этих системах. cognition-«scheduler» уже растворён (budget_clamp+worklist_system).
    ВЫВОД по shape'ам (2026-07-16): `make_map_system` (single) покрывает iterate/reduce/gather,
    `make_map_system_mt` — параллельный map; отдельные примитивы не нужны, create/delete (maintain_food/
    структурные) остаются шагами. Переведены на лямбда-системы: `build_sense_tree` (reduce→kd),
    `cognition`-SELECT (reduce→due_), `resolve_eating` (скан→буферы, структурное удаление в обёртке); resume
    bit-identical, eat 15/222. **РАЗВИЛКА per-frame контекста РЕШЕНА:** `frame_context` НЕ нужен — всё
    инъектируется в конструктор (лямбда=захват), per-frame (dt) через захваченный указатель/член;
    `update(size_t time)` остаётся ⇒ phase-list = `vector<basic_system*>`+run.
    **Таксономия задач эффектов по MT-безопасности** (память entity-interaction-model): (A) self-мутация→
    параллельный map; (B) contended claim→elect; (C) кумулятив к цели→collect; (D) структурное (create/
    remove компонента/сущности)→сбор запросов в MT + однопоточная поздняя фаза (resolve_eating=эталон D).
    **ЗАВЕРШЕНО (2026-07-17, решение автора):** phase-list = ЯВНАЯ traced-последовательность в `update()`
    (compile-time fn_traits, zero-cost — это и есть проектная регистрация порядка фаз; runtime-`vector<
    std::function>` отвергнут как маргинальный). `build_actor_batch` остаётся методом `actor_batch::build`
    (когезия: batch строит себя из мира; sim→render marshalling, не world-система; нет тест-покрытия —
    рисково конвертить). `maintain_food` (create) — шаг. integration/drives остаются subclass-системами
    (per-frame dt членом); конверсия в лямбды опциональна. **ПРОГРЕСС 2026-07-17:** добавлен минимальный
    `aesthetics::run(pool,time,systems...)`: range-query, explicit worklist, обычный ST `update(time)`
    как одно pool-задание и `single(fn)` сначала
    enqueue-ятся в одну независимую фазу, затем выполняются за одним `compute/wait`; dependency graph и
    автоматический анализ read/write-set намеренно отсутствуют. `template_system_mt`/`worklist_system::run`
    сохранены как compatibility facade, но lifecycle делегируют общему runner. Первая живая группа —
    `integration + drives` (общий barrier; read velocity, disjoint write position/stats).
    ИТОГ п.11: примитивы движковые
    (elect/collect/message/worklist/template+лямбда-системы/commit_calls + catalogue typed executors),
    3 скана→системы, порядок фаз=код. `simul::commit_calls` остался generic-примитивом;
    live actor effects теперь коммитятся catalogue collect/elect executor-ами (п.16).
    Исторический `simul::cognition_scheduler` уже растворён в `budget_clamp` + `worklist_system`, а
    коммуникация/commit разложены по `aesthetics::message_buffer` и catalogue typed journals. Отдельный
    runtime scheduler graph из этого больше не следует: compile-time порядок фаз остаётся проектным до
    появления второго живого потребителя.

12. ✅ **A-3 / A-4 — окно + настройки → общий devils_engine (app-shell слой)** — **[необходимо,
    готово 2026-07-13]**.
    Пользователь не переопределяет всю проводку main↔окно↔render. Переиспользуемое ядро: `window_policy`
    + аккумулятор событий + контракт resize→только-свопчейн + гейты draw/mute + `app_state` FSM.
    `simul::app_runtime<Traits>` владеет bootstrap/broker/main и расширяемым type-erased
    `worker_systems<Broker>` вместо фиксированных четырёх slots. Runtime сам делает broker wiring,
    typed registry, init, jthread start/stop/join и shutdown-order. `make_standard_workers` собирает
    optional render/sound/assets; отсутствие assets теперь действительно не создаёт объект/поток.
    Topology callback корректирует task-pool по фактическому числу стандартных и project workers.
    `tile_frontier` traits сведён к типам + одной worker factory, ручной `bind_systems` удалён.
    **Поддерживаемая форма host:** `game_host` остаётся CRTP/shared-state каркасом поверх свободных и
    шаблонных runtime helpers, без глубокой виртуальной иерархии. Конкретным state владеет проект
    (его тип может быть неполным в публичном header), а виртуалы `main_system` остаются тонкими
    out-of-line форвардерами в project TU — это не случайный boilerplate, а граница инстанцирования
    host-шаблона. Generic UI startup/standard bindings принадлежат host; project добавляет только
    gameplay hooks и своё policy-состояние.

### Перенос общей части из `tile_frontier`: текущий итог

План закрыт в согласованных границах. Gameplay policy не переносится в engine; единственный
отложенный пункт ждёт второго живого потребителя:

1. ⏸️ **Обобщить ownership deferred system pipeline после второго потребителя** — сейчас tile_frontier
   явно владеет executor-ами, барьерами и порядком collect/elect/FSM. Это уже чистая
   последовательность над `worklist_system`, catalogue executors и `atomic_pool`; не выносить её в
   runtime-graph заранее. Когда появится второй gameplay slice, выделить общий typed owner/helper;
   project оставляет strategy types и явный compile-time порядок фаз.
2. ✅ **Разделить engine lifecycle и project gameplay в `tile_frontier::simulation`** — общий main host
   владеет boot → loading → game, startup/runtime-state resources, main-frame и runtime settings;
   проект подключается через callbacks наподобие `register_project_bindings`,
   `begin_project_loading`, `update_gameplay`, `publish_project_frame`.
3. ✅ **Закончить границу engine events ↔ project bindings для текущего app-shell** — общие Lua API окна, ресурсов, состояния
   приложения, логирования и загрузки регистрируются движком; в проекте остаются только gameplay API,
   actor stats и конкретная presentation/sound policy.
4. ✅ **Поднять scheduler времени и пауз над системами** — общий runtime решает, какие gameplay и
   presentation фазы запускать при текущих `app_state` и pause masks; компоненты и project systems не
   размазывают эти ворота по своим update-функциям.
5. ✅ **Дочистить стандартную resource/loading orchestration** — общий слой управляет resource-set
   transitions, запросами startup-наборов и базовой readiness; состав сцены, чанки и дополнительные
   project-условия загрузки остаются в `tile_frontier`.

### Итог разгрузки до минимальной игровой обвязки (2026-07-18)

Промежуточный пятишаговый остаток полностью закрыт: `game_host` владеет lifecycle
`boot → loading → game`, часами, pause gates, окном, главным frame loop и стандартными Lua API;
project config/resources и игровая сцена отделены от него. Список ниже сохраняет итог и границы
этого переноса:

1. ✅ **Стандартное состояние host + UI lifecycle (2026-07-17).** Вынесены из `tile_frontier::simulation_init` в
   `simul::standard_game_state<Broker>` окно/input, framebuffer/window policy, clocks/calendar/pause,
   broker ownership, visage/font state и sound-device/UI-sound state. `game_host` теперь сам создаёт
   visage из активного runtime-state, доводит font atlas до GPU-view, ставит стандартные Lua bindings,
   загружает UI entry, логирует devices и сливает sound-state. Проект оставляет hook регистрации
   gameplay/UI API (`perf_stats` в первом живом потребителе).
2. ✅ **Config-driven scene/resource manifest (2026-07-17).** `states/*` задаёт UI entry/default font и
   ссылку на `scenes/*`; стандартный `simul::scene_manifest_resource` хранит типонезависимые
   `{id,target,group,alias,startup}` transitions. `game_host` разворачивает их в scope/load requests,
   target-aware startup readiness/progress и корректно обрезает external targets без render.
   `tile_frontier::begin_project_loading` больше не содержит списков texture/sound ids: он читает
   готовые manifest bindings, а размеры чанков/мира, actor count и ссылки на brain/prefab configs — из
   CPU-only `worlds/*` descriptor. `tile_texture_group` содержит только три grass-текстуры; auxiliary
   `grad*`/`quad` не попадают в terrain palette. Проект по-прежнему владеет созданием мира и chunk readiness.
3. ✅ **Generic config-resource adapters по owner-библиотекам (2026-07-17).** `mood::fsm_resource`
   владеет native-TAVL FSM parser, `acumen::goap_resource` — GOAP parse/merge/flatten,
   `prefab::prefab_resource` — raw prefab carrier, `act::script_resource` — typed script document.
   Общий type-erased `act::script_compiler` оставляет `(ret,scope) → parse<Ret,RootScope>` в проекте:
   `tile_frontier::script_environment` реализует `entity_scope`, а owner-библиотеки не знают об ECS.
   `assets_simulation` теперь только регистрирует owner-типы и передаёт этот compiler adapter.
4. ✅ **Удалить config-дублирование (2026-07-18).** FSM/GOAP/prefab/script и все поля `worlds/*`
   обязательны: `load_required_brain_config` валидирует resource set, а `actor_world_slice::init/load`
   больше не собирают hardcoded графы, переходы и prefab-тексты. Resume/config/MT tests используют
   явный `test_brain_fixture`, который грузит те же shipped resources. GOAP metrics/actions получают
   `act::script_function` facades из скомпилированных config-программ; прежняя параллельная таблица
   native predicates и fallback actions удалена. Нативные `eat/seek_food/wander` остаются разрешёнными
   building blocks до согласованного ds-контракта target scope/RNG, как оговорено ниже.
5. ✅ **Свернуть проектную сцену, не таща её в engine (2026-07-18).** `tile_frontier_game` теперь
   владеет chunk request/receive/apply, map/camera, camera UBO, tile/actor snapshots, actor update,
   presentation-sound policy, project metrics и UI perf facade. Он потребляет уже готовые generic
   host-выходы (`scene_binding`, `phase_gate`, framebuffer/subsystem presence), но целиком остаётся в
   `subprojects/tile_frontier`. `simulation.cpp` сокращён до state/worker wiring и тонких host callbacks;
   компоненты/effects/prefab specs, actor phase order, tile map и draw/sound policies остались игровым кодом.

✅ **Все шесть действий config-only (2026-07-18).** `act::script_function` сеет `vm->prng_state =
mix(rng_seed, rng_entity, rng_tick)` перед `process()` — ds-builtin `chance` (per-callsite соль +
container prng_state) даёт детерминированный RNG конфиг-эффектам БЕЗ `act::rng_source` в сигнатурах.
`seek_food`/`wander` = `effect = { set_course = chance }` над одним building block
`set_course(entity_scope, double)`; курс живёт до следующего think (прежние 12/24-тиковые slot-окна
ушли — визуально блуждание чаще меняет направление, осознанная смена поведения). `eat` = `effect =
{ eat = prey }`: скоуп-функция `prey(entity_scope)→entity_scope` берёт цель из восприятия, elect
арбитрит по typed prey-аргументу; `actor_eating` перешёл на обратный отсчёт `ticks_left`, поэтому
телу eat не нужен tick. `act::rng_source` остаётся provenance-carrier для нативного пути packer.

Ownership catalogue executors и compile-time порядок actor phases пока НЕ выносить: это игровая специфика,
а общий typed owner имеет смысл выделять только после второго живого gameplay-потребителя.

### Дела ближайших сессий (зафиксировано 2026-07-18)

Следующая очередь начинается уже ПОСЛЕ завершения app-shell/config/project-scene разгрузки. Не
возвращаться к переносу `tile_frontier_game` в engine и не строить scheduler graph без нового
потребителя.

1. ✅ **Закрыть узкий devils_script ↔ act registration/effect slice (п.14)** — **ядро готово 2026-07-18.**
   - ✅ `act::building_blocks` (libs/act/building_blocks.h) — одна декларативная точка: `effect<Traits>()`
     (fn_deferred_ptr → ds), `pure<&fn>(name)` (ds-only), исключения `effect_native/native` (act в обход
     ds), `reg_interaction(name, desc)` (свойство семантического имени). `register_ds()` один раз у
     владельца ds::system, `install(registry&)` — replay на каждом пересоздании act::registry; коллизия
     имени с config-скриптом падает громко в reg(). tile_frontier: `actor_building_blocks()` в
     actor_simulation.cpp; script_environment зовёт register_ds, setup_brain_registry — install.
     Тест `act_building_blocks_test` (ds-словарь, replay, loud collision).
   - ✅ DS RNG: `script_function::invoke/describe` сеет `vm->prng_state` из (seed, entity, tick);
     `seek_food`/`wander` = config `set_course = chance`. Тест в `act_call_context_test`.
   - ✅ target scope: `prey` скоуп-функция + config `eat = prey`; `actor_eating.until_tick` →
     countdown `ticks_left` (телу не нужен tick). `config_effect_smoke` теперь требует 6 scripted
     actions, 300 тиков 1-vs-4 identity и живое поедание (eating peak > 0).
   - Acceptance пройден: config-only все 6 действий, fail-loudly, resume + 1/2/4/8 hash/bytes identity
     (новый Release baseline 4096×120: 1.553/1.552/1.176/0.955 ms/tick, hash `0x639aea4f18f91daf` —
     семантика изменилась: countdown eat + ds-RNG курсы; cognition подорожал ~15–20% из-за ds VM на
     каждый scripted action — кандидат следующего профиля).
   - ✅ Мёртвый `effect_sink` seam удалён. Остаток п.14: общий
     string/object/vector marshalling отложен до фактических сигнатур; динамические GOAP cost formulas
     не планируются — численные характеристики проецируются ds-предикатами в дискретные состояния.

2. ✅ **Инструмент времени + generic-флаги с expiration (пп.7–8)** — **ядро готово 2026-07-18;
   scope переопределён автором: НЕ строить систему эффектов, а дать пользоваться временем.**
   - ✅ **Модель скорости — ВАРИАНТ B (решение автора): переменный dt.** Тик остаётся 1/кадр,
     `game_delta_ticks` масштабируется game-часами; тиковая логика поштучно переводится на
     game-время. Живая настройка: Lua `app.set_game_speed(1.2)` / `app.game_speed()` — double
     живёт ТОЛЬКО на границе UI, host конвертирует его в рационал с милли-точностью
     (1.2 → 6/5, 0.8 → 4/5) и дальше вся тайм-математика целочисленная (remainder-carry в
     timelines ⇒ игровые часы — точная пропорция engine-часов, без дрейфа округления по кадрам);
     точная форма `game_host::set_game_speed(num, den)` доступна из C++. Множитель — ПОВЕРХ
     номинального `settings.time`; runtime settings reload сбрасывает к номиналу; ноль/NaN
     запрещены (остановка = pause). Смена скорости = deterministic input (записанный вход —
     рационал, не float).
   - ✅ **Контракт слайса:** `actor_world_slice::update(uint64_t game_delta_ticks, …)` — слайс сам
     аккумулирует `game_now()` (µs game-домена, сериализуется в sim_globals ⇒ deadlines переживают
     resume); dt секунд для интеграции/drives выводится из той же дельты.
   - ✅ **`aesthetics::flag_set`** — generic per-entity флаги: агрегат с публичным сортированным
     `entries` ({хеш имени, остаток game_duration}), set/has/remove/advance. **Countdown-модель, не
     absolute deadline** (осознанно): тела ds-блоков исполняются на commit без контекста времени, а
     читателю-предикату время тоже не нужно; пауза (dt=0) и scale действуют через дельту
     автоматически. Тест `aesthetics_flag_set_test`.
   - ✅ **ds building blocks:** `set_flag = { name, seconds }` (`seconds <= 0` = бессрочный) /
     `clear_flag = name` — collect в serial_structural lane (create<flag_set> on demand) /
     `has_flag(name)` предикат; через `act::building_blocks`. Sweep `expire_flags(game_dt)` — фаза
     слайса после resolve_eating. Живой потребитель: **sated** — resolve_eating (post-commit факт
     доедания, НЕ eat-скрипт — ветки записываются независимо) ставит флаг на 10 game-секунд,
     drives замораживает рост голода, пока флаг жив.
   - ✅ **Тиковая логика → game-время:** `actor_eating.remaining` (µs game) и cognition-каденция
     `commit_game_ticks_` = 150ms game (= прежние 3 тика при 20fps/scale 1) — мышление и поедание
     замедляются/ускоряются вместе со временем. `think_budget_` остаётся count/tick (CPU-бюджет, не
     геймплей). Побочно: benchmark 4096×120 подешевел до 0.770/0.783/0.726/0.631 ms/tick (при dt
     бенчмарка 1/60с каденция реже), hash `0x6314d39ac4e9f875` identical 1/2/4/8.
   - ✅ **Acceptance:** config_effect_smoke — смена скорости мид-ран (×3 → пауза dt=0 → ×0.5) с
     1-vs-4 identity; resume bit-identity (flag_set сериализуется — vector-поле через reflect);
     пауза не двигает сроки (dt=0 ⇒ advance no-op).
   - ⏳ Отложено (по слову автора): очередь отложенных effect-вызовов, typed calendar/turn
     deadlines для событий календаря/ходов, абсолютная дата окончания в UI (= now + remaining).
   - ⏳ **Локальный множитель времени (прикинуть отдельно, автор 2026-07-18):** per-entity /
     per-system масштаб поверх глобального game clock — быстрая анимация конкретной сущности,
     haste/slow-эффекты (юнит «живёт быстрее»: drives/cooldowns/анимация ×k). Наброски к дизайну:
     локальный масштаб = обычный КОМПОНЕНТ (haste как флаг с payload-множителем — стыкуется с
     остатком B-е «модификаторы с payload»); системы, потребляющие dt, умножают per-entity дельту
     (map-фазы уже per-entity — точка врезки дешёвая); водораздел gameplay-haste (детерминированный,
     реплицируемый, рациональный множитель) vs presentation-only ускорение анимации (float, не
     реплицируется); countdown-сроки (flag_set/eat) при haste тают быстрее умножением dt ДО advance.

3. **Формализовать events/triggers/relations contract (п.13)** — **водоразделы зафиксированы,
   первый срез кода готов (2026-07-18).**
   - ✅ **Водораздел событий (решение автора): отдельной сущности «событие» НЕТ.** Четыре формы:
     (1) эффект → catalogue deferred call; (2) коммуникация систем ВНУТРИ тика →
     `aesthetics::message_buffer`/`message_registry`; (3) gameplay input (игрок/сеть) →
     `act::intent` — единственный replay-вход; (4) presentation side-output → паттерн `sound_emit`
     (эфемерно, сим не читает, переэмитится при реплее). Cross-tick события = будущая очередь
     отложенных вызовов (отложена в п.2).
   - ✅ **Первый живой input-кейс:** WASD-камера — named actions `camera_*` в
     `bind_default_actions` (движковый словарь), политика движения/клампа — проектная
     (`tile_frontier_game::move_camera`: скорость ∝ half_width, реальное время кадра — работает на
     gameplay-паузе; точка камеры клампится в бокс тайлового мира). Мир увеличен до 8×8 чанков
     (128×128 тайлов). Доводка по живому тесту (2026-07-19): верх экрана = -y мира (W/S
     инвертированы); акторы клампятся в тот же мировой бокс в `integration_system` (границы =
     spawn_min/max, уже сериализованы — resume не тронут); камера интерполируется на
     рендер-потоке — main шлёт снапшот `command_draw_camera`, рендер лерпит prev→cur по
     `nominal_clock` и сам собирает `camera_buffer`-UBO каждый рендер-кадр (лаг в один снапшот —
     как у акторов, визуально синхронны).
   - ✅ **Read-only UI-шов к act (loc/string facade):** slice `ui_predicate/ui_number/ui_string/
     ui_describe` (dry-run ctx, отдельный `ui_scratch_`, main-thread) → Lua
     `app.act_predicate/act_number/act_string/act_describe(name, entityid [, cb])`. Effect-категория
     намеренно недоступна — Lua не mutating backend. `act_string` возвращает хеш loc-ключа
     (64-битный Lua integer). `act_describe` с коллбеком стримит УЗЛЫ исполнения ds по одному
     (замысел автора: Lua строит из узлов маленький граф исполнения для тултипов); без коллбека —
     простая строка. Покрыто headless-проверкой в config_effect_smoke.
   - ✅ **Правило relations (контейнеры отложены, автор):** связь = entityid-поле компонента
     (ПОЛНЫЙ versioned id), читатель обязан `exists()`-проверять, автоочисток нет (версия делает
     dangling id безопасным) — так живут `actor_eating.target`/`actor_grabbed.by`, serial уже
     ресолвит ссылки после load. Generic-механизмы (энтити↔энтити, энтити→фракция→энтити ленивой
     матрицей) — позже: простые отношения TES-типа = флаги у энтити (flag_set уже есть), CK3-тип =
     своя матрица; конкретной игре всё равно нужны свои механизмы.
   - ⏳ **UI intent-очередь (дизайн зафиксирован, строить с player entity):** кнопка действия
     (атака) кладёт `act::intent` в очередь интенций игрока; анти-спам ОБЯЗАТЕЛЕН — dedup по типу
     действия (повторный emit того же типа, пока предыдущий не потреблён симом, игнорируется).
     Player entity появится позже — тогда же строить.

4. ↪ **Перенесено в общий пул техдолга (решение автора 2026-07-19):** catalogue perf service как
   поперечный инструмент — см. «Профилирование и бюджеты» в тех-долге ниже.

5. ↪ **Перенесено в общий пул техдолга (решение автора 2026-07-19):** ownership catalogue
   executors/system pipeline после второго gameplay-потребителя — см. «Перенос из tile_frontier
   в либы» в тех-долге ниже.

### Дела ближайших сессий (зафиксировано 2026-07-19)

1. ✅ **Input mapping (сделано 2026-07-19):** key-mapping как данные настроек —
   `input::bindings_config` (libs/input/bindings.h): tavl-секция
   `input = { actions = { camera_up = [key_w], attack = [mouse_left, mouse_right] } }`
   (btree_map ⇒ сортированный, детерминированный файл). Движок накатывает её ПОВЕРХ дефолтного
   словаря `bind_default_actions` при создании окна и при reload_settings; семантика: перечисленное
   действие перепривязывается ЦЕЛИКОМ (хвостовые слоты чистятся), пустой список = отвязать,
   неизвестная кнопка = warn+skip, неперечисленные действия не трогаются. `save_settings`
   выгружает эффективную живую карту обратно в settings.tavl (guard `has_bindings` — headless/до
   окна не затирает секцию из файла). Имена кнопок: canonical-таблица клавиатуры +
   mouse_left/right/middle/mouse_4..8 + `scancode_N` lossless-fallback (экзотика переживает
   save/load). `events` получил clear_key/clear_event/for_each_event/has_bindings (detach-чистка
   как в set_key). Lua: `app.save_settings()`/`app.reload_settings()` через host (reload прогоняет
   полную runtime-реакцию: логгирование/fps/скорость/биндинги). Тест `input_bindings_test`
   (мышь/scancode-fallback/tavl-roundtrip — без GLFW; клавиатурный путь живёт на живом окне).
   Остаток: live-rebind UI (перехват нажатой кнопки + app-биндинг) — вместе с экраном настроек.
2. ✅ **Позиционный звук (сделано 2026-07-19).** Найденный дефект: mono-голоса ВСЕГДА
   спатиализовались, но слушателя никто не двигал (origin) — громкость зависела от близости
   источника к углу карты, а не к камере; stereo (музыка/UI) были вне спатиализации флагом.
   Контракт теперь явный:
   - `sound::task.min_distance/max_distance`: `max_distance > 0` ⇒ позиционный голос
     (linear-затухание в [min, max] мировых единиц), `0` (дефолт) ⇒ спатиализация ВЫКЛЮЧЕНА
     (UI/музыка звучат как есть). Голоса переиспользуются ⇒ toggle выставляется на КАЖДОЙ
     выдаче голоса (иначе прошлый жилец протекает).
   - `command_sound_play` += volume/pitch/pos/min_distance/max_distance;
     `command_sound_listener` (pos/dir/up/vel, дефолт = top-down: панорама следует +x экрана)
     — mailbox `sound_listener` в standard_broker, standard_sound_system кормит
     ma_engine-слушателя (set_listener_ori ждёт ТОЧКУ взгляда — host передаёт pos+dir).
   - tile_frontier: `publish_sound_listener` = камера, каждый кадр и на паузе; sim-звуки:
     pos = event.pos, min = 0.25×audible, max = audible (1.5×half_width) — прежний
     радиус-фильтр остался как бюджет-отсечка, совпадающая с гранью слышимости.
   Остаток: velocity/doppler не кормим; per-type громкости уже живые (`set_source_volume` обновляет
   активные voice и переживает смену playback device);
   позиционные апдейты движущихся источников (command_sound_update) не используются
   fire-and-forget звуками.
3. ✅ **per-entity GOAP/FSM refs (сделано 2026-07-19; развилки решены автором):** ссылка =
   `utils::id` (хеш ИМЕНИ ресурса — переживает resume/пересборку реестров), GOAP и FSM — ДВЕ
   независимые ссылки (компоненты `goap_ref`/`fsm_ref`, SERIALIZABLE), живой кейс —
   хищник/добыча, runtime-переключение мозга отложено (acumen-долг).
   - Мозги = наборы ресурсов по префиксам scene config (`fsm_prefix`/`goap_prefix` вместо
     `actor_fsm`/`actor_goap`): каждый `goap/*` flatten-ится в отдельную `acumen::system`,
     каждый `fsm/*` — в отдельную `mood::system` (имя = id без префикса). `prefab::reference<C>`
     (шов был заложен заранее): строка `goap = prey` в префабе → компонент с хешем + громкая
     валидация имени; derive-хук стал ОБЩИМ data-driven (по наличию food_item/actor_tuning, не
     по имени префаба — наследники actor получают derive без per-name регистрации).
   - **Имена метрик/действий — глобальное act-пространство слайса:** несколько мозгов наследуют
     одну базу ⇒ одноимённые регистрации дедупятся по `origin` (новое поле goap_config: resource
     id, где элемент ОПРЕДЕЛЁН; overlay переносит origin) — одинаковый origin = копии одного
     скрипта (регистрируем раз), разный = громкая ошибка.
   - **Пойманный баг мемоизации:** plan_key НЕ содержал тождества системы, а cached_plan хранит
     ИНДЕКСЫ действий системы ⇒ общий per-thread solution_cache на два мозга подменял бы планы.
     Фикс: `plan_key.system_salt` + `system::set_cache_salt` (registry::add ставит = system_id).
   - decide_actor/apply резолвят мозг per-entity (`registry.get(hash)`, промах = громкая ошибка);
     `goap/prey.tavl` = base actor_base + `disable_actions = [chase, eat]`; `prefab/prey.tavl` =
     base actor + `goap = prey` (FSM общий — доказательство независимости ссылок). Живой мир:
     `actor_prefab_cycle = [prey, prey, prey, actor]` (25% хищников, i % size — детерминированно).
   - Acceptance: config_effect_smoke — смешанная популяция 1-vs-4 bit-identity, refs резолвятся,
     prey не ест; resume/MT benchmark identity 1/2/4/8 держится. 189/189 тестов.
   - ⏳ Остаток: runtime profile switch (deterministic tick-boundary + инвалидация кешей),
     required-components валидация мозга против префаба, prey-баланс (сейчас prey не умеет
     сбрасывать голод поеданием — только еда через общий eat отключена тоже: проверить желаемую
     экологию вживую).

**Не ближайшие:** сложный spatial query по множеству динамических spawner entities, ds `filter/pick`,
multi-participant catalogue `reserve/write-set`, runtime dependency graph, input contexts/live-rebind UI,
полная flow-анимация без первого потребителя. Простой первый spawn-point slice для еды, напротив,
перенесён в актуальную очередь п.15.

### Актуальная очередь после ревизии 2026-07-19

Пакет пунктов 1–5 закрыт 2026-07-19 последовательными проверяемыми slices.

1. ✅ **GPU asset lifetime + command coverage.** Texture slot при hot-unload после безопасной
   frame-generation/fence границы переключается на общую null texture, затем старые VMA image/view
   уничтожаются, а slot становится доступен для reuse. Mesh/buffer удаляется из всех `draw_group`,
   storage уничтожается и slot возвращается в `empty`; lifecycle покрыт painter-тестом. Аудит команд
   подтвердил полные switch/step реализации для живого draw/dispatch/transfer набора.
2. ✅ **Query rebuild после snapshot.** Общий `snapshot_loaded_event` живёт в aesthetics, а `query_t` и
   `lazy_query_t` подписаны на него и вызывают общий `rebuild()` по загруженному миру. Тест держит query,
   созданные до load, и проверяет новое содержимое после snapshot.
3. ✅ **Узкий spawn slice.** Простые spawn-point entities для автономного появления еды + script-facing
   абстракция выбора точки; координатный `spawn_at` сохранить для явно выбранной позиции. Сложные
   filter/pick/spatial policies отложены. `spawn(prefab, group)` выбирает semantic point, а food maintenance
   использует группу `food`.
4. ✅ **Player entity + UI intent queue; первый consumer — spawn food мышью.** Mouse input преобразует
   screen point в world point и записывает typed `act::intent`; gameplay потребляет его на границе тика
   и создаёт food в выбранной координате. Один pending intent одного action type — обязательный dedup;
   Lua/UI не получает прямого mutating доступа к world. Первый binding — `spawn_food = mouse_right`.
5. ✅ **UI budgets как engine settings.** Instruction/wall-time/GC-step и convert time/output limits
   живут в app/user settings. `lua.update` и `nuklear.convert` измеряются через catalogue statistics;
   ошибка/превышение пропускает кадр, серия неудач отключает UI, reload/recreate сбрасывает состояние.
6. **Replay — после трёх prerequisites, пока не активный slice:**
   (a) runtime-список модулей, где demiurg загружает core и сторонние modules в явном priority order;
   (b) камера как отдельный intent provider — её движение входит в replay/presentation track, хотя
   camera intents не обязаны входить в network input; (c) запись точных `game_delta_ticks` каждого тика.
   После этого базовый артефакт — versioned input log:
   все typed `act::intent` с tick/order/payload плюс влияющие на симуляцию host inputs и точные
   `game_delta_ticks` для текущей variable-dt модели. Для дампа последних N секунд нужен кольцевой
   input buffer и checkpoint/snapshot не позже начала окна; одних последних intents недостаточно,
   чтобы восстановить исходное состояние. Offline tool загружает checkpoint, переигрывает log и пишет
   видео в выбранном capture FPS. Presentation генерируется заново: pixel-perfect совпадение с исходным
   экраном не является контрактом. Camera intent track сохраняется для воспроизводимого начального
   ракурса/движения, но остаётся отдельным от network-replicated gameplay intents.
   Первый формат поддерживает только тот же build/config/resource fingerprint и громко отвергает mismatch,
   не требуя schema migrations. Catalogue остаётся execution/trace слоем, не replay serializer.

**Попутная чистка закрыта:** мёртвый `act::effect_sink`, `exec_context.sink` и устаревшая dry-run
семантика удалены. Read-only потребители вызывают только pure act-категории, mutating effects идут
через catalogue, spawn — через отдельный project-owned `spawn_sink`.

Не поднимать сейчас: netcode, selective deltas/partial dumps, реальные schema transforms старых save,
immutable engine packaging, cancellation/priority asset requests, input contexts/live rebind и animation
framework без ассетов/первого потребителя.

**Техдолг assets worker:** он остаётся coordinator-ом отдельного потока и может ждать тяжёлую загрузку,
но должен получить доступ к общему worker pool и уметь распараллелить независимые CPU prepare/dependency
jobs. Нужны корректные pending/failed transitions, единая публикация результата после join, diagnostics
и shutdown; pool-задачи не должны делать nested dispatch/wait. Cancellation/priorities пока не входят.

### Отложенная последовательность MT-обобщения gameplay pipeline

Dense deferred journal уже закрывает расход typed call storage: type-erased call хранится в 128-byte
inline payload, а storage размеряется по фактическому budget после select. Минимальный phase runner
реализован без scheduler graph: `aesthetics::run(pool,time,systems...)` объединяет независимые range,
worklist и `single`-работы под одним barrier. `integration + drives` переведены на него первыми.

Перед разговором о scheduler закрыты подготовительные пункты:

1. ✅ Identity domain tag отделён от reusable strategy policy:
   `domain<IdentityTag, Strategy>`; одинаковая policy больше не означает общий static executor binding.
2. ✅ Добавлены нейтральные `catalogue::mt::preset::{parallel_collect,serial_elect,structural_elect}`.
3. ✅ Добавлен `tile_frontier_mt_benchmark`: 1/2/4/8 workers обязаны дать одинаковые world hash И bytes.
   Сборка и запуск отдельно от приложения:
   ```sh
   cmake --build build-release --target tile_frontier_mt_benchmark -j
   ./build-release/subprojects/tile_frontier/bin/tile_frontier_mt_benchmark
   ```
   Текущий Release baseline после phase-runner migration, 4096 actors, 30 warmup + 120 measured ticks:
   1.475/1.504/1.310/1.181 ms/tick для 1/2/4/8 workers, speedup 1.00/0.98/1.13/1.25.
   Debug baseline: 10.051/10.733/9.775/8.966 ms/tick, speedup 1.00/0.94/1.03/1.12.
   Hash `fe76b88777f8f705` и полные snapshot bytes совпали во всех четырёх прогонах обеих сборок.
4. ✅ Добавлен общий `aesthetics::run` без dependency graph и вложенных barrier-ов; обычный ST
   `update(time)` и `single(fn)` становятся одной неделимой задачей внутри pool phase.
5. ✅ `integration + drives` запускаются совместно: вместо двух pool barrier теперь один.
   После миграции benchmark сохранил bit-identical snapshot для 1/2/4/8 workers.
6. ✅ **`sense_tree` измерен и ускорен (2026-07-17).** Benchmark теперь сбрасывает catalogue statistics
   после warmup и печатает avg/min/max + долю wall-clock для каждой фазы. Исходный Release 4096×120
   показал `sense.tree` 470–481 us независимо от workers: 32.5% тика на 1 worker и 40.8% на 8.
   Разложение подтвердило: gather всего 23–39 us, bottleneck — рекурсивный `nth_element` build.
   `utils::kd_tree::build_parallel` детерминированно строит несколько верхних медиан, затем отдаёт
   непересекающиеся поддеревья в pool. Snapshot следующего тика собирается после structural tail и
   строится одновременно с независимым `actor_batch::build`; init/load делает явный fallback. Семантика
   сохранена: позиции конца N = позиции начала N+1. Финальный Release baseline 1/2/4/8:
   **1.304/1.248/0.937/0.865 ms/tick**, speedup **1.00/1.05/1.39/1.51** против исходных
   1.479/1.532/1.316/1.165. Hash `fe76b88777f8f705` и bytes неизменны; resume/config-effect smoke зелёные.
   Длинный контроль 4096×600: 1.438/1.360/1.085/0.920 ms/tick, speedup 1.00/1.06/1.33/1.56,
   также byte-identical. Короткие прогоны шумят, поэтому для сравнений держать одинаковую длину.
   Следующий измеряемый кандидат — cognition (≈35% wall tick на 8 workers), затем apply (~21%);
   дерево дальше не усложнять до нового профиля/масштаба данных.

Multi-participant `reserve`/`write-set` снят с ближайшей последовательности и перенесён в дальний backlog
`libs/catalogue`: текущий single-key collect/elect сначала должен получить реальные ограничения из gameplay.

✅ После пяти пунктов переноса app-shell критерий достигнут: `simulation.cpp` — тонкий набор project
callbacks/регистраций, а tile map, actor gameplay, prefab policy и tile/actor render draws остаются
проектными за фасадом `tile_frontier_game`.

13. **B-ж — формализация контрактов** — **[необходимо]**. События/триггеры (на `intent`/catalogue
    INPUT), связи/отношения энтити (`exec_context.scope[8]`; референсная целостность в serial уже есть),
    экспонирование в UI-lua + loc-ключи (seam visage уже есть; loc = `string`-функции act).

14. **Интеграция devils_script ↔ act** — **[необходимо; ядро готово, расширять от потребителя]**.
    `script_function`, per-worker VM, GOAP/FSM-конфиги, mutable `call_context`, единый
    `act::building_blocks`, DS RNG/target scopes, config effects и базовый `describe` работают;
    production fallback, отдельная native predicate/action таблица и неиспользуемый
    `effect_sink.h`/`exec_context.sink` удалены. Остаток — string/object/vector/custom
    codecs добавлять под первый реальный relation consumer. Меж-энтити эффекты уже идут
    через catalogue; второй deferred-механизм рядом не нужен.
    **Уточнение MT-регистрации эффектов (2026-07-17):** `devils_script::on_effect` НЕ является MT-hook —
    это gameplay-реакция на вызов функции (ачивки, счётчики и т.п.) и сама при необходимости проходит
    через catalogue как обычная gameplay-функция. MT-перенаправление даёт САМА обёртка catalogue:
    эффект с обычной C++-сигнатурой регистрируется в ds не прямым указателем, а generated deferred-pointer
    вида `catalogue::mt::domain<DOMAIN_TAG, STRATEGY>::fn_traits<&fn,...>::fn_deferred_ptr`. В record-фазе он
    копирует функцию+аргументы в буфер стратегии и НЕ вызывает тело; настоящее тело вызывается executor-ом
    после barrier. Deferred execution-domain получает не владеющий executor через `set_executor`; executor должен
    быть установлен до MT record-фазы, оставаться неизменным до barrier и быть явно снят/заменён только
    вне параллельного прохода. Trace-domain ОРТОГОНАЛЕН execution-domain: один отвечает за наблюдение,
    второй — за способ исполнения. Если executor отсутствует или вызов пришёл не в record-фазе, deferred
    wrapper обязан громко ошибиться, а не незаметно выполнить мутацию немедленно.
    `on_effect` реагирует на ФАКТ ЗАПИСИ gameplay-вызова, а не на гарантированно успешный commit: elect
    может проиграть или commit может быть отфильтрован. Реакция на успешный эффект делается отдельным
    post-commit gameplay-шагом по фактическому состоянию/компонентам, оставленным эффектом. Это осознанное
    ограничение архитектуры, а не обязанность `on_effect` синхронно следовать за телом функции.

15. **Спавн-архитектура** — **[необходимо; следующий практический slice — простой]**.
    - ✅ **Per-entity GOAP/FSM уже закрыты отдельно:** независимые `goap_ref`/`fsm_ref`, prefab reference,
      несколько registry-систем и cache salt живы; общий `brain_ref` не вводить.
    - ✅ Низовой координатный примитив остаётся полезным: `spawn_at(prefab,x,y)` над project-owned
      `spawn_scope`/`spawn_sink`. Это ОТДЕЛЬНАЯ capability и не потребитель старого `act::effect_sink`.
    - ✅ **Первый slice:** `tile_frontier` создаёт простые spawn-point entities группы `food`; скрипт
      вызывает `spawn(prefab, group)`, а `spawn_at` остаётся низовым escape hatch. Первый вариант
      намеренно не вводит spatial query, cooldown/capacity/filter DSL или универсальный manager.
    - **Позже, после живого ограничения:** много статичных/динамических spawner entities,
      пространственный запрос + predicate filter, ds-композиция `spawn/filter/pick`, replicated-region
      policy для off-camera и catalogue-порядок массового spawn. Не цементировать этот API заранее.

16. **Меж-энтити взаимодействия — deferred-call эффекта через `catalogue`** — **[необходимо]** ·
    ✅ **ЖИВОЙ PIPELINE ПЕРЕВЕДЁН (2026-07-17):** `actor_world_slice::cognition` в MT вызывает
    выбранный GOAP-ом `act::effect` под `catalogue::mt::record_scope`. В `act::registry` лежит не
    нативное тело, а `fn_deferred_ptr` с той же С++-сигнатурой; поэтому cognition только
    копирует typed arguments в предаллоцированный dense journal с глобальным sequence id.
    `apply(pool)` явно делит commit:
    - local movement effects → `collect<entity_arg<0>, source_then_sequence, parallel_groups>`;
      разные self-группы идут в worker pool, внутри группы порядок стабилен;
    - `eat` → `elect<entity_arg<1>, source_then_sequence, serial_structural,
      conflict::target_not_source>`; один минимальный source побеждает на prey, а prey, которая сама
      вызвала eat, нельзя схватить (прежнее «intent бьёт grab»); ECS `create` идёт только в ST lane;
    - после обоих barrier-ов старый `call_log` обходится по source-index только как journal выбранного
      action для FSM/звука; тела эффектов он больше не хранит и не вызывает.
    Старые `interaction_arena` + `simul::commit_calls` остались как совместимые generic-примитивы, но
    live tile pipeline их больше не использует. `act::interaction` пока остался project seam-ом, который
    говорит `decide_actor`, какой scope slot заполнить target-ом; arbitration/commit живут в strategy type.

    **Acceptance:** `catalogue_deferred_test` покрывает два независимых ds-effect, collect/elect/order,
    `target_not_source`, parallel groups и fail-loudly contracts. `tile_frontier_resume_smoke` прогоняет
    live gameplay 1-worker vs 4-worker 45 тиков bit-identical, затем save/load + 120 resumed тиков
    bit-identical. Детерминизм не зависит от числа worker-ов в этом срезе.

    **Зафиксированная семантика составного effect-скрипта (2026-07-17):** все effect-ветки с истинным
    `condition` записывают свои вызовы; каждый вызов независимо маршрутизируется стратегией СВОЕЙ функции.
    Например `add_strength` может попасть в `collect` и выполниться независимо от того, выиграл ли
    `eat_prey` свой `elect`. Это штатное поведение, не транзакция скрипта. **Будущее:** явно описываемый
    совместный/all-or-nothing вызов нескольких эффектов (effect group/compound call); НЕ выводить такую
    связь неявно из соседства блоков в ds. Зависимые gameplay-реакции часто лучше выражать отдельным
    последующим скриптом/системой (например passive skills после успешного `eat_prey`).
    **Дальний data-driven follow-up из раннего claim-дизайна:** отдельный per-target `claim_buffer`
    больше не нужен — его роль закрывает `catalogue::elect`. Если второй живой structural interaction
    повторит форму eat/grab, рассмотреть декларативное описание результата победы (prefab-подобные
    grant/remove/assign компонентов): скрипт записывает семантический вызов, strategy выбирает
    победителя, ST structural commit применяет данные без project-specific CRUD-тела. Проигравшему
    автоматическое событие не посылается; успех наблюдается по post-commit состоянию. До второго кейса
    этот API не строить.
    ✅ Загружаемый `act::script_function<void>` для GOAP action готов: optional `effect = <ds>` co-parse'ится
    из tavl, semantic action name получает script backend в act, а ds building blocks — те же
    `fn_deferred_ptr`. Штатные `flee/chase/think` переведены; `tile_frontier_config_effect_smoke` грузит
    shipped `goap/actor` и проверяет 1-vs-4 bit identity. ✅ (2026-07-18) DS RNG для
    `seek_food/wander` (`set_course = chance`) и target scope для `eat` (`eat = prey`) сделаны —
    все 6 действий config-only, регистрация через `act::building_blocks`; старый `effect_sink` удалён.
    Ближайшие шаги: (1) member/custom bounded codecs по реальным
    сигнатурам; (2) ownership helper только после второго потребителя; (3) `join` только при
    реальном gameplay-кейсе.

---

## Срез 2026-07-13 и следующий порядок

1. ✅ **act↔ds call contract foundation:** fixed 8 scalar args + lists в reusable `execution_scratch`;
   `acumen::execution_scratch` показывает композицию subsystem scratch. Далее — registration/effects.
2. ✅ **generic stats:** template/concept/init + scope getters без префиксов; C++ default member
   initializers и reflected callback являются явными альтернативами, два агрегата проверены.
3. ✅ **time/calendar/pause:** engine/presentation/game/turn + project calendar source game_time|turn,
   rational duration mapping, typed deadlines, calendar projection и gameplay/presentation pause готовы.
4. ✅ **первый multi-system AI foundation:** независимые `acumen::registry`/`mood::registry`; GOAP-конфиг
   имеет single `base`, deterministic overlay/disable и flatten перед компиляцией. Compatibility и
   runtime GOAP switch остаются long-running.

Только после этих четырёх срезов — отдельное проектирование **per-entity brain + spawner entities +
ds filter/pick/spawn** (п.15). Perf service-channel/UI-графики (п.4) и system pipeline (п.11)
были следующими на момент среза: actor UI-графики и согласованный первый system-runner теперь готовы,
общий perf service остаётся; app-shell (п.12) закрыт.

---

## Тех-долг по подсистемам `libs/`

> **Статус: блок A (🟢 быстрые) ВЫПОЛНЕН (2026-07-05).** Сделано: input mouse-buttons first-class
> (`events::update_mouse_button`/`set_mouse_button`); painter `clear_color`/`clear_depth` из констант +
> сообщение `command_update_constant`; sound PCM убран из форматов + тип звука в play-команде; mood
> тесты (blocked/internal/settle/limit) + лимит 8 guards/actions + `settle()` idle-helper + диагностика
> парсера с позицией; act registry различает повтор имени vs hash-коллизию; aesthetics `view<T>` понятная
> ошибка; catalogue `demo.h` include guard; demiurg диагностика циклов зависимостей; simul
> jthread-friendly `run(stop_token,…)`; visage `endf`→`fin` + удалены dead `draw_resource`/`draw_stage`;
> bindings `rng_state + int` (advance N); `sound.enabled` + динамический `worker_threads_reserved`.
> **Отложено:** demiurg «строгий контракт zip до parse» (#13) — существующий путь уже warn+skip на
> незарегистрированном типе; «строже» без уточнения автора смысла не имеет.
> Собрано rc=0, тесты зелёные (кроме bundled cpuinfo init-test), tile_frontier доходит до game без lua-ошибок.

Собрано из README каждого подпроекта. `libs/bindings` намеренно без README (сейчас сильно завязан на
visage). Обозначения строк: **усилие** 🟢/🟡/🔴 (как выше) + **необходимость** `(н)` = необходимо /
`(qol)` = quality of life.

### Сквозные темы (видны сразу в нескольких либах)
- **Документация header-файлов (отдельный проход, зафиксировано 2026-07-13)** — текущие короткие
  purpose-комментарии считать временной разметкой, а не завершённой документацией. Описание header
  должно находиться **после полного блока `#include`, но до первого `namespace`**. Оно должно быть
  содержательнее одной строки: кратко объяснять, что представляет собой модуль/контракт, его основные
  обязанности и границы, типичный способ использования, а также важные особенности реализации или
  инварианты, если они неочевидны. Не пересказывать декларации механически и не смешивать это общее
  описание с локальными комментариями отдельных типов и функций. Сделать единый обзорный проход по
  всем собственным header-файлам позднее; массово расширять комментарии в текущей стилевой чистке не
  требуется.
- **Описание через конфиги/ресурсы вместо C++** — `act` (формат регистрации функций), `acumen`
  (metrics/goals/actions), `mood` (FSM из строк→config), `demiurg` (list pattern `path:name`), `flow`
  (формат анимаций), `painter` (`.tavl` data model), `input` (bindings из config). Это ядро модели
  «проект = конфиги + заполнение реестров».
  `demiurg` уже разворачивает `.tavl` list-файлы в отдельные ресурсы, а `painter` умеет читать
  render-config категории через эти subresources.
- **Стабильность формата и версионирование** — `aesthetics::serial` уже пишет container version и
  compile-time fingerprint из component type hash + рекурсивного positional layout hash; несовпадение
  схемы отвергается громко. Это detection/version guard, а не migration. Преобразование старых save
  появится как явные per-version adapters только с первым требованием backward compatibility;
  `act` сохраняет типизированные gameplay payload. Save/replay/netcode при появлении получают
  ОТДЕЛЬНЫЙ owning/serialization слой; catalogue предоставляет им не более чем наблюдаемые вызовы и
  намеренно не становится бинарным форматом или replay executor. Связано с
  [[determinism-replication-architecture]].
- **Профилирование и бюджеты** — `catalogue` perf-tracing как поперечный инструмент (перенесено
  сюда из очереди сессий 2026-07-19): actor-local graphs живы; следующий результат — общий service
  consumer/budget API для sound/assets/render/visage без project-specific `app.perf_stats` таблицы
  на каждый домен; делать параллельно с новыми системами, когда измерения нужны для выбора
  bottleneck. Также `sound` (`system2::update`), `visage` (Nuklear/Lua), `painter`. Один инструмент
  закрывает несколько либ. `demiurg` уже подключён к `catalogue` доменом `demiurg`.
- **Перенос из tile_frontier в либы (library-first)** — ✅ `simul` broker/topology/lifecycle и
  app-shell окна/настроек закрыты в п.12; минимальный system runner п.11 живёт в `aesthetics`.
  `tile_frontier_game` остаётся project policy. Следующее обобщение ownership — только после второго
  gameplay-потребителя, не автоматический перенос очередного project-файла в engine. Сюда же
  (из очереди сессий 2026-07-19): ownership catalogue executors/system pipeline — delayed-effects/
  expiration или triggers должны сначала показать повторяемую форму, тогда выделить typed
  owner/helper, сохранив project strategy types и compile-time порядок фаз.
- **Очистка legacy** — ✅ painter (`painter_base`/старый `execution_pass` и image containers) и
  utils (`actor_ref`/`context_stack`/неиспользуемые allocator-прототипы) перенесены в `exclude/`
  2026-07-13; в `sound` туда же ушла заброшенная `virtual_source`/`basic_sources` иерархия, но
  смешанный OpenAL `sound::system` остаётся. Далее: старые dispatcher-подходы и две линии `catalogue` API.
- **Пробелы в тестах** — focused-тесты уже есть у `catalogue` introspection, `mood`, `sound::system2`,
  `flow` и `simul`; не покрыты их более старые/широкие контракты. Отдельные пробелы остаются у `input`
  и `visage`; catalogue strategy wrapper/executor/order/overflow + live tile determinism теперь покрыты.
  Config-loaded void-script acceptance и inline journal stress закрыты; дальше нужны bounded-codec
  acceptance по фактическим сигнатурам и расширение `utils`; `effect_sink` уже удалён.
- **Детерминизм на уровне сборки** — `options` (fp-модель/fast-math/FMA как отдельные interface targets).
- **Отключаемость подсистем + модель потоков** — `simul` (optional render/sound/assets), `sound.enabled`
  (п.2), `painter` (typed graphics/transfer/compute queues + mutex при физическом alias). Широкие
  waitIdle не используются в кадре; оставшиеся queue/device waits принадлежат present drain и teardown.

### `libs/act` — реестр gameplay-функций
- 🟡 (н) **native↔ds фасад и полный call contract** — `act::building_blocks` уже даёт единый
  registration path, `script_function<bool/number/void>` и reusable `execution_scratch` с mutable
  args/out/lists работают. Остаток — string/object/vector return/bind policy и custom bounded codecs,
  но расширять их ПО ФАКТИЧЕСКИМ сигнатурам spawn/player/cost, не строить универсальный variant заранее.
  `libs/act` скрывает источник функции, а не дублирует ds runtime.
- 🟡 (н) **Lua backend policy** — Lua остаётся UI/guest; если pure gameplay-вызовы ему понадобятся,
  экспонировать их через act-фасад, но не делать Lua mutating backend симуляции.
- ✅ (н) **старый `effect_sink` seam удалён.** `exec_context.sink`, header и комментарии убраны;
  live native/ds effects идут через catalogue, UI вызывает pure categories, spawn использует отдельный
  project `spawn_sink`. Будущий replay recorder остаётся отдельным owning слоем.
- 🟡 (н) **формат конфигов регистрации функций** + загрузка функций из модулей/ресурсов.
- 🟡 (н) **типизированные границы `intent`/payload** — для act/ds marshalling; долговременная
  сериализация replay/network принадлежит отдельному слою.
- 🟡 (н) **number unit/tag** — нужен ли числам тег единицы (деньги/дистанция/проценты) — открытый вопрос.
- ✅ (qol) **базовый describe для script backend** — `act_describe` стримит ds execution nodes в UI;
  богатые metadata/tooltips и cost explanation добавлять вместе с первым потребителем.
- ✅ (qol) **реестр различает повтор имени и hash-collision**.

### `libs/acumen` — GOAP
- ✅ **expression layer для predicates** — используется devils_script, отдельный мини-язык не нужен;
  GOAP-метрики из tavl уже содержат inline-ds выражения.
- ✅ **metrics/goals/actions через конфиги для текущей модели:** структура, predicates и effects готовы.
  GOAP остаётся дискретным: численные характеристики режутся ds-предикатами на состояния вроде
  `low_hp/damaged/healthy`; динамические numeric cost formulas не планируются. Числовые ds-значения
  между двумя entity относятся к будущим relation/оценочным системам, не к текущему GOAP backlog.
- 🟡 (qol) **встроенный выбор цели** (сейчас на вызывающем) и **бюджет перевычисления metrics**.
- 🟡 (qol) **глобальная/persistent политика кеша** (сейчас ручные per-thread + merge).
- ✅ **registry + config inheritance foundation** — registry владеет стабильными immutable systems;
  tile_frontier GOAP resources поддерживают single-base overlay/disable и flatten. Runtime hierarchy нет.
- 🔴 (н, long-running) **compatibility + runtime profile switch** — required components/capabilities,
  prefab validation, deterministic tick-boundary switch и раздельные cache/action-index domains.
- 🟡 (qol) **динамический размер состояния** (сейчас фикс. `bitset<256>`).

### `libs/aesthetics` — ECS
- 🟡 (техдолг, не блокирует текущий pipeline) **задокументировать и debug-проверить policy structural
  changes под MT.** Текущий рабочий контракт:
  worker map-фазы меняют только заранее существующие disjoint components; `create/remove/remove_entity`,
  создание allocator/type-id и события query запрещены до barrier и выполняются в ST
  `serial_structural` lane. `world` сам это пока не проверяет — безопасность держится на phase policy.
  Отдельный техдолг — описать contract рядом с API и добавить debug phase guard/assert на структурных
  entry points. Полноценный structural command buffer нужен только при втором паттерне.
- 🟢 (н) **авто-пересборка query после загрузки снапшота — ближайший малый slice:** `load_world` уже
  эмитит `snapshot_loaded_event`; перенести event в `common.h`, добавить receiver в `query_t` и общий
  `rebuild()`. `view/lazy_view` ленивые и этого не требуют.
- 🟡 (отложено) **частичные дампы/дельты/selective components** — будущий save/replay/network workflow,
  не часть MT structural/query rebuild задачи.
- 🟡 (qol) **оптимизация random/forward deletion** (sidecar dense-owner array) — если станет узким местом.
- ✅ **schema detection/version guard уже есть; migrations — дальний ящик:** component key = hash type name, recursive consteval
  `layout_hash<T>` учитывает порядок и типы полей/контейнеров, registry fingerprint пишется в snapshot,
  container имеет version и mismatch громко reject-ится. Это compile-time structural hash, который и
  требовался для текущей стадии. Настоящая migration = преобразование старой схемы в новую; не
  планировать до первого реального требования backward-compatible save, затем добавить явные
  per-version adapters, не эвристику.
- 🟢 (qol) **auto-create allocator для `view<T>`** (сейчас нужно вручную `get_or_create_allocator<T>`).

### `libs/catalogue` — function wrappers, trace/log и deferred MT-dispatch
- 🟡 (н) **MT function decorator через domain strategy** — ✅ первый срез реализован в
  `catalogue/deferred.h`: `fn_ptr` остаётся immediate/traced, `fn_deferred_ptr` сохраняет сигнатуру
  free void-функции, но record-ит owned функцию+аргументы в
  `catalogue::mt::domain<IDENTITY_TAG, STRATEGY>` и
  откладывает тело до соответствующей pipeline-фазы.
  Execution-domain получает executor через `set_executor`; trace-domain остаётся отдельной, независимой
  осью. ds регистрирует именно wrapper; `ds::on_effect` остаётся gameplay-реакцией, не транспортом MT.
- ✅ **identity domain отделён от reusable strategy policy** — `domain<IdentityTag, Strategy>` владеет
  static executor binding, тогда как один policy type можно безопасно использовать в нескольких доменах.
  `preset::{parallel_collect,serial_elect,structural_elect}` покрывают нейтральные частые комбинации.
  Низкоуровневое описание strategy как policy type остаётся доступно, например:
  `catalogue::mt::collect<key::entity_arg<0>, order::source_then_sequence,
  commit::parallel_groups>`. Минимальные оси: arbitration (`collect`/`elect`, позже `join`), key extraction,
  deterministic total order, commit lane (`parallel_groups`/`serial`/`serial_structural`) и overflow policy.
  Для collect разные key-группы могут исполняться параллельно, внутри одной группы — последовательно в
  стабильном порядке. Elect выбирает победителя детерминированным rank и обычно commit-ится позже в ST.
- 🔴 (дальний backlog) **multi-participant `reserve`/`write-set`** — не входит в ближайший scheduler slice;
  вернуться после реального gameplay-кейса, которому недостаточно одного arbitration key.
- ✅ **`local_sequence` как часть детерминированного ключа (первый срез)** — каждый effect получает глобальный
  монотонный id `source_index * sequence_capacity + local_ordinal`; ordinal общий для execution-domain одного
  script pass. Порядок прихода задач/atomic append-index нельзя использовать как semantic order: они зависят
  от планировщика. Базовый порядок collect:
  `(group_key, source_id, local_sequence, function_id)`; дополнительные варианты задаёт policy. Переполнение
  sequence/buffer — громкая ошибка, не drop и не недетерминированный fallback. `mt::record_scope`
  инжектит source/index и local ordinal; executor имеет dense multi-record journal. Уникальность source в
  worklist — инвариант producer/scheduler; journal не сканирует вызовы для поиска/дедупликации ошибочных задач.
- ✅ **phase contract executor-а + первый live lane** — `begin_record` → параллельный `record` →
  barrier/`seal` → `commit`/parallel `dispatch_group`+`finish_commit` реализованы; отсутствие executor/scope
  и overflow громко ошибаются. tile_frontier использует parallel collect → ST serial_structural
  elect в apply. Осталось формализовать ownership/lifecycle смены `set_executor` для нескольких pipelines.
- ✅ **128-byte inline аргументный storage только для памяти текущего шага** — dense journal копирует
  copyable value args, type-erased call object не аллоцируется; oversized/over-aligned signature падает
  compile-time. `string_view` превращается в owned `std::string` (его длинное содержимое пока может иметь
  внутреннюю аллокацию), ссылки запрещены. Дальше: member-functions/custom bounded codecs. Это НЕ serializer.
- 🟡 (qol) **простое раскрытие стандартных типов в trace/log** — bool/integral/floating/enum/string/view
  форматируются без аллокаций; сложные gameplay-типы остаются opaque или получают явный formatter.
- ✅ **старый `act::effect_sink` seam удалён** — аудит не нашёл implementations/`emit` calls;
  typed executor уже закрывает MT-dispatch, `on_effect` остаётся gameplay reaction, а replay получит
  отдельный owning input/log layer.
- ✅ **deferred-call взаимодействий (п.16)** — record-time typed capture + collect/elect arbitration →
  barrier → parallel/ST commit встроены в actor pipeline. Это только deterministic in-memory execution;
  catalogue не участвует в replay.
- ✅ **тесты первого среза** — mirrored ds-call signature, два независимых ds-effect,
  отсутствие executor/scope, `local_sequence`, overflow, parallel record schedule, elect winner,
  `target_not_source`, collect order, parallel groups, независимость trace-domain, structural ST integration,
  1-vs-4 worker bit identity и resume bit identity в live tile_frontier. Отдельный benchmark проверяет
  hash+полные bytes при 1/2/4/8 workers и печатает scaling.

### `libs/demiurg` — ресурсы/моды
- ✅ **discovery/manifest шаг перед созданием ресурсов** — shadowed resources из модулей ниже
  приоритета больше не создают `resource_interface`; winner-module создаёт primary + свои
  supplementary; временные discovery/manifest контейнеры отпускаются после инстанцирования.
- ✅ **catalogue logging/trace домен `demiurg`** — `flow` фиксирует requests/external jobs/фактические
  переходы уровней ресурсов; `trace` оборачивает `parse_resources`, `append_resources`,
  `discover_resources`, `resource_loader::{request,update,external_done}`.
- ✅ **tavl list pattern `path:name` / `path:index`** — `.tavl` с `//---` разворачивается в отдельные
  ресурсы; именованные записи имеют canonical id `path:name` и index-alias `path:N`, unnamed записи
  доступны по `path:N`; пустые секции работают как index holes для partial override по индексу.
- ✅ **partial override list-записей** — модуль с более высоким приоритетом может переопределить только
  нужную запись list-файла по `name` или по индексу без имени; индексные alias/metadata наследуют
  стабильную позицию из базового списка.
- ✅ **диагностика list-pattern** — subresource хранит start-line/offset/size; tavl diagnostics могут
  выводить реальные строки исходного файла, а секция может быть восстановлена из файла после очистки.
- ✅ **resource memory lifecycle** — добавлен флаг `hot_unload_to_cold` для ресурсов, чья CPU-копия
  очищается после GPU upload; painter render-config source может оставаться warm без удержания текста.
- ✅ **stable `resource_handle`** — `{const resource_system*, utils::id hash}` (хеш логического id, не
  указатель); резолвится через `system->get(hash)` (O(1) `resources_by_hash`), `get<T>()` проверяет
  `loading_type_id`. Переживает `clear()`+re-`parse_resources()`: тот же id снова резолвится в новый
  объект (`rebuild_hash_index()` в конце parse/append). API: `handle(id)`/`handle(hash)`/`resource_hash(id)`.
  Потребители мигрированы: `flow::image_ref`/`sprite_sample` держат handle; tile_frontier message-структуры
  (`command_sound_play`/`command_load_resource`/`command_gpu_transition`/`command_gpu_done`) — через
  `resource_ref` (handle + `direct`-fallback для synthetic-ресурсов вне registry, напр. UI-`font_resource`).
- ✅ **demiurg↔Lua resource API** — 4 глобала в UI-sandbox (host-side в `simul::game_host`, не в demiurg):
  `request(id)`→`resource_handle`, `require(id)`→lua-модуль (движок владеет `require`, cycle-guard cache,
  относительные пути от текущего модуля), `find(prefix)`/`filter(substring)`→массив handles по обоим
  registry. `resource_handle` — lua-usertype (`valid/id/hash/state/usable/final_state/top_state`).
  UI bootstrap теперь `require("ui/entry")` (вместо `visage::load_entry_point` с диска → `set_entry_point`);
  `app.image`/`app.play_sound` принимают handle, не имя. Новый тип `lua_script_resource` (`register_type("ui","lua")`).
  НЕ вошло: import-rule таблица «type IS the path» (longest-segment match) — тип по-прежнему через
  `find_proper_type`; `find` пока prefix-, не by-kind; `:name` для lua-модулей не определён.
- 🟡 (н) **asset reuse/unload policy** — registry/handles и CPU→external GPU pipeline уже есть;
  ближайший пробел — корректно вернуть GPU texture/buffer slot после hot unload и переиспользовать его.
  Более широкий «полноценный asset manager» выделять только по следующему реальному требованию.
- 🔴 (дальний backlog, 2026-07-19) **упаковка immutable engine resource module** — после стабилизации
  project/app config и формата ресурсов собирать весь `resources/engine` в архив либо embedded blob,
  чтобы topology-конфиги и базовые engine assets нельзя было подменить рядом с готовым exe. Не делать
  особый CMake→C++ путь только для `app.tavl`: упаковка должна покрыть модуль целиком и сохранить
  обычный demiurg resource workflow на этапе разработки.
- 🟡 (техдолг) **дать assets worker доступ к общему worker pool:** отдельный asset thread остаётся
  coordinator-ом и может ждать завершение загрузки, но независимые CPU prepare/dependency jobs должен
  уметь fan-out-ить в pool. Зафиксировать отсутствие nested dispatch/wait из pool-задач, корректные
  pending/failed transitions, публикацию после join, diagnostics и shutdown. Per-tick slicing,
  cancellation и priorities отложены.
- ⏸️ (дальний ящик) migration/versioning metadata при первом реальном несовместимом формате ресурса.
- 🔴 (н, ближайшее для GPU assets) policy автоматической выгрузки/reuse ресурсов после использования;
  точный painter-side порядок описан в разделе `libs/painter`.
- ✅ (qol) диагностика циклов dependency graph.
- 🟢 (qol) строгий контракт zip до parse; `append_resources` при коллизии id (сейчас skip+warn,
  новый manifest path уже не создаёт пропускаемые коллизии).

### `libs/flow` — анимации *(фундамент формата/playback готов; реализацию ждать первого потребителя)*
- ✅ **CMake target + базовые тесты** — `devils_flow` оформлен, `flow_test` покрывает parsing/playback/sampling.
- ✅ **формат анимаций + demiurg list pattern** — `animation_resource` читает `.tavl` subresources
  через `list_section`, поддерживает `id:name`/`id:index`, real-line diagnostics идут через offset.
- ⏸️ **Следующий slice — только вместе с первыми найденными 2D assets/живым потребителем:** модель
  времени (tick/render/clip), 2D data model и ECS-компоненты состояния анимации.
- 🟡 (н) **mapping mood state → flow animation**; interpolation/blending/crossfade; callbacks через `act`.
- 🟢 (н) **разграничить callbacks по потокам** (что можно на render thread, что через intents).
- 🟡 (qol) **2.5D directional sprites** (DOOM-style) — зависит от типа игры.
- 🔴 (qol) **3D skeletal clips**; **GPGPU bone matrices** (batched skinning) — крупное, на будущее.
- 🟡 (н) **расширить тесты** (loops/callbacks/thread-boundary/ECS-компоненты).

### `libs/input` — ввод/окно (GLFW)
- ⏸️ **control schemes/contexts** (character/vehicle/menu) + стек/приоритет — не нужны, пока хватает
  одного gameplay/UI контекста.
- ✅ **bindings из config** — `input::bindings_config`, TAVL round-trip, live reload/save поверх default map.
- ✅ **mouse buttons/wheel как first-class** в event/window path; canonical mouse key names входят в bindings.
- 🟡 (qol, позже) геймпады/джойстики; live rebinding UI (конфликты + feedback); event queue/frame snapshot.
- 🔴 (qol) убрать статический global state — только если понадобятся несколько окон/input-доменов.
- 🟡 (н) **тесты** (state machine `events`, key-name mappings).

### `libs/mood` — FSM состояний *(основную задачу закрывает)*
- ✅ **FSM-описания из C++ строк → resource/config** — tile_frontier загружает mood-конфиг через demiurg;
  общий `mood::registry` позволяет держать несколько стабильных таблиц независимо от GOAP.
- 🟡 (qol) **inline-выражения / dotted-имена** (парсер запрещает `.`; сейчас dot-free имена).
- 🟢 (qol) явные ошибки на лимит 8 guards/8 actions; helper idle-loop с cap; диагностика parser errors с позицией токена; serialization/debug view состояния.
- 🟢 (н) **тесты** (`blocked`, guards в `on_exit`/`on_entry`, internal transitions).

### `libs/options` — CMake build contract
- 🟡 (н) **флаги детерминизма отдельными interface targets** (fp-модель/fast-math/denormals/FMA/precise) — не всем либам нужны одинаковые ограничения. Связано с [[determinism-replication-architecture]].
- 🟢 (qol) presets instruction sets (SSE/AVX…) + понятный API выбора target CPU; MinSizeRel/release policy; sanitizer/debug targets; политика `-Werror`; ревизия PUBLIC/INTERFACE/PRIVATE; задокументировать комбинации.

### `libs/painter` — Vulkan render graph
- ✅ **queue model (2026-07-19):** typed graphics/transfer/compute roles; отдельные families, несколько
  queue одной universal family и alias одной физической queue реализованы; alias синхронизирован
  общим mutex. Asset upload использует EXCLUSIVE sharing + release/acquire ownership transfer между
  transfer/graphics families; host fence синхронизирует этапы.
- ✅ **waitIdle audit:** в обычном frame path широких waits нет. `wait_all_fences()` ждёт graphics submit,
  `graphics queue.waitIdle()` остаётся только при drain перед разрушением swapchain/surface, потому что
  обычный fence не сигнализируется завершением present; `device.waitIdle()` — финальная страховка
  teardown. Убирать их без `VK_KHR_present_wait`/эквивалентного доказательства не нужно.
- 🟡 (н) **GPU command coverage:** `clear_color`/`clear_depth` уже имеют реализации; пройти остальные
  реально используемые draw/dispatch/transfer команды, убрать заглушки и добавить focused tests.
- ✅ **render-config через demiurg tavl list pattern** — категории render-config могут быть одним
  list-файлом с subresources; связи резолвятся по `name`, diagnostics получают label/line-offset,
  текст config-resource сбрасывается после parse без перевода ресурса из warm.
- 🟡 (н) **стабилизировать `.tavl` data model** (схемы/валидация/ошибки/примеры); строгая валидация render graph; контракт UI-buffers/texture-array/shader-prep + тесты.
- 🔴 (н, ближайшее) **освобождение GPU-ассетов:** `unload_hot` пока только сбрасывает index. Для
  texture после fence-safe границы поставить null texture в descriptor, уничтожить старые VMA image/view
  и вернуть slot в `empty`; для mesh/buffer удалить его из всех пар/списков `draw_group`, затем безопасно
  уничтожить storage и освободить slot. Покрыть reuse и отсутствие stale draw focused-тестами.
- ✅ **очистка CPU shader payload** — после создания pipeline `glsl_source_file::unload_warm()` чистит
  GLSL-текст и подготовленный SPIR-V; `.spv` resources чистят bytes через `unload_warm`.
- 🟡 (qol) device-local draw groups + GPU indirect; форматы (mips/arrays/MSAA/RT roles); ✅ legacy `painter_base`/старый `execution_pass` вынесен в `exclude/`; GLSL-компиляция на render thread как fallback.

### `libs/simul` — каркас симуляций/акторов
- ✅ **broker/topology/app runtime foundation вынесен из tile_frontier** (= п.12): lock-free channels,
  стандартные сообщения/системы, lifecycle, window/settings/loading helpers и jthread ownership.
- ✅ **app-shell завершён:** расширяемый `worker_systems`, typed registry, стандартная worker factory,
  optional render/sound/assets и project workers; ручные project bindings и фиксированные slots удалены.
- ✅ граница текущего app-shell engine events ↔ project gameplay bindings закрыта: generic API ставит
  `game_host`, project добавляет только gameplay UI hooks; первый ECS system runner — п.11.
- 🟡 (qol) бюджеты каналов + runtime-рост; судьба advancer как владельца timing loop vs отдельный scheduler.
- 🟢 (qol) stop token / jthread-friendly API.
- 🟡 (н) **расширить тесты:** lifecycle/broker-before-init/typed optional workers/stop-before-first-tick
  покрыты; остаются delivery/backpressure и partial-init/shutdown сценарии.

### `libs/sound` — звук (miniaudio `system2`)
- 🟡 (н) **контракт выгрузки `sound_resource`** — нельзя освобождать `data`, пока active task её держит (риск use-after-free).
- 🟢 (н) **PCM: подключить или убрать из форматов** (сейчас `data_type::pcm` → пустой decoder); **тип звука в play-команде** (сейчас всё как `sfx`).
- ✅ **`set_source_volume` + группы громкости** (master/music/talk/talk_pos/ui_effect/sfx), включая
  live-обновление активных voice и восстановление после смены playback device; 🟢 (н) профилировать `system2::update`.
- 🟡 (qol) звуковые эффекты окружения (reverb/filters); формат Opus; 🟢 (qol) проверить 3D на реальных сценах.
- 🔴 (qol) сложные модели 3D (priorities/virtual voices/occlusion); потоковый источник + capture device (VoIP).
- 🟡 (qol) **почистить OpenAL** (после окончательного решения по miniaudio).
- 🟡 (н) **тесты** (start>0, after, underrun, device fallback, snapshot).

### `libs/utils` — низкоуровневые инструменты
- 🟡 (н) **`file_io` на коды ошибок / `std::expected`** вместо исключений (нужно загрузчикам/фону/редактору).
- 🟡 (н) **расширить тесты** (allocators/spatial/string_pool/compression/serializer).
- 🟡 (qol) разнести `core.h` (diagnostics/math/paths/unicode/crc); перенести serializer из `aesthetics`; уменьшить тяжесть частых заголовков.
- 🟢 (qol) сгруппировать `thread/` (channel/* vs pool/*), аллокаторы по сценариям, выделить `spatial/` + единые тесты.
- 🟡 (qol) разделить публичный vs experimental API: ✅ `actor_ref`/`context_stack` и три неиспользуемых allocator-прототипа вынесены в `exclude/`; старые dispatcher/loader-подходы ещё требуют аудита.

### `libs/visage` — UI (Nuklear + Lua)
- ✅ **image Стадия 1** + **кодировка id** (тип/mirror/индекс/sampler_id) + **Стадия 2** (стенсил cooldown/mix) + **bindless v2** (раздельные sampled-image + sampler-пул, nearest-маски через `sampler_id`). ⏳ далее: `screen_px_range` в push-константу (не runtime); общий GPU-api header для C++ и шейдеров (embed как constexpr); composite/геральдика (type=3); demiurg-ресурс как источник картинки; полный descriptor_indexing (>16 слотов).
- ✅ **базовая error isolation:** UI entry вызывается как `sol::protected_function`; ошибка логируется,
  незакрытые Nuklear windows принудительно закрываются, instruction/time hook уже существует.
  🟡 Остаток: вынести hardcoded лимиты в engine settings, добавить wall-time/instruction/GC policies и
  явную реакцию на превышение (skip frame/disable state), не роняя host.
- ✅ **read-only API к ресурсам и gameplay:** demiurg `request/require/find/filter` + project
  `act_predicate/act_number/act_string/act_describe`; mutating Lua backend намеренно отсутствует.
- 🟢 (н) **профилировать/бюджетировать Nuklear** (Lua update, `nk_convert`, буферы).
- 🟢 (qol) `endf` → `fin`; убрать старый `draw_resource`/`draw_stage`.
- 🟡 (qol) closable vars (Lua 5.4); runtime reload UI; система числовых units (DPI/percent/relative + restore при resize); debug overlay как системный инструмент; убрать глобальный `nk_context*` из bindings (multi-context).
- 🔴 (qol) UI над юнитами (world-space).

---

## Проект `cardgame` — первый внешний потребитель движка (сопоставление 2026-07-21)

Дизайн-документы лежат в `exclude/gamedesigns/cardgame/` (design/combat_pipeline/engine_gaps —
первоисточник; elements/mechanics/characters/archetypes/power_budget/technical_scope — контент и объём).
`cardgame` рассматривается как **первый реальный проект на движке** и одновременно как проверка целевой
модели «проект = конфиги + заполнение реестров». С 2026-07-22 активен первый headless combat kernel;
раздел по-прежнему собирает требования в кучу, а детальный дизайн каждой следующей задачи делается отдельно.

### Главный архитектурный вывод

`tile_frontier` — **риал-тайм** актор-симуляция (per-frame фазы, MT-декомпозиция, непрерывное game-время).
`cardgame` — **пошаговая дискретно-событийная** игра: единица времени — ДЕЙСТВИЕ игрока (розыгрыш карты),
всё остальное (намерения врагов, призывы, countdown) встроено в цикл действия, а порядок разрешения строго
детерминирован. Это НЕ второй риал-тайм слайс, а **новый режим исполнения** поверх тех же реестров.
Уточнения автора (2026-07-21):

1. **Детерминизм — только в пределах одной ОС и одного билда.** Кросс-платформенная/кросс-компиляторная
   воспроизводимость НЕ в фокусе этого проекта, поэтому **fixed-point снят** (бывший CG-15). Хватает
   воспроизвести результат внутри того же билда — это ровно контракт текущего replay-фингерпринта
   («same build/config/resource fingerprint»). ds может продолжать считать в double. Модель
   `state[t+1] = f(state[t], intentions[t])` остаётся полезной для headless-replay баланс-тестов;
   netcode/rollback не нужны сингловой roguelike (см. [[catalogue-determinism-design]],
   [[determinism-replication-architecture]]).
2. **Карточная очередь — ПРОЕКТНАЯ система; массовый interaction resolver — движковый.** Конкретный
   порядок `primary → elemental reaction subtree → retaliation subtree → death → next primary`, окна карт
   и продолжения принадлежат project FSM. Однако форма «тысячи typed instances → semantic seal → serial
   commit внутри write-target при parallel targets → deterministic children/outcomes» повторяется и в
   real-time/FPS, поэтому вынесена в `libs/resolve`. `catalogue` collect/elect остаётся механизмом deferred
   вызова правил, а не authoritative очередью damage jobs. Не переносить в движок карточные стадии,
   таблицу стихий, effect stacking или UI-порядок.
3. **`cardgame` = ВТОРОЙ живой gameplay-потребитель**: минимальное headless-ядро уже подключено отдельным
   подпроектом, полный app shell всё ещё ждёт п.11/п.15/тех-долг «Перенос из tile_frontier в либы».
   Переиспользуемая часть включает resumable step-каркас/presentation checkpoints и низкоуровневое
   `libs/resolve`-ядро; UI-инструментарий взаимодействия (CG-4/CG-13), tooltip+глубина в painter (CG-7),
   шаблонная сцена-гексокарта (CG-10). Конкретные боевые стадии, реакции, ресурс, preview-логика и AI
   остаются проектными.

### Что движок УЖЕ закрывает (признать контрактом, не переделывать)

| Потребность cardgame | Существующее в движке | Комментарий |
|---|---|---|
| Data-driven карты/реакции/персонажи (не скрипт на карту) | `demiurg` list-pattern `path:name` + tavl | схема карт = новый resource type + документы `//---`; ~212–352 карт = контент |
| Эффекты карт, предикаты, детерминированный RNG, intent | `act` + `devils_script` + `act::building_blocks` | ТОЧНО модель «карта = building blocks + ds», а не C++ на карту |
| Сериализуемое состояние матча | `aesthetics::serial` + snapshot + replicated/derived split | UI-анимации = derived (не пишем); mid-turn resume ложится сюда |
| Флаги/метки на сущности | `aesthetics::flag_set` | НО countdown в game-времени; карточный countdown двигает проектный pulse — см. CG-1/CG-3 |
| Turn как отдельная координата | time-модель `turn_index/duration/deadline` | тип есть; конкретные turn/action/pulse — независимые проектные координаты внутри gameplay-FSM |
| Числовые характеристики (HP, инициатива) | `act::register_stats` generic aggregate (B-д) | инициатива/резонанс = агрегаты поверх этого |
| Отношения сущностей (владелец призыва, цель) | entityid-поле + `exists()`-проверка (правило relations) | `actor_eating.target` = эталон |
| Пауза/скорость/read-only UI-шов к act | pause_state, `app.act_predicate/number/string/describe` | основа preview (CG-8) |
| Headless-запуск, звук, ввод-биндинги | `render.enabled=false`, sound, `input::bindings_config` | основа headless-харнесса (CG-14) |
| Массовые damage/effect instances и MT consequences | `libs/resolve` journal/frontier/target groups | общий execution/provenance contract; формулы и stage order задаёт проект |

### Задачи (пробелы) — что движку не хватает

Легенда прежняя (🟢/🟡/🔴 усилие, [необходимо]/[QoL]). Разбивка ответственности **[движок]** vs
**[проект]** проставлена по уточнениям автора (2026-07-21/22): движок даёт инструменты, чёткие переходы
состояний и generic interaction-resolution execution; конкретный боевой stage order, реакции, ресурс,
preview и AI — проектные.

- **CG-1. Фазовый gameplay-FSM боя в C++ + понятия ход/действие** — 🟡/🔴 **[первый срез готов] · [движок: каркас
  + анимационный барьер; проект: фазы/карты]**. **РЕШЕНИЕ (2026-07-21): FSM боя описывается в C++, НЕ
  конфигом.** Конкретные пайплайны исполнения (пробежаться и применить DoT, инстанс атаки и т.п.) всё равно
  остаются C++-функциями; config дал бы лишь перестановку пары вещей местами, но стоил бы индиректности,
  усложнил гейт ожидания анимаций и сериализацию. Прецедент в кодовой базе — `app_state{boot,loading,game}`
  (плоский C++ enum в оркестраторе, намеренно НЕ `mood`); матчевый FSM — тот же класс задачи (оркестрация
  control-flow), не per-entity. «Данные» карточной игры живут НЕ в FSM, а в КАРТЕ: последовательность
  действий карты = ds поверх building blocks (`attack_instance`/`draw`/… = C++). Движковые дельты: (а)
  фазовый FSM с **сериализуемым курсором прогресса** (не C++-корутина — иначе не сделать resume в середине
  хода, CG-11); (б) **анимационный барьер как presentation-гейт**, не влияющий на sim-результат. Полная
  раскладка фаз/переключений/анимации + привязка изменения хар-к к анимации — в подсекции ниже. Источник:
  combat_pipeline §«Рабочий порядок хода», §«Действие игрока».

- **CG-2. Вычисление порядка внутри карточного пайплайна** — 🟢 **[необходимо] · [проект]**. Ничего
  архитектурно сложного: ВНУТРИ пайплайна дополнительно разбиваем последовательности действий разных
  подсистем и считаем их отдельно (стабильный приоритет из combat_pipeline). `catalogue` collect/elect —
  это deferred-вызов правил, а НЕ механизм этой очереди. `libs/resolve` даёт typed provenance/frontier,
  semantic seal и target-parallel commit, но не выбирает карточную последовательность. Источник:
  combat_pipeline §«Стабильный порядок», §«Общие правила очереди».

- **CG-3. Проектный countdown pulse** — 🟢/🟡 **[первый срез готов] · [проект над CG-1]**.
  Карточные countdown отсчитываются НЕ в game-времени (`flag_set`) и НЕ по всякому действию/смене состояния.
  Проект в явном под-шаге вызывает pulse, а решение «даёт ли текущая карта pulse» становится отдельным
  проектным ds-предикатом над данными карты (`quick/no_countdown_advance` ⇒ false). Поэтому action index и
  pulse index независимы: карта может считаться действием без pulse, а принудительный слив конца хода даёт
  pulse без действия. Лимит поколений, single-fire guard и валидатор циклов также проектные. Источник:
  combat_pipeline §5, §7.

- **CG-4. Движковый UI-инструментарий взаимодействия + прокидка состояния игрового FSM** — 🔴
  **[необходимо] · [движок]**. Крупная часть. Движок должен дать инструменты: выделение (highlight) некоторых
  элементов UI, навигация мышью и клавишами, СПИСКИ и их свойства, несколько стандартных систем-кусочков UI
  (например список карт в руке слева и др.) + дополнительно прокинуть в UI информацию о состоянии текущего
  игрового FSM (CG-1). Реактивные окна (парирование/кража/до/после) при этом = проектная логика пайплайна
  (считается как CG-2), а не отдельный движковый механизм. Тесно связано с CG-13. Источник: design
  §«Представление доступных карт»; combat_pipeline §5.

- **CG-5. Ресурс инициативы (две модели) + предпросмотр как параллельное состояние S+1** — 🟡
  **[необходимо] · [проект]**. Сам ресурс несложен (две формулы: мана vs текущая+следующая инициатива с
  переносом/зарядом/разрядом/долгом) над generic stats (B-д). Предпросмотр стоимости входит в ЕДИНУЮ систему
  предпросмотра, которая скорее не dry-run, а **параллельное состояние S+1**: сначала на него отображается
  результат розыгрыша карт и рисуется ПОВЕРХ текущей информации. Для S+1 необязательно вычислять весь
  пайплайн (хотя можно). Источник: design §«Ресурс действий».

- **CG-6. Стихийные метки + таблица реакций** — 🟢/🟡 **[необходимо] · [проект]**. Чистые данные + отображение
  поверх этих данных: инертные метки (per-turn), таблица 28 реакций, consume-on-reaction, резонанс металла,
  «Инь-янь», preview. Полностью над act+ds+demiurg; отдельного движкового механизма не требует. Источник:
  elements.md.

- **CG-7. Поле боя в painter: тултип по наведению + глубина сцены** — 🟡 **[необходимо] · [движок-поддержка
  + проект]**. Поле битвы рисуем в `painter`. Нужно поддержать вывод ТУЛТИПА по пересечению курсора мыши с
  элементом на поле, и само поле обладает свойствами ГЛУБИНЫ (спрайты визуально чуть дальше/чуть ближе в
  зависимости от особенностей сцены). Слоты призывов/агро/redirect — проектный ECS над relations
  (entityid-поля + `exists()`). Источник: combat_pipeline §4, §6.

- **CG-8. Единая система предпросмотра (S+1)** — 🔴 **[необходимо] · [проект]**. Крупная часть; частично
  описана в CG-5: параллельное состояние S+1 с наложением прогноза (цели, реакции, изменение инициативы,
  сдвиг countdown врагов) поверх текущего экрана. Опирается на act `describe` + read-only UI-шов
  (`ui_predicate/…`). Источник: design §6; combat_pipeline §«Представление игроку».

- **CG-9. Схема карточного контента + множество валидаций** — 🟡 **[необходимо] · [проект-контент +
  движок-проверка масштаба]**. Механически крупная, но несложная: схема карты (теги/цели/стоимости/
  улучшения/правила колоды), документы demiurg list-pattern (один файл = N карт), МНОГО разных валидаций
  (причинные циклы, «смешанный статус» без порядка, countdown после форс-слива, недопустимые цели).
  От проекта — множество описаний карт; **надо убедиться, что demiurg легко тянет такой объём и не
  раздувается по ресурсам**. Источник: engine_gaps «Описание карточного контента».

- **CG-10. Гексокарта как отдельная сцена (шаблонно в движок)** — 🟡 **[необходимо] · [движок]**. Гексокарта
  — отдельная сцена внутри игры. Сами гексы — простая задача; сделать ШАБЛОННО и добавить в devils_engine
  (как в будущем и квадратную карту): hex-математика + ориентированный-вперёд граф + проверка связности/
  отсутствия непреднамеренных тупиков + 2D-рендер. Источник: design §«Карта забега».

- **CG-11. Состояние забега + resume** — 🟢 **[необходимо] · [проект]**. Run seed, состояние этажа, история
  узлов, временные мутации колоды, resume. В целом должен уже работать текущий сериализатор с небольшими
  дополнениями (состояние матча полностью сериализуемо, UI-анимации = derived). Источник: engine_gaps
  «Состояние забега»; combat_pipeline проверка 3.

- **CG-12. Постоянный профиль/коллекция** — 🟡 **[необходимо] · [проект]**. Профиль/коллекция (анлоки,
  библиотека колод) = часть сохранения игры над run-save. Миграция версий контента → дальний ящик (пока не
  делаем). Источник: engine_gaps «Постоянная коллекция».

- **CG-13. UI-инструменты эффектов в `visage` + общий элемент-список** — 🟡/🔴 **[необходимо] · [движок]**.
  `visage` сам по себе должен дать множество UI-инструментов для разных эффектов (квад-текстурки и градиенты
  делались как раз под это — [[ui-texture-id-and-effects]]). Дополнительно — описать ПОВТОРЯЮЩИЕСЯ элементы:
  список карт в руке и список отложенных действий предположительно очень похожие друг на друга списки → один
  переиспользуемый UI-list. Смыкается с CG-4. Источник: design §«Представление доступных карт»; engine_gaps
  проверка 1.

- **CG-14. AI: сценарные намерения врага + полноценный AI игрока для симуляций** — 🟡 **[необходимо] ·
  [проект]**. AI противника скорее всего строится ЧИСТО на заранее заготовленной последовательности действий
  внутри хода (данные через ds — хороший fit). Дополнительно было бы неплохо добавить полноценный AI ИГРОКА,
  чтобы гонять симуляции (headless batch, баланс). Источник: engine_gaps «AI карточного противника»,
  «Автотест боя».

- ~~**CG-15. fixed-point**~~ — **СНЯТ (2026-07-21).** Достаточно воспроизводить результат в пределах одной
  ОС и одного билда; кросс-платформенный детерминизм не в фокусе. ds продолжает считать в double.

### CG-1 подробно: фазы, переключения, анимация, привязка хар-к к анимации

✅ **Движковое ядро уточнено и первый потребитель построен (2026-07-22):**
`libs/simul/turn_pipeline.h` — переиспользуемый,
header-only, по образцу `lifecycle_controller` (host-templated, единственный писатель курсора):
- `presentation_barrier` — транзиентное множество typed checkpoint `{task, gameplay|finished}` + watchdog;
  НЕ сериализуется; headless/анимации-выкл ничего не регистрируют и проходят инлайн.
- `turn_pipeline<Cursor>` — цикл «run_step до wait»; сериализует ТОЛЬКО проектный курсор. Барьер, ожидание
  и render task ids derived и на resume отбрасываются. Ход, действие, countdown pulse и generation движок
  не хранит: это независимые координаты конкретного проекта.
- Watchdog = **сторожевой таймер** потери ожидаемого события с render/flow-стороны. По таймауту pipeline
  защёлкивает fault, ровно один раз зовёт `on_barrier_timeout` и больше не двигает рассинхронизированный FSM.
- `turn_pipeline_test` (5 кейсов / 40 assertions) проверяет оба checkpoint, headless, resume без double commit,
  одноразовый watchdog и независимые action/pulse.
- `subprojects/cardgame` — первый project-owned FSM: своя интенция игрока, карта с pulse и без него, enemy
  countdown/attack, forced end-turn pulses. `cardgame_headless_smoke` гоняет три хода с fake render thread и
  headless и после каждого ввода требует одинаковый state; отдельно сохраняется до gameplay checkpoint.
- ✅ **Atomic resolution slice (2026-07-22):** внутри большого FSM появился project-owned
  `resolution_work + resolution_cursor` (не второй `turn_pipeline` и не `mood`). `attack_instance` отделён
  от `damage_instance`; карта сначала материализует все исходные attack/effect requests, а отдельный
  `resolution_work.plan` сохраняет порядок текста между ними. Атаки разрешаются по одной: modifier journal
  резистов → shield/HP → elemental reaction → returned damage → death boundary. Reaction/returned — явные
  damage channels общего resolver и по умолчанию нерекурсивны. Эффекты хранятся в отдельном проектном
  `combatant_state.effects` (**НЕ `aesthetics::flag_set`**) и возвращают
  `added/updated/immune/invalid_target/rejected`. Snapshot после primary commit сохраняет уже собранные
  reaction/response/effect очереди и продолжает их без double hit.

Остаток CG-1 — проектный: заменить hard-coded построители `resolution_work` записью из ds карты, заменить
native resistance row проектным ds modifier-script, подключить реальные broker-каналы render/flow вместо
fake-render и расширить таблицу реакций/стабильный приоритет (CG-2). Ниже — контракт, по которому проект
продолжает наполнять уже живой каркас.

Два слоя (оба в C++, данные — только карты):

- **Матчевый фазовый FSM** — грубая машина состояний матча в `game_state`. Мало состояний, control-flow,
  гейтится интенциями игрока и двумя типами сигналов анимации: gameplay point и finish.
- **Под-степпер тактов** внутри действия — идёт по атомарным эффектам карты, уступая управление на
  анимационных барьерах.

**Курсор прогресса (сериализуемый, для CG-11):** `{phase, step, card_id, beat_index, generation}` плюс
проектный authoritative state с независимыми `{turn_index, action_index, countdown_pulse_index}` — простые
данные, НЕ suspended-стейт корутины. Анимации и task ids derived: курсор заранее переводится на безопасную
границу, поэтому resume пропускает потерянную presentation-часть и не повторяет уже сделанный commit.

**Такт (beat)** = минимальная единица
`{ cue → gameplay-барьер → commit/result → finished-барьер }`:
- `cue` — presentation-эффект (запустить снаряд, вспышку, полёт карты); НЕ трогает sim-состояние;
- `gameplay-барьер` — ждём нотифай «пора»/«impact» от системы анимаций по ВСЕМУ батчу такта (мультиудар/AoE/реакции
  на нескольких сущностях = несколько cue разом → ждём все);
- `commit` — детерминированный sim-эффект (вычислить урон, реакции, металл-резонанс, проверка смерти).
  Мгновенный. Main отправляет авторитетный `result` обратно в render/flow (цифры, impact reaction, новое
  визуальное состояние);
- `finished-барьер` — ждём, пока анимация результата закончится, и только затем переводим gameplay-FSM к
  следующему такту/фазе (например enemy attack → positive effects).

Аннотации ниже: `[ФАЗА]` смена состояния FSM · `[INTENT]` ждём интенцию игрока · `[cue]` старт анимации ·
`[ЖДУ gameplay]` точка вычисления · `[SIM/result]` детерминированный коммит и ответ render ·
`[ЖДУ finished]` конец анимации перед следующим шагом.

**Полный порядок (сверка с combat_pipeline):**

1. `[ФАЗА turn_begin]` — начало хода (combat_pipeline §1): счётчик ходов, новая последовательность намерений
   врагов + их countdown, ресурс игрока (мана/перенос инициативы), обновление руки, автотриггеры начала
   хода, заранее поставленные в очередь эффекты игрока/противника. Каждый видимый шаг = полный такт с двумя
   checkpoints. Фаза автоматическая — реактивная карта из руки между под-шагами не разыгрывается. → проверка
   смертей/победы → `[ФАЗА awaiting_action]`.
2. `[ФАЗА awaiting_action]` `[INTENT]` — ждём интенцию игрока: выбор карты ИЛИ «конец хода». Это отдельный
   проектный `player_intent` (экземпляр карты/цели/оплата/выборы не должны расширять generic `act::intent`)
   через проектную очередь с обязательным dedup. Выбор →
   `[ФАЗА resolving_action]`. «Конец хода» → `[ФАЗА end_turn]`.
3. `[ФАЗА resolving_action]` — цикл действия (combat_pipeline §2), под-степпер:
   - `[SIM]` §2.1–2.3 проверка розыгрыша/целей/стоимости/формы, фиксация карты+владельца+целей, оплата
     (мгновенно, без анимации; после оплаты отмены нет);
   - §2.4 окно замены/перехвата ДО исполнения (кража номера) — проектная логика (CG-4-механизм);
   - §2.5 исполнение карты по её ds-последовательности тактов. **Пример «два снаряда по врагу»:**
     - такт 1: `[cue]` запустить снаряд-1 → `[ЖДУ gameplay]` impact → `[SIM/result]` урон-1 + реакции +
       металл-резонанс + проверка смерти, вернуть числа → `[ЖДУ finished]` цифры/вспышка закончились;
     - такт 2: `[cue]` снаряд-2 → `[ЖДУ gameplay]` → `[SIM/result]` урон-2 → `[ЖДУ finished]`;
     - такт 3 (взять карту из сброса): `[cue]` полёт карты → `[ЖДУ gameplay]` → `[SIM/result]` карта в
       список → `[ЖДУ finished]`.

     Здесь и работает **приём «разбить эффект на два несвязанных»**: `cue` (визуальный эффект) и `commit`
     (изменение хар-к) — это ДВА отдельных act-эффекта; `commit` запускается степпером ИМЕННО по нотифаю от
     системы анимаций, а не сразу за `cue`. Так «цифры вылетают» ровно в момент удара, а sim остаётся
     детерминированным (см. ниже);
   - `[SIM]` §2.6 карта → сброс/истощение/countdown;
   - §2.7 состояния исполнителя (положительные → DoT → отрицательные) — **каждый с явной паузой-тактом**:
     `[cue]` иконка → `[ЖДУ gameplay]` → `[SIM/result]` счётчик/урон → `[ЖДУ finished]`;
   - `[SIM]` §2.8 вызвать проектный **countdown pulse**, если ds-предикат карты разрешает его. Это НЕ
     инкремент action index: `no_countdown_advance` остаётся действием без pulse;
   - §2.9–2.10 разрешить готовые намерения врагов (основной/босс → stable id) + их состояния — полные такты;
   - §2.11–2.13 призывы «после каждого действия» (союзные → вражеские) + удаление одноразовых — такты;
   - §2.14–2.15 достигшие нуля отложенные эффекты игрока/противника — такты;
   - `[SIM]` §2.16 проверка победы/поражения → назад в `[ФАЗА awaiting_action]` (или `battle_over`).
4. `[ФАЗА end_turn]` — конец хода (combat_pipeline §7): триггеры «карта осталась в руке», затем
   **принудительные импульсы**: каждый импульс = `[SIM]` уменьшить countdown + разрешить готовые намерения/
   отложенные как в §2, тактами с анимацией; повторять, пока есть незавершённые countdown (лимит 3 поколений
   новых countdown — проектный валидатор/watchdog, CG-3). Затем эффекты конца хода, призывы «в конце хода»,
   корни (§7.9), положительные → снятие щитов → DoT → отрицательные, очистка стихийных меток, фиксация руки,
   списание/перенос инициативы → проверка итога → `turn_begin` или `battle_over`.

**⚠ TODO — реализовать и проверить порядок как иерархию групп (рабочий контракт 2026-07-22;
зафиксировать API только после scenario tests).** Перед расширением текущего `cardgame` ещё раз пройти весь расчёт урона и
проверить порядок его внутренних стадий в контексте ПОЛНОГО пайплайна разыгранной карты. Текущий атомарный
порядок `modifiers → shield/HP → elemental reaction → retaliation` доказывает работу `libs/resolve`, но не
обязан быть окончательным gameplay-порядком. В частности, надо заново расставить gather/prepare/route,
commit guards, elemental/status outcomes, listener scripts, retaliation и death boundary относительно
следующего эффекта карты и следующих участников действия.

Предпочтительная форма следующей ревизии — не плоско выбросить наружу все прежние §2.1–2.16, а сделать
крупные сериализуемые **группы**, каждая из которых владеет своим маленьким упорядоченным степпером:

1. **Начало хода.**
2. **Ожидание действия игрока:** карта, конец хода, побег или другой project intent.
3. **Начало действия игрока.**
4. **Эффекты карты.**
5. **Общий follow-up pass всех персонажей игрока, включая исполнителя.**
6. **Общий follow-up pass всех противников.**
7. **ActorStateTick персонажа, использовавшего карту.**
8. **Атака/заклинание противника.**
9. **Общий follow-up pass всех противников, включая исполнителя enemy execution.**
10. **Общий follow-up pass всех персонажей игрока.**
11. **ActorStateTick конкретного противника-исполнителя.**
12. **Конец действия** — после разрешения ответа противника.
13. **Конец хода.**

Предполагаемое внутреннее содержание групп:

- **Начало хода (1):** эффекты начала хода союзников → эффекты начала хода противников → добавить карты
  в руку → вычислить/обновить ресурс действия.
- **Начало действия (3):** validate/fix actor+card+targets/pay, затем replacement/intercept до исполнения.
  Успешная кража означает, что карта фактически НЕ разыграна: группы 4, 5 и 6 пропускаются, но группа 7
  выполняется. Это всё ещё полное действие игрока: `ActorStateTick` выполняется, action countdown
  продвигается и затем разрешается обычная очередь противников.
- **Эффекты карты (4):** полностью и последовательно разрешать атомарные инструкции текста карты, например
  `damage instance → damage instance → взять карту в руку`; не схлопывать multi-hit. Только после завершения
  всей карты заморозить её `execution_report` для follow-up групп.
- **Party follow-up / ActorStateTick (5–7):** это два последовательных party pass над ОДНИМ отчётом карты:
  все персонажи игрока, ВКЛЮЧАЯ исполнителя → все противники → отдельный ActorStateTick исходного
  исполнителя. У исполнителя больше нет специального self-follow-up pass. Каждый follow-up effect
  декларативно указывает, какие категории отчёта его включают (`has attack`, `has elemental reaction`,
  `has stat change`, ...). C++ вызывает подходящий script РОВНО ОДИН раз на rule, а script сам проходит
  стабильные списки всех атак, реакций или stat changes и решает, сколько attack/heal/other instances
  создать. Work source-party полностью разрешается до opposing-party; eligibility проверяется перед
  bucket каждого участника. Follow-up work не попадает обратно в исходный report и не может открыть
  follow-up-on-follow-up — это такой же жёсткий запрет, как retaliation-on-retaliation. После обоих pass
  ровно один раз запускается `ActorStateTick: DoT → negative → positive` исходного actor; остальные
  участники партии state tick не получают.
- **Атака противника (8):** аналог группы эффектов карты, но источником является project-defined
  последовательность ударов/заклинаний, а не обязательно объект карты.
- **Ответные группы (9–11):** симметричны 5–7 относительно источника: после enemy execution сначала
  общий pass ВСЕХ противников, включая исполнителя → общий pass всех персонажей игрока → отдельный
  `ActorStateTick` исходного противника. Общий инвариант:
  `Execution → SourceSidePartyFollowUps → OpposingSidePartyFollowUps → SourceActorStateTick`.
  Группы 8–11 повторяются на каждое готовое конечное enemy execution, а не превращают hits в действия.
- **Конец действия (12):** эффекты, которые должны произойти только после завершения действия игрока и
  связанного с ним ответа противника.
- **Конец хода (13):** до локальных end-turn эффектов разыграть все оставшиеся готовые атаки противников,
  разрешить корни и другие специальные эффекты, затем закончить ход.

Для локального пайплайна состояний персонажа принят более предсказуемый для игрока порядок:
**DoT → отрицательные эффекты → положительные эффекты**. Это намеренно заменяет старые строки выше
(`positive → DoT → negative`). Design-документ `combat_pipeline` уже синхронизирован; scenario tests ещё
должны проверить кодовые границы действия/хода, preview и тесты.

**Порядок участников follow-up party pass (решено 2026-07-22):** он намеренно выглядит хаотично, но
authoritative и воспроизводим. В начале pass фиксируется eligible participant snapshot; участники
сортируются по `(mix(combat_seed, action_token, side_domain, stable_entity_id), stable_entity_id)`.
Следовательно, action `42` может дать `[2,1,3]`, action `119` — `[3,2,1]`; player/enemy имеют разные
domain salts. Relative order одних actor остаётся тем же во всех party passes одного action даже при
изменении состава. Умерший до своего bucket участник пропускается, новый после materialization не входит.
Внутри одного actor несколько подходящих независимых follow-up rules сохраняют обычный project order
(`priority/definition/effect id/local ordinal`) и НЕ перемешиваются. Для forced end-turn execution без
player action используется отдельный монотонный cycle token. Этот порядок не меняет stable policy очереди
самих enemy executions (босс/основной противник, затем project order). ✅ Проектный
`materialize_follow_up_order` уже реализует и тестирует container-independent/subset-stable materialization,
разные side domains и громкую валидацию duplicate/invalid participant; подключение materialized snapshot к
группам FSM остаётся следующим слоем.

`execution_report` нужен как временный owning/snapshot-safe вход последующих групп, но не должен копировать
все тяжёлые outcomes. Предпочтительная реализация: `resolution_work` владеет стабильными typed trace-массивами,
а report содержит execution/card/actor, category mask и упорядоченные `effect_ref {kind,index}`/диапазоны
записей, созданных ТОЛЬКО группой 4. Script scope даёт `each_attack`, `each_healing`,
`each_elemental_reaction` или общий filter по kind. Для украденной карты допустим маленький отчёт
`action committed + card stolen`, но follow-up window не открывается, потому что нет факта успешного
`card executed`. ✅ Компактный category mask и `follow_up_enabler {any_of, all_of}` уже живут в коде;
текущий combat resolver отмечает attack/damage/stat-change/status/reaction/elemental/retaliation. Typed ds
итераторы и категории healing/attribute будут подключены вместе с соответствующими instance stores.

Нужен общий pointer-free **effect instance/outcome envelope** с provenance и project kind, но payload и
outcome остаются типизированными: `attack_instance`, `damage_instance`, `healing_instance`,
`agility_damage_instance`, draw/status/... . Семантический kind и знак значения ОРТОГОНАЛЬНЫ:
`attack(-5)` остаётся атакой и проходит attack/damage rules, а `healing(-5)` остаётся лечением и проходит
healing effectiveness/resistance/cap rules; C++ не выводит тип эффекта из знака. `execution_report`
перечисляет общие refs в semantic order, а ds легко фильтрует typed scopes и сам решает, сколько нового
work породить.

Точная проектная иерархия терминов:

```text
execution (карта / enemy ability / follow-up / status tick / pre-card effect)
  → effect_program
    → ordered beat[]
      → authored effect[] одного semantic batch
        → target_set
        → один вызов effect script
          → N typed effect/work instances
            → N typed outcomes и intrinsic children
```

Формат программы эффектов задаётся вложенным списком, например
`effects = [[eff1,eff2],[eff3],[eff4,eff5,eff6]]`. Внешний список строго последователен; каждый внутренний
список — один beat/batch. Presentation запускает эффекты одного beat одновременно, затем следующий beat;
gameplay всё равно вызывает `eff1..eff6` в стабильном порядке и не использует arrival order render-событий.
Один authored effect имеет ОДНУ animation identity и ОДИН script call, даже если script породил три damage
instances: presentation показывает один удар по цели, а после commit может вывести три отдельных числа по
outcomes. Та же форма effect_program разрешена для карт, DoT/status ticks, follow-up и enemy executions.

Death predicate вызывается после outcomes каждого instance и защёлкивает смерть сразу, но отмена действует
на границе beat: все заранее materialized effects/targets текущего внутреннего списка доигрываются, а
следующий beat для умершего required actor/target уже не запускается. Target sets всех authored effects beat
фиксируются authoritative gameplay ДО cue. Поэтому AoE/случайные цели и animated/headless получают один
результат независимо от физического порядка анимаций.

Рабочие targeter-ы проекта: `target` (зафиксированная выбранная цель, включая self/ally/enemy),
`random_target` (детерминированный выбор из eligible snapshot) и `all_targets` (AoE snapshot всех eligible
целей в stable order). Все три создают один authored atomic effect и один script call; target set может иметь
1 или N участников, а script порождает нужные per-target instances. Между beats targeter запускается заново:
умершая выбранная цель делает следующий beat для неё недопустимым, random может выбрать другую, AoE получает
новый snapshot. Внутри одного beat смерть не меняет уже зафиксированный target set.

✅ **Target binding зафиксирован в коде (2026-07-22):** default — каждый authored effect независимо
материализует target set в начале beat. Ненулевой явный `target_binding_id` связывает несколько effects и
переиспользует ровно один snapshot/random choice; одинаковый key с отличающимся targeter, fixed target или
eligible set является громкой ошибкой данных. `target` проверяет выбранную цель против eligible snapshot,
`random_target` выбирает один раз на authored effect/binding, `all_targets` сохраняет весь stable-order
snapshot. `cardgame_effect_program_test` проверяет independent/shared random selection, fixed target, AoE и
fail-loud validation.

✅ **Первый grouped effect-program slice жив (2026-07-22):** `subprojects/cardgame` хранит pointer-free
`effect_program → effect_beat[] → authored_effect[] → effect_ref`; typed recipes отделены от emitted
`effect_instance_ref`. Все target sets beat материализуются до cue, authored cues публикуются одним
presentation batch, после общего gameplay barrier scripts исполняются последовательно, а затем один result
на authored effect несёт его typed outcome range. `execution_report` сохраняет beat/effect identity,
target snapshot и диапазоны emitted plan/damage/effect outcomes. `double_strike` доказывает один authored
call с двумя attack instances, `fire_strike` — два authored effects одного beat, `combo_strike` — отмену
следующего beat после latched death; animated/headless и resume остаются идентичны.

Один `attack_instance` может породить N дочерних `damage_instance` под общей provenance. Damage по щиту и
остаток по здоровью лучше фиксировать отдельными instances/outcomes: shield-child уничтожает/уменьшает щит,
HP-child применяет остаток к здоровью. Это сохраняет наблюдаемые факты для скриптов, presentation и
follow-up и не прячет несколько authoritative writes в одном агрегированном поле. Точная route policy для
отрицательной атаки тоже выбирается семантикой attack/damage, а не неявным clamp до нуля.

После КАЖДОГО committed effect instance вызывается project death predicate для затронутой сущности — в том
числе после attack/damage, `healing(-N)`, follow-up attack/healing, каждого DoT и специального эффекта.
Результат смерти защёлкивается немедленно, а фактическое structural удаление actor/entity может произойти на
удобной явной boundary. Уже sealed/in-flight batch instances всё равно доигрываются (их анимации/cue уже
запущены), но перед переходом к СЛЕДУЮЩЕМУ FSM-step проверяется liveness требуемого исполнителя/цели: новые
card beats, follow-up pass или actor-state steps мёртвого участника больше не начинаются.
Состав такого batch определяется authoritative gameplay ДО отправки cue и одинаков в animated/headless;
render не может случайно решить, какие поздние instances переживут смерть, лишь потому что успел показать их.

Открытые вопросы этой ревизии:

- показывать ли materialized follow-up party order заранее в preview/UI или только объяснять его в
  execution log после действия;
- какой минимальный erased `effect_ref`/typed-store API нужен ds, чтобы report не стал толстым variant и
  при этом snapshot/describe/filter не зависели от указателей;
- в какой точке после latched death выполняется structural cleanup/free-slot и какие on-death scripts
  получают ещё доступ к snapshot погибшего;
- какие именно damage stages должны быть общими в `libs/resolve`, а какие останутся project continuation
  stages карточного боя. Не расширять общий API, пока эта группировка не проверена на нескольких картах.

**Детерминизм / headless / скорость (три режима, один sim-результат):**
- Барьер `[ЖДУ]` влияет ТОЛЬКО на то, КОГДА запустится следующий `[SIM]`, а не на его результат: во время
  ожидания FSM заблокирован, sim-состояние не меняется → `commit` детерминирован.
- **Анимировано** — барьер ждёт presentation-time (реальные часы, паузится, вне детерминизма).
- **Анимации выкл (настройка)** — checkpoints можно подтверждать на границах кадров, сохраняя ту же
  последовательность FSM без визуальной работы.
- **Headless (CG-14)** — ни `cue`, ни ожидания; такты идут вплотную, batch-скорость. Все три дают
  идентичный sim (countdown двигают явные проектные pulses, длительность анимации косметична).

**Движок vs проект в CG-1:**
- `[движок]` resumable step-каркас + project cursor; typed presentation checkpoints `gameplay/finished`;
  сторожевой таймер потерянного render-события; headless-прохождение пустых checkpoints.
- `[проект]` конкретные фазы и их порядок (явной traced-последовательностью, как actor-фазы в п.11);
  turn/action/pulse/generation; разбиение эффектов карт на `cue`/`commit-result` и их ds-последовательности;
  стабильный приоритет реакций (CG-2); окна замены/перехвата (CG-4-механизм).

### Порядок и первый срез

engine_gaps «наиболее важные проверки» + technical_scope «Этап 1. Проверка боевой модели» задают
естественный первый вертикальный срез, который проверяет самые рискованные допущения:

1. ✅ **Первый CG-1/CG-3 kernel** — новый растянутый по кадрам FSM, grouped project-owned effect program,
   batched animation checkpoints, target snapshots/bindings, execution report, resume и animated/headless
   identity уже проверены. Следом заменить hard-coded typed recipes ds-картами и подключить реальный
   render/flow broker вместо fake render; CG-2 остаётся проектной надстройкой.
2. ✅ **Первый generic resolution kernel** — `libs/resolve`: bounded MT journal, semantic id/order,
   target groups, host-paced frontier, retaliation-lineage и minimum-HP commit guard. Cardgame resolver уже
   перенесён на work/frontier/damage-route/retaliation. Grouped card pipeline уже имеет pointer-free
   effect/instance refs и scenario tests; следом расширить typed envelope healing/attribute work и добавлять реальный ECS stage adapter/stress на
   тысячи consequences, не перенося project ordering/elements/effects policy.
3. **CG-5** (ресурс + предпросмотр как параллельное S+1) — прямая ближайшая цель дизайна: сравнить ману vs
   две инициативы на одном наборе 30–40 карт.
4. **CG-13 + CG-4** (переиспользуемый UI-list слева + инструментарий выделения/навигации + прокидка
   состояния FSM в UI) и **CG-9** (минимальная схема + базовые валидации) — чтобы 3–4 стихии, 6–8 реакций и
   2 противника с таймерами были играбельны и читаемы.
5. **CG-6/CG-7/CG-8** — по мере появления карт, требующих реакций/поля боя/предпросмотра S+1.
6. **CG-10/CG-11/CG-12/CG-14** — на «Этапе 2» (вертикальный срез забега): гекс-сцена, resume, профиль,
   headless-баланс с AI игрока.

Ключевые проверки перед фиксацией API: пригоден ли `visage` для длинного вертикального списка с
наведением/targeting/reorder и общего элемента-списка (CG-13); хватает ли `act`+ds для цепочек с целями/
S+1-preview/28 реакциями/строгим порядком; как сериализовать бой в середине хода без derived-UI (CG-11);
тянет ли `demiurg` сотни описаний карт без раздувания ресурсов (CG-9). Движковые дельты сведены к: большой
gameplay-FSM + ход/действие (CG-1), generic interaction resolver, UI-инструментарий (CG-4/CG-13),
tooltip+глубина в painter (CG-7), шаблонная гекс-сцена (CG-10); конкретный боевой stage order и карточная
очередь остаются проектными. Полный контент-объём
(212–352 карты, 16 героев, 3 этажа) — цель, а НЕ размер первой версии; критично не писать уникальный
C++/скрипт на каждую карту.

---

## Ссылки
- Память: `engine-appshell-and-settings-direction`, `engine-usage-model`, `gameplay-function-registry`,
  `serialization-strategy`, `determinism-replication-architecture`, `mood-fsm-design`, `demiurg-lua-api`,
  `ai-scheduler-model`, `exec-context-ecs-bridge-plan`, `entity-interaction-model`.
- `AGENTS.md` — секции про `libs/act`, `aesthetics::serial`, «Window management, UI control & app-state FSM».
