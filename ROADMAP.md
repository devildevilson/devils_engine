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

**Критерий готовности:** в tile_frontier `simulation.cpp` (и вся проводка потоков/окна/реестров,
~3 из 4 тыс. строк core/) уходит в devils_engine, проекту остаётся `actor_simulation.cpp`-подобный
файл регистраций + конфиги. Сейчас соотношение boilerplate:специфика ≈ 3:1 — целевая модель его
инвертирует.

**Актуальная оценка на 2026-07-13 (три слоя):**
- реестры по концернам (act/acumen/mood/aesthetics-serial/demiurg/catalogue) — каркасы готовы и
  обкатаны; у catalogue остаётся большой replay/binary-format слой;
- config/data-first вертикальный срез СОБРАН: FSM и GOAP загружаются из tavl через demiurg, GOAP-метрики
  содержат inline-ds выражения, `libs/prefab` умеет data/list/callback/reference/custom + наследование,
  tile_frontier создаёт actor и food из `prefab/*.tavl`, а generic stats-accessors уже вынесены в
  `libs/act`. Следующий пробел здесь — не ещё один формат, а перенос system pipeline; app-shell уже
  обобщён в `libs/simul`;
- devils_script подключён: `act::script_function<bool/number/void>` работает, есть per-worker ds-context,
  `act::call_context` переносит именованные in/out args и списки между native и ds через reusable
  per-worker `execution_scratch`; есть конфиговый GOAP-срез и натив `spawn_at`. Не закончены:
  string/object/vector-маршалинг, effects→catalogue и
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
  args/lists bridge готовы и покрыты native+ds тестом; FSM/GOAP читаются из ресурсов. Следующий срез —
  убрать двойную регистрацию и перевести guards/effects/cost formulas на общий путь;
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
| Replay/netcode/save-лог | `libs/catalogue` | каркас каналов рабочий |

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
   update размечен вложенными catalogue fn-traits. Остаются общий service-channel/графики UI и перенос
   остальных подсистем на тот же паттерн.

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

8. **B-е — состояния/флаги энтити + expiration** — **[необходимо]** *(спроектировано, не построено)*.
   Компонент флагов/модификаторов; флаги с доп-данными хранить `(date, hash)` сортированно. Требует
   системы времени (п.7). Ложится на act + aesthetics.

9. **A-1 — UI-состояние в save** — **[QoL]**.
   Durable = состояние `ui_rng` (4×uint64) + `ui_timestamp` в main + lua-upvalues. Нужен сериализатор
   «простой lua-таблицы»: без функций, резолв вложенных + защита от циклов, `rng_state` писать как число.
   Ложится на слоистую sink-схему (`dump_<side>` рядом с `dump_world`).

10. **Настройки: водораздел ЖИВЫЕ vs РЕСТАРТ** (A-4) — **[необходимо]**.
    Параметрические (размер окна, качество) — вживую; топологические (какие потоки есть) — рестарт.
    Ресайз уже вживую → обобщить на остальные параметры.

---

## 🔴 Крупные (архитектурная генерализация)

11. **B-в — декларативный пайплайн систем** — **[необходимо]**. Список систем + действия над
    компонентами как упорядоченные фазы над view/query. Сейчас think→apply + intent-буфер +
    cognition-scheduler ПРОТОТИПИРОВАНЫ в tile_frontier; поднять в либу.
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

### Ближайшее продолжение: довести перенос общей части из `tile_frontier`

App-shell закрывает топологию приложения, но не весь перенос общей main/gameplay orchestration.
Вернуться к следующему пятишаговому плану:

1. **Поднять общий system pipeline в `libs/simul`** — вынести think → intent → apply,
   cognition scheduling, фазовые барьеры и запуск фаз над ECS view/query. Проект регистрирует фазы и
   queries, но не реализует scheduler заново. Это основной implementation slice п.11.
   **Начато 2026-07-16:** commit-фаза (apply) поднята — `simul::commit_calls` (interaction_commit.h);
   simul получил PUBLIC-deps `act`+`aesthetics` ⇒ дальнейшие переносы пайплайна (think/cognition/барьеры)
   уже дешевле (deps на месте). Остаётся think/cognition scheduling + фазовые барьеры.
2. **Разделить engine lifecycle и project gameplay в `tile_frontier::simulation`** — общий main host
   владеет boot → loading → game, startup/runtime-state resources, main-frame и runtime settings;
   проект подключается через callbacks наподобие `register_project_bindings`,
   `begin_project_loading`, `update_gameplay`, `publish_project_frame`.
