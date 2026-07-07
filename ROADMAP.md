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

**Оценка на 2026-07-06 (три слоя):**
- реестры по концернам (act/acumen/mood/aesthetics-serial/demiurg/catalogue) — ~готовы, обкатаны боем;
- конфиг-описания поверх реестров (FSM/GOAP/префабы из tavl) — ~30–40%: потребители уже декларативны
  (mood ест строки переходов, acumen — именованные метрики/действия), но описания живут в C++,
  лоадеров из tavl нет, префабы не начаты. Дизайн-вопросов нет — только объём;
- devils_script не подключён (script/lua-бэкенды act — заглушки) + app-shell не извлечён (п.12).
  Остаток работы практически полностью завязан на devils_script + разгребание tile_frontier по части
  акторов симуляции.

**Решение по devils_script ↔ act (2026-07-06).** devils_script умеет сильно больше, чем наспех
накиданный exec_context: автовывод сигнатур (`register_function<&fn>`), типизированная навигация по
скоупам, итераторы (`register_function_iter` = each_neighbor), списки с filter/map/sum,
script-in-script, `container::describe` (интроспекция для UI), split container/script_container,
детерминированный PRNG per-context (`context.prng_state` — дисциплина `mix(seed,entity,tick,purpose)`
ложится напрямую), кастомные арифметические типы (задел под fixed-point). Отсюда:
- **`ds::system` = ЕДИНСТВЕННАЯ точка регистрации нативных функций** (скрипты обязаны их видеть;
  второй параллельный реестр нативок в act — ошибка, которую разматываем);
- **act усыхает до тонкого слоя**: таблица «имя → (категория, script_container* | нативный вызов)» +
  категорийные проверки на загрузке («guard обязан быть предикатом») + кэширование для acumen/mood.
  Что переживает: резолв-по-имени в acumen/mood (не меняется), `intent`/`effect_sink` (архитектура
  детерминизма, не скриптовый концерн). Что заменяет ds: value, native_function-обёртки, describe,
  большая часть exec_context;
- **GOAP из конфига = три поля**: состояния (predicate-скрипты), как получить состояние, реакция на
  выбранное решение (effect-скрипты) + приоритеты/стоимости числовыми скриптами (формулы уезжают из
  C++). tavl — парсер и конфигов, и ds → скрипт в конфиге = просто ещё одно поле документа;
- **порядок интеграции:** (1) подключить ds + реализовать `script_function` поверх
  script_container/context; (2) вертикальный срез: ОДИН GOAP-предикат tile_frontier из tavl-конфига
  (demiurg-ресурс → ds::parse → acumen) — вскроет все швы; (3) консолидация регистрации нативок в
  ds::system, усыхание act — ПОСЛЕ среза, когда виден реальный интерфейс; (4) FSM-guards и весь GOAP
  из конфига конвейером;
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
- **B-д.** Большой компонент характеристик — определён сразу в devils_engine, а в проекте заполняется
  конкретикой (список чисел + обёртка-название + что считать положительным значением + ещё пара вещей).
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
| Геймплейные функции (предикат/значение/объект/эффект) | `libs/act` — `act::registry` | каркас готов (native done; script/lua — заглушки) |
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

- **B-а** → `act::registry` (типы effect/predicate/number/string/object, `native_function`,
  `exec_context`, `intent`, `describe`). Проект регистрирует native-функции.
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

1. **`rng_state + int` — прокрутить состояние N раз вперёд** (A-2) — **[QoL]**.
   `sol::overload` на meta-`+`: `(rng,rng)`=микс (есть), `(rng,int)`=advance N. Нюанс: сейчас `prng64` —
   splitmix hash-step, «N вперёд» = N применений (O(N)); для больших N перейти на splitmix-counter
   (`state += gamma·N`). Задокументировать семантику `+int`=advance / `+rng`=mix.

2. **`sound.enabled` + динамический `worker_threads_reserved`** (A-4, топология) — **[QoL]**.
   Зеркало `render.enabled`: пропустить создание `sound_simulation` + его поток. `reserved` считать от
   числа ВКЛЮЧЁННЫХ движковых потоков, не статиком. Топологические настройки → рестарт движка.

3. **Признать B-а / B-б явным контрактом** (документация, 0 кода) — **[необходимо]**.
   Зафиксировать в AGENTS/README: «список функций» = регистрация в `act::registry`, «список
   компонентов» = `SERIALIZABLE_COMPONENT` + пометка replicated/derived. Разблокирует остальное.

