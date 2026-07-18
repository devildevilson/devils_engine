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

**Актуальная оценка на 2026-07-18 (три слоя):**
- реестры по концернам (act/acumen/mood/aesthetics-serial/demiurg/catalogue) — каркасы готовы и
  обкатаны; catalogue MT-декораторы collect/elect и живой tile_frontier commit pipeline
  готовы; config-loaded void actions и 128-byte inline payload тоже закрыты. Остались общий trace
  service/UI, старый effect_sink seam и расширение bounded codecs.
  Replay/binary-format не входят в ответственность catalogue;
- config/data-first вертикальный срез СОБРАН: FSM и GOAP загружаются из tavl через demiurg, GOAP-метрики
  содержат inline-ds выражения, `libs/prefab` умеет data/list/callback/reference/custom + наследование,
  tile_frontier создаёт actor и food из `prefab/*.tavl`, а generic stats-accessors уже вынесены в
  `libs/act`. App-shell и минимальный system runner обобщены; project gameplay свёрнут за
  `tile_frontier_game`, не перенося map/actor/draw/sound policy в engine;
- devils_script подключён: `act::script_function<bool/number/void>` работает, есть per-worker ds-context,
  `act::call_context` переносит именованные in/out args и списки между native и ds через reusable
  per-worker `execution_scratch`; есть конфиговый GOAP-срез и натив `spawn_at`. Не закончены:
  string/object/vector-маршалинг и
  окончательная консолидация регистрации нативок. Lua остаётся UI/guest backend, не mutating backend.

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
- ✅ **контекст разделён на две части (2026-07-13)**: immutable execution context (world/scopes, deterministic RNG
  inputs, tick, effect sink) и отдельный mutable набор аргументов/результатов вызова.
  `act::call_context` поддерживает до стандартных 8 ds input/out/in-out values inline, result и списки;
  caller владеет `execution_scratch{ds::context,call_context}` и переиспользует capacity на worker-е.
  Подсистемы встраивают этот блок в свой scratch (`acumen::execution_scratch` добавляет A*+cache), а не
  используют глобальный/TLS-раздатчик. `script_function` bind/collect-ит ds args/lists,
  native получает тот же объект. Работают bool/integer/number/entity-id основы; string/object policy и
  `vec3` (>16-byte ds stack slot при double) остаются частью fixed-point/marshalling pass;
- `intent`/`effect_sink` переживают эту переделку: это граница детерминированной мутации, а не деталь
  конкретного script backend. `describe` должен проходить через тот же фасад независимо от источника;
- **GOAP из конфига = три поля**: состояния (predicate-скрипты), как получить состояние, реакция на
  выбранное решение (effect-скрипты) + приоритеты/стоимости числовыми скриптами (формулы уезжают из
  C++). tavl — парсер и конфигов, и ds → скрипт в конфиге = просто ещё одно поле документа;
- **статус интеграции:** ds подключён, `script_function`, вертикальный GOAP-предикат и mutable
  args/lists bridge готовы и покрыты native+ds тестом; FSM/GOAP читаются из ресурсов. Production
  fallback и параллельная native-таблица predicate/flee/chase/think удалены. Следующий срез — единый
  registration facade, DS RNG/target scopes и перевод guards/effects/cost formulas/describe на общий путь;
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
| Геймплейные функции (предикат/значение/объект/эффект) | `libs/act` — `act::registry` | native + ds bool/number/void работают; args/lists/string/object/effect-sink pending |
| Компоненты + сериализация | `aesthetics::serial` — `SERIALIZABLE_COMPONENT` | готов, с тестами |
| GOAP | `libs/acumen` | готов, потребляет `act` |
| FSM состояний энтити | `libs/mood` | готов, потребляет `act` |
| Ресурсы/моды | `libs/demiurg` | готов; type-from-path + discovery/manifest override + tavl list pattern + stable `resource_handle` + lua `request`/`require`/`find`/`filter` |
| Trace/log + deferred MT-dispatch gameplay-вызовов | `libs/catalogue` | introspection + collect/elect wrappers/executors + inline journal + config-loaded effects готовы; service/UI/effect_sink pending |
| Replay/netcode/save-лог | отдельный будущий слой | не проектировать через catalogue автоматически |