3. **Закончить границу engine events ↔ project bindings** — общие Lua API окна, ресурсов, состояния
   приложения, логирования и загрузки регистрируются движком; в проекте остаются только gameplay API,
   actor stats и конкретная presentation/sound policy.
4. **Поднять scheduler времени и пауз над системами** — общий runtime решает, какие gameplay и
   presentation фазы запускать при текущих `app_state` и pause masks; компоненты и project systems не
   размазывают эти ворота по своим update-функциям.
5. **Дочистить стандартную resource/loading orchestration** — общий слой управляет resource-set
   transitions, запросами startup-наборов и базовой readiness; состав сцены, чанки и дополнительные
   project-условия загрузки остаются в `tile_frontier`.

После этих пяти пунктов повторно оценить критерий готовности: `simulation.cpp` должен стать тонким
набором project callbacks/регистраций, тогда как tile map, actor gameplay, prefab policy и
tile/actor render draws намеренно остаются проектными.

13. **B-ж — формализация контрактов** — **[необходимо]**. События/триггеры (на `intent`/catalogue
    INPUT), связи/отношения энтити (`exec_context.scope[8]`; референсная целостность в serial уже есть),
    экспонирование в UI-lua + loc-ключи (seam visage уже есть; loc = `string`-функции act).

14. **Завершить интеграцию devils_script ↔ act** — **[необходимо]** · **центральный оставшийся столб**.
    `script_function`, per-worker VM, GOAP/FSM-конфиги и mutable `call_context` args/out/lists уже
    работают. Осталось: единая регистрация building blocks в `ds::system` при сохранении `act` как
    фасада источников → string/object/vector policy → effect_sink/catalogue → formulas/describe.
    `effect_sink.h` + `exec_context.sink` уже есть (интерфейс-заглушка); его первый реальный потребитель
    и acceptance-тест — меж-энтити взаимодействия (п.16, deferred-call через catalogue).

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
    ✅ **МЕХАНИЗМ ПОСТРОЕН (in-memory, 2026-07-16):** `elect_buffer`+`interaction_arena` (aesthetics reduce)
    + `catalogue::call_log` (generic контейнер «сложить вызов → проиграть позже») + `act::interaction`
    дескриптор (`reg_interaction`/`interaction_of`). tile_frontier eat: decide_actor=record+claim по
    дескриптору, apply=call_log.replay с generic-гейтом (arena.won), effect_eat=чистая мутация scope[1];
    спец-каст `h_eat` УБРАН. Дет-развилка: catalogue generic (ниже act, act ECS-агностичен), reduce→
    aesthetics, композиция в слайсе. build rc=0, 150/150 тестов, resume 120т bit-identical, eat идентично.
    Остаётся: ✅ `collect` примитив готов (вписать в arbitration когда появится потребитель);
    ✅ generic commit-loop поднят в simul (`simul::commit_calls`, interaction_commit.h — replay+дескриптор-гейт+
    invoke; проекту 3 хука skip/make_ctx/after; simul обрёл deps act+aesthetics); byte-serial (netcode/replay)
    ортогонально.
    - ✅ **act call-packer + fat entity-handle + least-privilege + разнородные сигнатуры (СДЕЛАНО
      2026-07-16):** `act::pack<&fn>()` (packer.h) адаптирует обычную C++-функцию в `act::function<Ret>`,
      биндя аргументы по ТИПУ: `entity_handle{world*,entity_id}`←scope, `rng_source`←ctx (RNG/тик),
      `const exec_context&`←ctx (спец-случай), plain←call_context. Все 12 brain-функций tile_frontier
      переведены (эффекты+предикаты без exec_context); exec_context-путь сохранён. Модель блокировок
      (goal 2/3): НЕ в упаковщике, а pipeline-шаг (interaction_arena по дескриптору); ключ контенции =
      (interaction_id, target-entityid) — fn_id выбирает контест, entityid = слот; self-claim по
      claimant-entityid. Out-of-scope (goal 5): внешние-к-ECS системы через handle локер не арбитрирует.
      Тест act_packer_test (5), build rc=0, 155/155, resume bit-identical, eat идентично. Документировано
      в libs/act/README.md. Follow-up: вернуть describe в pack (desc-arg). Исходный набросок ниже:
    - **(исходный дизайн-набросок 2026-07-16):**
      обобщить ручную проводку взаимодействий в СИГНАТУРО-УПРАВЛЯЕМЫЙ упаковщик (прецедент TMP —
      `catalogue::fn_traits`). Эффект пишется обычной функцией от fat-handle'ов
      (`struct {world*, entityid_t}` — упаковка на ВРЕМЯ вызова, НЕ в лог: указатель непереносим),
      напр. `effect_eat(entity_handle self, entity_handle prey)`. Упаковщик по числу entity-аргументов
      (2/3/4) выводит арность взаимодействия и сам: (record) достаёт участников из контекста + ставит
      барьеры (self-claim + `elect.claim` на target-аргументы — «сколько entityid забирает функция,
      столько барьеров»); (commit) реконструирует handle'ы (id из `call_log` + текущий world) и
      вызывает. `exec_context` НЕ исчезает — становится ПРИВАТНЫМ субстратом упаковщика; эффект в
      сигнатуре объявляет только нужное (least-privilege: не видит rng/sink/scratch, если не просил —
      те инжектятся по сигнатуре). Хранение в `call_log` = `fn_id` + entity-id (данные, сериализуемо).
      Сюда же — **поддержка разнородных сигнатур** геймплейных функций (первые N аргументов = entityid,
      дальше — скаляры/handle), чтобы не ограничивать эффекты фикс-формой `{primary,target}`. Скриптовые
      эффекты по-прежнему через act-фасад (упаковщик абстрагирует источник native↔ds). Мотив: снять
      разбросанную по decide_actor/apply обвязку и сузить поверхность, которую эффект видит.
    *Дизайн согласован 2026-07-15.* Зависит от reduce-примитивов п.11 (`elect`/`collect`) и
    шва effect_sink→catalogue п.14; это ПЕРВЫЙ потребитель и acceptance-тест `effect_sink`. Полная
    осмысленная модель — память `entity-interaction-model`. Драйвер: `effect_eat` (actor_simulation.cpp)
    смешивает eligibility + контенцию + cross-entity CRUD в одной нативке.
    - **Ключевая идея:** вызов эффекта-взаимодействия РАСЩЕПЛЯЕТСЯ ВО ВРЕМЕНИ (ровно «отложенное
      выполнение» из `catalogue/core.h` п.7). GOAP/FSM не исполняет `eat` сразу, а ЗАПИСЫВАЕТ вызов
      `(fn_id, self, prey)` в catalogue; тело откладывается.
    - **record-time (MT think, lock-free):** catalogue получает эффект + его ЯВНЫЕ entity-аргументы
      (`self=scope[0]`, `prey=scope[1]`) и по interaction-дескриптору эффекта гоняет reduce-хук:
      `elect` (atomic-min `prey_slot[prey]=self`) + self-claim (`intent[self]=true`). Аргументы обязаны
      быть ЯВНЫМИ (сейчас `prey` выкапывается из `actor_perception` внутри тела ⇒ арбитраж его не видит).
      Запись вызова = детерминизм/replay (INPUT-канал).
    - **commit-time (deferred, однопоточно, полная картина зависимостей кадра):** catalogue проигрывает
      тела ТОЛЬКО победителей. Тело `effect_eat` усыхает до ЧИСТОЙ МУТАЦИИ (grant `actor_eating`/
      `actor_grabbed`, freeze) — БЕЗ guard'ов/контенции. Reduce'ы на record-time уже дали БЕСКОНФЛИКТНОЕ
      множество победителей (elect = один на цель, self-claim + «intent бьёт grab» = каскад/симметрия
      сняты) ⇒ commit безусловен, re-check'и не нужны.
    - **Раскладка концернов:** (A) eligibility = предикат на think-time (снапшот); (B) контенция = reduce
      на record-time; (C) мутация = тело на commit-time.
    - **Строить:** (1) interaction-дескриптор рядом с эффектом (recipe: какой reduce, на каком scope-слоте,
      commit-тело); (2) reduce-хук в record-пути catalogue (`elect`/`collect`/self-claim); (3) deferred-commit
      проход (replay тел победителей); (4) рефактор `effect_eat` на явные scope-аргументы + тело=мутация.
      `rpc_function` уже ловит `Args...`/`write()` — новое = reduce-хук + перенос `call()` в commit-фазу.
    - **Примитивы (в `libs/aesthetics`, п.11):** ✅ `elect_buffer` (atomic min/max, Cap 1) + ✅ `collect_buffer`
      (СДЕЛАН 2026-07-16): ПЛОСКИЙ конкурентный буфер на тип (atomic fetch_add, НЕ per-entity), детерминизм
      отдельным `sort(cmp)` + `for_each_group` скользящим окном; переполнение = ГРОМКАЯ ОШИБКА (не дроп).
      Тест aesthetics_collect_buffer_test (5). Потребителя (урон/атака) в tile_frontier пока нет ⇒ в arena
      не вписан (cumulative-интеракции = type-erased collect per fn + `arbitration::cumulative` — future).
      `join` (взаимное согласие) = та же 2-фазная схема + `choice[]` reciprocity, НЕ новый примитив; пока не нужен.

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
остаются необходимыми; app-shell (п.12) закрыт.

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
- **Стабильность формата и версионирование** — `catalogue` (binary format), `demiurg` (migration
  metadata), `aesthetics` (migration схем), `act` (сериализация `intent`/payload). Нужно для
  save/replay/netcode. Связано с [[determinism-replication-architecture]].