---

## 🟡 Средние

4. **catalogue perf-tracing** (A-5, B-г) — **[необходимо]** · **приоритет: РАНО**.
   *Автор: заняться пораньше — надо научиться профилировать код и следить за бюджетами.*
   RAII-таймер ПО УРОВНЯМ (вложенные скоупы) → SERVICE-канал catalogue → графики в UI (метрики уже в
   env). Ложится на существующий `utils::perf` + consumer-модель. Это же и B-г «доп-шаги симуляций».
   **Статус 2026-07-06:** базовый доменный logging/trace слой уже работает; `demiurg` подключён как
   отдельный домен (`flow` = ресурсные переходы, `trace` = вход/выход + время крупных функций).
   Остаются общий service-channel/графики UI и перенос остальных подсистем на тот же паттерн.

5. **B-ё — prefab / спавн энтити** — **[необходимо]**. Первый содержательный слайс (связывает
   act + компоненты + tavl). Модель (уточнена автором):
   - **C++ задаёт каркас** (какие компоненты) + значения по умолчанию, которые каркас получает БЕЗ
     явного указания.
   - **tavl-конфиг задаёт конкретный тип префаба и значения**; префаб может наследовать другой
     (огр = «большой гоблин» с переопределением локальных полей).
   - Конкретный тип префаба (с возможностью переопределить некоторые локальные поля) ТАКЖЕ уходит
     функцией `spawnX`/`createX` в базовые функции (B-а/`act`) — чтобы из скрипта создать в точке
     несколько противников.

6. **B-д — generic-компонент ХАРАКТЕРИСТИК** — **[необходимо]**.
   Определить в devils_engine, проект заполняет: список чисел + обёртка-название + «что считать
   положительным» (для UI-tooltip окраски) + пара полей. Родственно dense-реестру через `string_pool`.

7. **Время/календарь + отложенные эффекты** (B-ж) — **[необходимо]**. Уточнено автором — ДВЕ
   несмешиваемые системы времени:
   - **Игровой КАЛЕНДАРЬ** — игровые ДАТЫ; произвольное число месяцев и дней в месяце (отдаётся на откуп
     внешнему проекту); игровое календарное время НЕ совпадает с реальным.
   - **Игровое ВРЕМЯ** — микросекунды (+ реальные дата/время) — для «действует 60 секунд».
   На это вешаем отложенные эффекты / expiry (драйвер для B-е).
   **ОТКРЫТЫЙ ВОПРОС:** как не смешать — «60 секунд» это игровых или реальных? Описание эффекта должно
   явно указывать систему отсчёта; продумать разделение.

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

12. **A-3 / A-4 — окно + настройки → общий devils_engine (app-shell слой)** — **[необходимо]**.
    Пользователь не переопределяет всю проводку main↔окно↔render. Переиспользуемое ядро: `window_policy`
    + аккумулятор событий + контракт resize→только-свопчейн + гейты draw/mute + `app_state` FSM.
    Library-first: стабилизировать в tile_frontier → вынести.

13. **B-ж — формализация контрактов** — **[необходимо]**. События/триггеры (на `intent`/catalogue
    INPUT), связи/отношения энтити (`exec_context.scope[8]`; референсная целостность в serial уже есть),
    экспонирование в UI-lua + loc-ключи (seam visage уже есть; loc = `string`-функции act).

14. **Интеграция devils_script + усыхание act до адаптера** — **[необходимо]** · **центральный
    оставшийся столб** (см. «Решение по devils_script ↔ act» в разделе «Цель проекта»). Порядок там же:
    script_function → вертикальный GOAP-срез из tavl → единая регистрация нативок в `ds::system` →
    FSM/GOAP из конфига конвейером. Разблокирует пункты B-а(each_*)/6-скрипты, забирает формулы
    стоимости acumen и describe-тултипы UI.

---

## Предлагаемый порядок первых шагов
🟢 1–3 (разминка + разблокировка контракта) → 🟡 **4 (perf-tracing, приоритет РАНО — профилирование и
бюджеты)** → 🟡 **5 (prefab/data-first)** как первый «мясной» слайс → далее 6/7/8 (характеристики →
время → флаги, в этом порядке из-за зависимости) → 🔴 генерализация (11, 12) когда паттерны устоятся.