**Итог модели:** «проект» = **ЗАПОЛНИТЬ реестры** (C++ native: `reg`/`register_type`/
`SERIALIZABLE_COMPONENT`) + **ОПИСАТЬ конфиг-деревья** (data-сторона, tavl). «Инструментарий движка» =
эти registration-API + формат tavl + seam'ы `exec_context`/`intent`. Изобретать новое почти не нужно —
закрыть несколько пробелов и генерализовать app-shell.

---

## Что УЖЕ готово (не переделывать, только «признать контрактом»)

- **B-а** → `act::registry` (типы effect/predicate/number/string/object, native/ds implementations,
  execution context, `intent`, `describe`). Проект регистрирует building blocks в ds и публичные
  gameplay-контракты через act-фасад; точная единая registration API — ближайшая работа п.14.
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
   - Низовой script-примитив `spawn_at(prefab,x,y)` готов и покрыт тестом. Выбор места, spawner-entity,
     filter/pick и живые trigger/event scripts — отдельный отложенный дизайн (п.15); автор повторно
     подтвердил 2026-07-13, что ECS-spawners и DS-композицию пока не начинаем.

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
    **⚠️ Согласованный дизайн (2026-07-13, сделать в ближайшем будущем):** целевая форма —
    `process query_t<> → message_buffer → process query_t<> → …`. Примитивы в **`libs/aesthetics`**
    (не simul; simul = только порядок фаз + гейтинг): оживить `template_system_mt` из `exclude/` как
    map-примитив (`query_t`→`process`, `distribute1/compute/wait`) + `message_buffer<Msg>` indexed by
    (message_type, entityid) (плотный массив по entity-index ⇒ lock-free непересекающиеся записи,
    детерминированный обход без sort; intent = один тип сообщения) + отдельный select/work-list шаг для
    бюджетируемых систем (cognition = reduce-select → map-think → message → map-apply). Текущий
    `simul::cognition_scheduler` — ПРОВИЗОРНЫЙ (уже переведён на distribute1/compute/wait), растворится в
    этой модели. Детали: `docs/simul-extraction-design.md` (шаг 1, блок «отложенный редизайн»).

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
   - ⏳ Остаток п.14: cost formulas/`describe` через тот же facade; string/object/vector marshalling по
     фактическим сигнатурам; судьба `effect_sink` seam (адаптировать к catalogue deferred или удалить).

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
   Остаток: velocity/doppler не кормим; per-type громкости (`set_source_volume` — заглушка);
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

**Не ближайшие:** spawner entities + spatial query, ds `filter/pick/spawn`,
multi-participant catalogue `reserve/write-set`, runtime dependency graph. Для них сначала нужна
отдельная дизайн-сессия и реальный gameplay-кейс; текущий single-key collect/elect
не цементировать в общий API.

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

14. **Завершить интеграцию devils_script ↔ act** — **[необходимо]** · **центральный оставшийся столб**.
    `script_function`, per-worker VM, GOAP/FSM-конфиги и mutable `call_context` args/out/lists уже
    работают; production config fallback и отдельная native predicate/action таблица удалены.
    Осталось: единая registration API для building blocks в `ds::system` при сохранении `act` как
    фасада источников → DS RNG и дополнительные entity scopes → нужная string/object/vector policy →
    formulas/describe. `effect_sink.h` + `exec_context.sink` остаются интерфейсом-заглушкой; п.16 уже
    провёл меж-энтити вызовы через catalogue БЕЗ sink, поэтому теперь нужно явно адаптировать либо
    удалить этот seam, а не строить вторую deferred-модель рядом.
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