- **Профилирование и бюджеты** — `catalogue` perf-tracing (п.4 выше, приоритет РАНО), `sound`
  (`system2::update`), `visage` (Nuklear/Lua), `painter`. Один инструмент закрывает несколько либ.
  `demiurg` уже подключён к `catalogue` доменом `demiurg`.
- **Перенос из tile_frontier в либы (library-first)** — ✅ `simul` broker/topology/lifecycle и
  app-shell окна/настроек закрыты в п.12; следующий перенос — system pipeline п.11.
- **Очистка legacy** — ✅ painter (`painter_base`/старый `execution_pass` и image containers) и
  utils (`actor_ref`/`context_stack`/неиспользуемые allocator-прототипы) перенесены в `exclude/`
  2026-07-13; в `sound` туда же ушла заброшенная `virtual_source`/`basic_sources` иерархия, но
  смешанный OpenAL `sound::system` остаётся. Далее: старые dispatcher-подходы и две линии `catalogue` API.
- **Пробелы в тестах** — focused-тесты уже есть у `catalogue` introspection, `mood`, `sound::system2`,
  `flow` и `simul`; не покрыты их более старые/широкие контракты. Отдельные пробелы остаются у `input`
  и `visage`, плюс нужны replay/RPC тесты `catalogue` и расширение `utils`.
