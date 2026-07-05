# devils_engine — план развития (обзор)

Документ-обзор: свести воедино два направления мыслей и разложить по скорости достижения, чтобы
СНАЧАЛА увидеть общую картину, а детальное планирование каждой задачи делать отдельно.

Два направления:
- **A. Движковый слой** — оконный менеджмент, настройки, тумблеры потоков, save/rng, трассировка.
- **B. Контракт внешнего проекта** — что проект определяет в C++, а что описывает в конфигах.

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
| Ресурсы/моды | `libs/demiurg` | готов; type-from-path + import-rules — спроектировано |
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

---

## Предлагаемый порядок первых шагов
🟢 1–3 (разминка + разблокировка контракта) → 🟡 **4 (perf-tracing, приоритет РАНО — профилирование и
бюджеты)** → 🟡 **5 (prefab/data-first)** как первый «мясной» слайс → далее 6/7/8 (характеристики →
время → флаги, в этом порядке из-за зависимости) → 🔴 генерализация (11, 12) когда паттерны устоятся.

---

## Тех-долг по подсистемам `libs/`

Собрано из README каждого подпроекта. `libs/bindings` намеренно без README (сейчас сильно завязан на
visage). Обозначения строк: **усилие** 🟢/🟡/🔴 (как выше) + **необходимость** `(н)` = необходимо /
`(qol)` = quality of life.

### Сквозные темы (видны сразу в нескольких либах)
- **Описание через конфиги/ресурсы вместо C++** — `act` (формат регистрации функций), `acumen`
  (metrics/goals/actions), `mood` (FSM из строк→config), `demiurg` (list pattern `path:name`), `flow`
  (формат анимаций), `painter` (`.tavl` data model), `input` (bindings из config). Это ядро модели
  «проект = конфиги + заполнение реестров».
- **Стабильность формата и версионирование** — `catalogue` (binary format), `demiurg` (migration
  metadata), `aesthetics` (migration схем), `act` (сериализация `intent`/payload). Нужно для
  save/replay/netcode. Связано с [[determinism-replication-architecture]].
- **Профилирование и бюджеты** — `catalogue` perf-tracing (п.4 выше, приоритет РАНО), `sound`
  (`system2::update`), `visage` (Nuklear/Lua), `painter`. Один инструмент закрывает несколько либ.
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
- 🔴 (н) **полноценный asset manager** — строит полный registry проекта, кеширует/переиспользует.
- 🟡 (н) **финализировать list pattern `path:name`**; строгая политика override/priorities модулей.
- 🟡 (н) **stable handles поверх `resource_interface*`** — если указатель переживает rebuild registry.
- 🟡 (н) **фоновая загрузка (thread pool + per-tick budget)** — сейчас loader однопоточный (API уже готов).
- 🟡 (qol) migration/versioning metadata; cancellation/priority pending-запросов.
- 🟢 (qol) диагностика циклов dependency graph; строгий контракт zip до parse; `append_resources` при коллизии id (сейчас skip+warn).

### `libs/flow` — анимации *(почти greenfield: `system` пустой, только `state_t`)*
- 🟢 (н) **оформить CMake target** (сейчас нет build target).
- 🟡 (н) **модель времени** (tick/render/clip); **2D data model**; формат анимаций + demiurg; ECS-компоненты состояния анимации.
- 🟡 (н) **mapping mood state → flow animation**; interpolation/blending/crossfade; callbacks через `act`.
- 🟢 (н) **разграничить callbacks по потокам** (что можно на render thread, что через intents).
- 🟡 (qol) **2.5D directional sprites** (DOOM-style) — зависит от типа игры.
- 🔴 (qol) **3D skeletal clips**; **GPGPU bone matrices** (batched skinning) — крупное, на будущее.
- 🟡 (н) **тесты** (stepping/loops/callbacks/sampling).

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
- 🟡 (н) **стабилизировать `.tavl` data model** (схемы/валидация/ошибки/примеры); строгая валидация render graph; контракт UI-buffers/texture-array/shader-prep + тесты.
- 🟡 (н) **модель освобождения GPU-ассетов** (`unload_hot` сбрасывает index, но слот через render API не освобождается).
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
- 🟡 (н) **полноценный image для Nuklear** (demiurg-ресурс/GPU slot/UV/размеры/lifetime/Lua-handle) — сейчас заглушки.
- 🟡 (н) **error policy** (visage не должен ронять движок; локальный «стоп скрипта с точным местом»); **реальные бюджеты Lua** (время/инструкции/GC).
- 🟡 (н) **read-only API к игровому состоянию/demiurg** (UI читает, но не грузит/выгружает).
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