15. **Спавн-архитектура + per-entity brain** — **[необходимо, но НЕ следующий implementation slice]** ·
    сначала отдельная дизайн-сессия; тех-долг зафиксирован 2026-07-12
    (примитивы готовы: prefab_registry в `libs/prefab`, food+actor из конфига `prefab/*.tavl`, ds-натив
    `spawn_at(prefab, x, y)` над spawn_scope). Три отложенных куска:
    - **per-entity systems** — GOAP/FSM сейчас slice-level, но общие независимые `acumen::registry` и
      `mood::registry` уже готовы. Не вводить общий `brain_ref`: FSM может жить без GOAP, GOAP — без FSM
      (например мозг отряда). При необходимости prefab получает независимые optional `goap_ref`/`fsm_ref`;
      FSM обычно фиксирована типом сущности и runtime-switch ей пока не нужен. Совместимость GOAP,
      runtime-switch профиля и per-profile caches — отдельная long-running задача.
    - **спавнеры-энтити + запрос** — реальное «где» спавна = спавнер-энтити (статичные designer-точки ИЛИ
      динамические правила), их много, пространственный запрос (переиспользовать `kd_tree` как перцепция) +
      фильтр по предикату (теги/cooldown/вместимость). `spawn_at(точка)` остаётся примитивом; слой
      выбора/placement резолвит намерение → точку. Детерминизм: спавн = эффект catalogue на sim-rng+состоянии;
      «вне камеры» — presentation (в MP sim не смотрит в локальную камеру — реплицируемый регион/все игроки).
    - **ds `spawn`/`filter`/`pick`** — эргономичная композиция в скрипте
      (`spawn(horse, pick(filter(spawners, mount, off_camera)))`); триггер = per-entity reference-список
      спавнеров + on-enter ds-скрипт; динамический спавнер = источник точки — ds/натив-функция.
    Перед реализацией решить границы ownership/lifetime brain pools, формат ссылок prefab→brain,
    порядок catalogue effects при массовом spawn и контракт replicated-region для off-camera. До этого
    не цементировать API вокруг текущего slice-level мозга или локальной камеры.

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
    ✅ Загружаемый `act::script_function<void>` для GOAP action готов: optional `effect = <ds>` co-parse'ится
    из tavl, semantic action name получает script backend в act, а ds building blocks — те же
    `fn_deferred_ptr`. Штатные `flee/chase/think` переведены; `tile_frontier_config_effect_smoke` грузит
    shipped `goap/actor` и проверяет 1-vs-4 bit identity. ✅ (2026-07-18) DS RNG для
    `seek_food/wander` (`set_course = chance`) и target scope для `eat` (`eat = prey`) сделаны —
    все 6 действий config-only, регистрация через `act::building_blocks`. Ближайшие шаги:
    (1) решить границу со старым `effect_sink`; (2) member/custom bounded codecs по реальным
    сигнатурам; (3) ownership helper только после второго потребителя; (4) `join` только при
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
- **Стабильность формата и версионирование** — `demiurg` (migration metadata), `aesthetics`
  (migration схем), `act` (типизированные gameplay payload). Save/replay/netcode при появлении получают
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
  Config-loaded void-script acceptance и inline journal stress закрыты; дальше нужны effect_sink/
  bounded-codec acceptance и расширение `utils`.
- **Детерминизм на уровне сборки** — `options` (fp-модель/fast-math/FMA как отдельные interface targets).
- **Отключаемость подсистем + модель потоков** — `simul` (optional render/sound/assets), `sound.enabled`
  (п.2), `painter` (отдельная transfer queue; убрать device-wide waitIdle).

### `libs/act` — реестр gameplay-функций
- 🟡 (н) **native↔ds фасад и полный call contract** — `script_function<bool/number/void>` и отдельный
  reusable `execution_scratch` с mutable args/out/lists уже работают; осталось провести единый registration
  path и определить string/object/vector-маршалинг. `libs/act` скрывает источник функции, а не дублирует ds runtime.