- **Детерминизм на уровне сборки** — `options` (fp-модель/fast-math/FMA как отдельные interface targets).
- **Отключаемость подсистем + модель потоков** — `simul` (optional render/sound/assets), `sound.enabled`
  (п.2), `painter` (отдельная transfer queue; убрать device-wide waitIdle).

### `libs/act` — реестр gameplay-функций
- 🟡 (н) **native↔ds фасад и полный call contract** — `script_function<bool/number/void>` и отдельный
  reusable `execution_scratch` с mutable args/out/lists уже работают; осталось провести единый registration
  path и определить string/object/vector-маршалинг. `libs/act` скрывает источник функции, а не дублирует ds runtime.
- 🟡 (н) **Lua backend policy** — Lua остаётся UI/guest; если pure gameplay-вызовы ему понадобятся,
  экспонировать их через act-фасад, но не делать Lua mutating backend симуляции.
- 🔴 (н) **effect_sink → catalogue** — сейчас в act только интерфейс; реальная реализация переедет в replay-слой.
- 🟡 (н) **формат конфигов регистрации функций** + загрузка функций из модулей/ресурсов.
- 🟡 (н) **стабильный формат сериализации `intent`/payload** — для лог/replay/network границ.
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

### `libs/catalogue` — replay/лог вызовов
- 🔴 (н) **финализировать binary format** — tick headers, payload sizes, checksum, versioning, endian, alignment.
- 🔴 (н) **replay executor** — пройти headers → reader из registry → вызов функций.
- 🟡 (н) **выбрать одну линию API** (старый `core.h` vs новый набор), убрать дублирование; стабилизировать `rpc_function`.
- 🟡 (н) **demo на диск** (`write_to_disk`/`load_from_disk` = `false`); связать `act::value` ↔ payload; dry-run sinks + policy областей мутации.
- 🟢 (н) **решить где живёт effect_sink** (act/catalogue/replay); какие данные писать под какие задачи (не смешивать каналы).
- 🔴 (н) **deferred-call взаимодействий (п.16)** — catalogue расщепляет вызов эффекта во времени: record-time (захват entity-аргументов + lock-free reduce-хук elect/collect/self-claim) → barrier → commit-time (replay тел победителей). Это форма «отложенного выполнения» (core.h п.7); первый потребитель effect_sink.
- 🟡 (qol) **больше consumer'ов** (сейчас только demo-прототип); 🟢 (qol) мелочи (include guard `demo.h`).
- 🟡 (н) **тесты** (registry/channel/demo/rpc).

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
- ✅ **demiurg↔Lua resource API** — 4 глобала в UI-sandbox (host-side в tile_frontier, не в demiurg):
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
- 🟡 (н) закончить границу движковых событий и project gameplay bindings; общий ECS system pipeline — п.11.
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