*Поправка 2026-07-06:* остаток работы почти целиком завязан на **14 (devils_script)** + разгребание
tile_frontier по акторам. Вертикальный ds-срез (один GOAP-предикат из конфига) конкурирует с prefab
за роль первого «мясного» слайса — оба вскрывают швы act+компоненты+tavl с разных сторон; ds-срез
при этом снимает главный архитектурный риск (форма script_function/контекста) раньше.

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
- **Перенос из tile_frontier в либы (library-first)** — `simul` (broker/topology/lifecycle) = п.12;
  app-shell окна/настроек = п.12.
- **Очистка legacy** — `sound` (OpenAL), `painter` (`painter_base`/`execution_pass`), `utils`
  (`actor_ref`/dispatcher/`context_stack`), `visage` (`draw_resource`/`draw_stage`), `catalogue`
  (две линии API).
- **Отсутствие тестов** — `catalogue`, `mood`, `input`, `sound`, `flow`, `simul`, `visage`, расширить в
  `utils`. Точечно и относительно дёшево, но важно для core-либ (сериализация/детерминизм).
- **Детерминизм на уровне сборки** — `options` (fp-модель/fast-math/FMA как отдельные interface targets).
- **Отключаемость подсистем + модель потоков** — `simul` (optional render/sound/assets), `sound.enabled`
  (п.2), `painter` (отдельная transfer queue; убрать device-wide waitIdle).

### `libs/act` — реестр gameplay-функций
- 🔴 (н) **script/lua backend** — `script_function`/`lua_function` сейчас `utils::error`-заглушки; реализовать поверх `devils_script`.
- 🔴 (н) **effect_sink → catalogue** — сейчас в act только интерфейс; реальная реализация переедет в replay-слой.
- 🟡 (н) **формат конфигов регистрации функций** + загрузка функций из модулей/ресурсов.
- 🟡 (н) **стабильный формат сериализации `intent`/payload** — для лог/replay/network границ.
- 🟡 (н) **number unit/tag** — нужен ли числам тег единицы (деньги/дистанция/проценты) — открытый вопрос.
- 🟢 (н) **роль Lua** — решить: UI/guest vs ограниченный доступ к pure-функциям (не mutating backend).
- 🟡 (qol) **describe для script backend** — доделать (UI-tooltip'ы уже заготовлены).
- 🟢 (qol) **реестр: различать повтор имени vs hash-collision** (сейчас один assert на оба).

### `libs/acumen` — GOAP
- 🔴 (н) **expression layer** для predicate/effect (`has_effect(fear) && enemy_nearby`) — мини-язык выражений.
- 🟡 (н) **metrics/goals/actions через конфиги**; богатый контракт исполнения действия.
- 🟡 (qol) **встроенный выбор цели** (сейчас на вызывающем) и **бюджет перевычисления metrics**.
- 🟡 (qol) **глобальная/persistent политика кеша** (сейчас ручные per-thread + merge).
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
- 🟡 (н) **FSM-описания из C++ строк → resource/config**.
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
- 🟡 (qol) device-local draw groups + GPU indirect; форматы (mips/arrays/MSAA/RT roles); уборка legacy (`painter_base`/`execution_pass`); GLSL-компиляция на render thread как fallback.

### `libs/simul` — каркас симуляций/акторов
- 🔴 (н) **перенести broker/topology из tile_frontier** (= п.12) + модель синхронизации advancer (lock-free каналы вместо mutex-на-тик).
- 🟡 (н) **базовый lifecycle actor-подсистем**; разделить движковые события и проектные расширения (= A-3); **механизм optional-симуляций** (= A-4, первый шаг — `sound.enabled` п.2).
- 🟡 (qol) бюджеты каналов + runtime-рост; судьба advancer как владельца timing loop vs отдельный scheduler.
- 🟢 (qol) stop token / jthread-friendly API.
- 🟡 (н) **тесты** (lifecycle/stop/optional/delivery).

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
- 🟡 (qol) разделить публичный vs experimental API (`actor_ref`/dispatcher/`context_stack` — аудит перед использованием/удаление после миграции).

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
  `ai-scheduler-model`, `exec-context-ecs-bridge-plan`.
- `AGENTS.md` — секции про `libs/act`, `aesthetics::serial`, «Window management, UI control & app-state FSM».