- 🟡 (н) **Lua backend policy** — Lua остаётся UI/guest; если pure gameplay-вызовы ему понадобятся,
  экспонировать их через act-фасад, но не делать Lua mutating backend симуляции.
- 🔴 (н) **effect_sink ↔ catalogue strategy executor** — определить, остаётся ли старый sink общим
  act-сеамом или полностью заменяется typed deferred wrapper для effect-функций; replay сюда не примешивать.
- 🟡 (н) **формат конфигов регистрации функций** + загрузка функций из модулей/ресурсов.
- 🟡 (н) **типизированные границы `intent`/payload** — для act/ds marshalling; долговременная
  сериализация replay/network принадлежит отдельному слою.
- 🟡 (н) **number unit/tag** — нужен ли числам тег единицы (деньги/дистанция/проценты) — открытый вопрос.
- 🟡 (qol) **describe для script backend** — доделать (UI-tooltip'ы уже заготовлены).
- ✅ (qol) **реестр различает повтор имени и hash-collision**.

### `libs/acumen` — GOAP
- ✅ **expression layer для predicates** — используется devils_script, отдельный мини-язык не нужен;
  GOAP-метрики из tavl уже содержат inline-ds выражения.
- 🟡 (н) **довести metrics/goals/actions через конфиги** — структура и predicates готовы; добавить
  script costs/priorities/effects и богатый контракт исполнения действия через обновлённый act call.
- 🟡 (qol) **встроенный выбор цели** (сейчас на вызывающем) и **бюджет перевычисления metrics**.
- 🟡 (qol) **глобальная/persistent политика кеша** (сейчас ручные per-thread + merge).
- ✅ **registry + config inheritance foundation** — registry владеет стабильными immutable systems;
  tile_frontier GOAP resources поддерживают single-base overlay/disable и flatten. Runtime hierarchy нет.
- 🔴 (н, long-running) **compatibility + runtime profile switch** — required components/capabilities,
  prefab validation, deterministic tick-boundary switch и раздельные cache/action-index domains.
- 🟡 (qol) **динамический размер состояния** (сейчас фикс. `bitset<256>`).

### `libs/aesthetics` — ECS
- 🔴 (н) **строгая политика structural changes под MT** (`world` не полностью thread-safe) + синхронизация событий.
- 🟡 (н) **авто-пересборка query после загрузки снапшота** — structural blocker: перенести `snapshot_loaded_event` в `common.h`, добавить receiver + общий `rebuild()`.
- 🟡 (н) **частичные дампы/дельты/selective components** — для save/network workflows.
- 🟡 (qol) **оптимизация random/forward deletion** (sidecar dense-owner array) — если станет узким местом.
- 🟡 (qol) **migration/versioning снапшот-схем** (сейчас mismatch → reject).
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
- 🟢 (н) **effect_sink boundary** — решить совместимость старого `act::effect_sink` с typed executor,
  не смешивая MT-dispatch, gameplay `on_effect` и будущую долговременную запись.
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
- 🔴 (н) **полноценный asset manager** — строит полный registry проекта, кеширует/переиспользует.
- 🟡 (н) **фоновая загрузка (thread pool + per-tick budget)** — сейчас loader однопоточный (API уже готов).
- 🟡 (qol) migration/versioning metadata; cancellation/priority pending-запросов; policy для
  автоматической выгрузки warm-only CPU ресурсов после использования.
- ✅ (qol) диагностика циклов dependency graph.
- 🟢 (qol) строгий контракт zip до parse; `append_resources` при коллизии id (сейчас skip+warn,
  новый manifest path уже не создаёт пропускаемые коллизии).

### `libs/flow` — анимации *(почти greenfield: `system` пустой, только `state_t`)*
- ✅ **CMake target + базовые тесты** — `devils_flow` оформлен, `flow_test` покрывает parsing/playback/sampling.
- ✅ **формат анимаций + demiurg list pattern** — `animation_resource` читает `.tavl` subresources
  через `list_section`, поддерживает `id:name`/`id:index`, real-line diagnostics идут через offset.
- 🟡 (н) **модель времени** (tick/render/clip); **2D data model**; ECS-компоненты состояния анимации.
- 🟡 (н) **mapping mood state → flow animation**; interpolation/blending/crossfade; callbacks через `act`.
- 🟢 (н) **разграничить callbacks по потокам** (что можно на render thread, что через intents).
- 🟡 (qol) **2.5D directional sprites** (DOOM-style) — зависит от типа игры.
- 🔴 (qol) **3D skeletal clips**; **GPGPU bone matrices** (batched skinning) — крупное, на будущее.
- 🟡 (н) **расширить тесты** (loops/callbacks/thread-boundary/ECS-компоненты).

### `libs/input` — ввод/окно (GLFW)
- 🟡 (н) **control schemes/contexts** (character/vehicle/menu) + стек/приоритет активных contexts.
- 🟡 (н) **bindings из config** (`events::init` сейчас хардкод).
- 🟢 (н) **mouse buttons/wheel как first-class** в `events` (сейчас только через внешние callbacks).
- 🟡 (qol) геймпады/джойстики; API rebinding (конфликты + UI feedback); event queue / frame snapshot.
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
- 🔴 (н) **queue model** — поддержать отдельную transfer queue family + ownership transfers + fallback при одной очереди.
- 🟡 (н) **убрать `vkQueueWaitIdle`/`DeviceWaitIdle` из lifecycle** → только frame-fence (`wait_all_fences`). Совпадает с фидбеком про granularity.
- 🟢 (н) **`clear_color`/`clear_depth`** сейчас заглушки; дореализовать/проверить остальные draw/dispatch/transfer команды (🟡).
- ✅ **render-config через demiurg tavl list pattern** — категории render-config могут быть одним
  list-файлом с subresources; связи резолвятся по `name`, diagnostics получают label/line-offset,
  текст config-resource сбрасывается после parse без перевода ресурса из warm.
- 🟡 (н) **стабилизировать `.tavl` data model** (схемы/валидация/ошибки/примеры); строгая валидация render graph; контракт UI-buffers/texture-array/shader-prep + тесты.
- 🟡 (н) **модель освобождения GPU-ассетов** (`unload_hot` сбрасывает index, но слот через render API не освобождается).
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
- 🟡 (н) **`set_source_volume` + группы громкости** (master/music/sfx/ui/dialogue); 🟢 (н) профилировать `system2::update`.
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
- 🟡 (н) **error policy** (visage не должен ронять движок; локальный «стоп скрипта с точным местом»); **реальные бюджеты Lua** (время/инструкции/GC).
- 🟡 (н) **read-only API к игровому состоянию/demiurg** — demiurg-сторона ЗАКРЫТА (`request`/`require`/
  `find`/`filter` глобалы отдают `resource_handle`, lua читает `usable()`/`state()`, но не грузит/выгружает);
  остаётся read-only доступ к игровому состоянию (ECS/характеристики).
- 🟢 (н) **профилировать/бюджетировать Nuklear** (Lua update, `nk_convert`, буферы).
- 🟢 (qol) `endf` → `fin`; убрать старый `draw_resource`/`draw_stage`.
- 🟡 (qol) closable vars (Lua 5.4); runtime reload UI; система числовых units (DPI/percent/relative + restore при resize); debug overlay как системный инструмент; убрать глобальный `nk_context*` из bindings (multi-context).
- 🔴 (qol) UI над юнитами (world-space).

---

## Ссылки
- Память: `engine-appshell-and-settings-direction`, `engine-usage-model`, `gameplay-function-registry`,
  `serialization-strategy`, `determinism-replication-architecture`, `mood-fsm-design`, `demiurg-lua-api`,
  `ai-scheduler-model`, `exec-context-ecs-bridge-plan`, `entity-interaction-model`.
- `AGENTS.md` — секции про `libs/act`, `aesthetics::serial`, «Window management, UI control & app-state FSM».
