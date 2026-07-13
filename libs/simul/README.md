# libs/simul

`libs/simul` - общий app/runtime слой для крупных движковых симуляций, которые крутят вычисления в отдельных циклах и часто в отдельных потоках. Помимо `interface`/`advancer`, библиотека уже владеет стандартным broker-контрактом, lifecycle, расширяемой optional worker topology, window/settings/loading/render/sound/assets runtime helpers; проект добавляет локальные каналы и gameplay-фазы.

Целевое назначение библиотеки - собрать в одном месте правила многопоточного взаимодействия больших подсистем: main/gameplay, assets, render, sound и будущие проектные симуляции.

## Текущая Модель

Базовый интерфейс состоит из трех методов:

- `init()` - подготовить внутреннее состояние симуляции;
- `update(time)` - выполнить один тик;
- `stop_predicate()` - сообщить, что цикл можно остановить.

`simul::advancer` добавляет fixed timestep loop:

- хранит целевое время кадра;
- считает номер тика;
- вызывает `update(_frame_time)` в цикле;
- спит до следующего расчетного времени;
- позволяет менять frame time через `set_frame_time()`;
- останавливается через `stop()`, `_stop` или `stop_predicate()`;
- отдает простую статистику: `counter()`, `start()`, `compute_fps()`.

`advancer::run(wait_mcs)` можно запускать в `std::jthread` или другом внешнем потоке. Перед стартом он делает небольшой initial offset через spin wait, чтобы несколько симуляций не стартовали строго в один и тот же момент.

Сейчас `update()` выполняется под внутренним mutex `advancer`. Это защищает текущие поля `advancer`, но не является полноценной моделью синхронизации между подсистемами.

## Как Это Используется Сейчас

В `tile_frontier` от `simul::advancer` наследуются:

- main `simulation` - владеет жизненным циклом приложения, ресурсными реестрами, окном, UI, thread pool и дочерними симуляциями;
- `assets_simulation` - строит demiurg registry, обслуживает загрузку ресурсов и пересылает GPU-переходы render-потоку;
- `render_simulation` - владеет Vulkan/render graph runtime и обрабатывает render-команды;
- `sound_simulation` - владеет звуковой системой и обслуживает sound-команды.

Main создает единый `broker` и подсистемы, передает каждой `broker*` через `set_broker()`, затем вызывает `init()` и только после этого запускает потоки. Это важный контракт: каналы доступны уже во время стандартной инициализации и гарантированно готовы до первого `update()` фоновой симуляции.

Окно трактуется как поздний platform resource. Render-поток может существовать до появления окна, а событие создания/пересоздания окна приходит через broker. Этот принцип должен остаться общим: внешние проекты не должны переопределять базовое взаимодействие движка с GLFW/window lifecycle, а должны расширять поведение поверх готового контракта.

## Lifecycle Приложения

Lifecycle разделен на два уровня.

`app_runtime` задает синхронные границы запуска: `bootstrap_ready`, `systems_created`,
`systems_initialized`, `workers_started`, `main_loop`, `workers_stopped`. Traits определяют только
bootstrap/broker/main типы и собирают `worker_systems<Broker>`; стандартный render/assets/sound набор
строит `make_standard_workers()`, после чего проект при необходимости добавляет свои workers. Runtime
сам передает broker до `init()`, регистрирует системы по concrete type, запускает `jthread` и join-ит
их по явному `shutdown_order`. Main получает optional систему через `runtime_system<T>()`, поэтому
ручного `bind_systems` и нового runtime-слота для каждой подсистемы больше нет.
Стандартный topology callback также пересчитывает размер общего task pool по фактическому числу
workers, включая project-specific системы, а не только по render/sound/assets флагам.

`lifecycle_controller` задает асинхронные фазы main-loop: начальный
`boot -> loading -> game` и последующие переходы `game -> loading -> game`. Хост реализует:

- `on_lifecycle_enter(phase)` — одноразовая работа при входе;
- `on_lifecycle_tick(phase, time)` — работа текущей фазы каждый main-тик;
- `lifecycle_phase_complete(phase)` — единственное условие перехода;
- `on_lifecycle_leave(phase)` — одноразовая работа перед переходом.

Контроллер один владеет текущей фазой и выполняет переход в порядке `leave(old)`, смена фазы,
`enter(next)`. Переход из game запрашивается через `request_loading()`: контроллер применяет его на
следующем lifecycle tick, а loading снова завершается обычным `lifecycle_phase_complete()`.

В текущем переходном варианте `tile_frontier` boot читает дисковую startup entry и готовит resource
set начального runtime-state до первой external/GPU-ступени. При входе в loading создается окно,
набор состояния доводится до `final_state()`, запрашиваются проектные текстуры/звуки/чанки и
создается actor world. Первый UI script запускается только после готовности всего UI-набора. Переход
в game происходит после готовности остальных стартовых ресурсов и чанков; gameplay update до этого
не идет. Последующие runtime-переходы оставляют старый UI как loading screen до готовности нового.

Текущий дисковый startup-срез использует два стандартных типа ресурсов:

- активный `startup/entry` задает logical id начального `state`;
- `states/*` задает entry `script`, единый список `resources` и optional `scene`.

Первый модуль в module list имеет наивысший приоритет, поэтому мод может заменить
`startup/entry` или выбранный runtime-state, если стоит перед `core`. Все ресурсы из
`state.resources` запрашиваются до `final_state()` до первого исполнения UI script. Этот же
список является allowlist: Lua может получать handles, исполнять `require` и перечислять только
разрешенные ресурсы, но не может запускать load/unload. `require` принимает только уже usable
script resource. Ресурсы активной scene добавляются в UI scope сразу и могут быть еще не готовы;
скрипт видит их `state()`/`usable()` для отображения прогресса и диагностики.

`tile_frontier` хранит `current` и `target` runtime-state. Lua может запросить переход через
`app.request_state(id)`, после чего локальные чанки и actors пересоздаются для новой generation;
запоздавшие ответы старой generation отбрасываются. Долгоживущие engine-системы и их глобальное
состояние в переходе не пересоздаются. Политика выгрузки более не нужных ресурсов пока не оформлена:
старые ресурсы могут оставаться resident после смены состояния.

Дисковый boot является переходным вариантом. Целевой boot bundle (splash, подготовленный font и
минимальный UI) будет задаваться compile-time Traits и встраиваться в бинарник; модульные registry,
`startup/entry` и project UI остаются частью loading.

## Сообщения Между Симуляциями

Обобщенной системы сообщений в `libs/simul` пока нет. В `tile_frontier` она реализована локально как `core::broker`:

- `thread::mailbox<T>` для latest-wins снапшотов и one-shot состояний;
- `thread::spsc_queue<T>` для FIFO-команд;
- `thread::payload_channel<Msg>` через `write_buffer_channel` для POD-сообщения плюс byte payload.

Каждый канал сейчас строго SPSC: один producer и один consumer. Бюджеты каналов фиксированы в конструкторе broker и не растут в runtime.

Это хороший кандидат на перенос в `libs/simul`: сама библиотека должна описать типы каналов, правила владения, lifecycle broker'а, политику переполнения и типовые топологии `main -> render`, `main -> assets`, `assets -> render`, `render -> assets`, `main -> sound`, `sound -> main`.

## Планируемый Контракт Actor Pattern

`libs/simul` должен стать местом, где описан базовый контракт больших actor-like подсистем:

- кто владеет объектом симуляции;
- кто создает поток;
- когда вызывается `init()`;
- когда и как передаются каналы сообщений;
- как останавливаются и join'ятся потоки;
- какие события являются движковыми и не переопределяются проектом;
- где проект может добавить свои сообщения и поведение;
- как отключаются отдельные симуляции в конфиге.

Целевой дизайн: движок предоставляет стандартные actors для render/assets/sound/platform, а внешний проект может добавить локальную симуляцию или расширить набор сообщений, не переписывая базовый lifecycle.

## Отключаемые Подсистемы

Движок должен уметь работать без отдельных симуляций:

- без графики;
- без окна/headless;
- без звука;
- потенциально без ассет-потока для простых тестов или offline tools.

В `tile_frontier` уже есть часть этого поведения: render можно отключить через конфиг, а headless render не требует GLFW/window surface. Звук пока создается как обычная подсистема, и общий механизм отключения звуковой симуляции еще нужно оформить.

Любой worker является optional по факту: фабрика не добавляет его в `worker_systems`, объект и поток
не создаются, а `runtime_system<T>()` возвращает `nullptr`. Стандартная фабрика применяет это правило
к render/sound/assets через boot config. Конкретный проект отдельно решает, может ли его gameplay
работать без отсутствующей системы; например, обычный boot `tile_frontier` требует assets.

## Что Сейчас Может

На текущем срезе `libs/simul` умеет:

- задать минимальный интерфейс обновляемой симуляции;
- запускать fixed timestep loop через `advancer::run()`;
- менять frame time безопасно через mutex;
- считать tick counter и примерный FPS;
- останавливать цикл через `stop()`;
- использовать один и тот же базовый класс для main, render, assets и sound симуляций в `tile_frontier`.

Библиотека уже содержит общий broker, стандартные сообщения/системы,
`app_runtime<Traits>` с владением `std::jthread`, optional render/sound/assets topology,
shutdown/join policy и общие window/settings/loading helpers. Пока не содержит:

- общего scheduler-а фаз над ECS view/query;
- runtime-роста каналов и универсальной политики backpressure;
- полностью независимого standalone CMake-контракта для всех header-only runtime
  зависимостей вне корневого aggregate target.

## Техдолг И Направления

- ✅ Общий broker/message topology вынесен; проект наследует `standard_broker` и добавляет локальные каналы.
- ✅ Базовый lifecycle create→wire→init→threads→stop/join и late window events вынесен в `app_runtime`/runtime helpers.
- ✅ Window/GLFW lifecycle, live resize/fullscreen, loading и settings helpers вынесены; проектными остаются bindings/gameplay policy.
- ✅ Optional simulations: render/sound/assets и project workers отсутствуют без создания объекта и потока; main проверяет их через typed registry.
- Уточнить модель синхронизации `advancer`: сейчас `update()` выполняется под mutex, но межсимуляционное общение должно идти через lock-free channels/snapshots, а не через общий lock на тик.
- ✅ `stop_token` / `std::jthread`-friendly API используется `app_runtime`.
- Формализовать бюджеты каналов и политику переполнения: latest-wins, reliable FIFO, lossy FIFO, payload arena.
- Базовые lifecycle/stop/optional тесты есть; расширить delivery/backpressure и partial-init/shutdown сценарии.
- Решить, остается ли `advancer` владельцем timing loop или нужен отдельный scheduler, который управляет несколькими симуляциями единообразно.

## Design Note 2026-07-08: Runtime Bootstrap And Settings

Текущий перенос `tile_frontier` уже сдвинул общий actor lifecycle в сторону `libs/simul`:

- `simul::app_runtime<Traits>` владеет broker'ом, main и расширяемым `worker_systems<Broker>` с worker `std::jthread`;
- `simul::standard_broker`, `simul::messages`, `simul::write_buffer_channel` и базовые `main_system/render_system/assets_system/sound_system` вынесены в `libs/simul`;
- `tile_frontier::runtime_traits` наследует стандартный bootstrap/settings policy и одной фабрикой выбирает стандартный состав workers;
- `tile_frontier::runtime_bootstrap` уже выделен как внешний контекст для общего startup-состояния;
- `tile_frontier::simulation` больше не создает worker-системы и не держит голые `graphics_actor*`/`sound_actor*`/`assets_actor*`; вместо этого используется `system_presence`.

Открытый вопрос: как правильно разделить boot config, runtime settings и project/game config.

### Наблюдение

Один "общий конфиг приложения" быстро становится неподходящей границей:

- часть значений нужна только один раз при boot;
- часть значений пользователь может менять и сохранять;
- часть значений принадлежит конкретному проекту/игре;
- часть значений требует пересоздать систему, а часть применяется сразу.

Поэтому различие между конфигами не должно быть только в форме структуры. Главное различие - **policy применения**:

- boot-time only;
- saveable/reloadable user setting;
- runtime override;
- project default;
- requires restart;
- requires subsystem recreate;
- immediate/next-frame apply.

### Предварительная Модель

Вместо одного `engine_config` стоит думать о слоях:

- `engine_boot_config` - редкие движковые настройки, нужные до поднятия систем:
  - resource roots / engine module;
  - config resource id;
  - cache roots;
  - enabled topology for render/sound/assets;
  - thread topology defaults;
  - logging bootstrap defaults;
  - CLI overrides.
- `runtime_settings` - пользовательские/game settings:
  - window mode/resolution/fullscreen;
  - graphics profile/vsync/active graph;
  - sound device/master volume;
  - input/keymapping;
  - metrics/log verbosity that can be changed at runtime.
- `project_config` - конкретные настройки игры/подпроекта:
  - startup resources;
  - UI entry point;
  - world/gameplay constants;
  - project-specific render additions.

`runtime_bootstrap` может хранить несколько частей:

```cpp
struct runtime_bootstrap {
  simul::engine_boot_config engine;
  simul::settings_registry settings;

  demiurg::module_system engine_modules;
  demiurg::resource_system engine_resources;
  demiurg::module_system cache_modules;
  thread::atomic_pool pool;
};
```

### Settings Registry Direction

Обсуждаемый вариант - registry typed settings blocks:

```cpp
settings.add<graphics_s>("graphics");
settings.add<input_s>("keymapping");
```

Системы должны иметь возможность читать базовый контракт:

```cpp
settings.get<graphics_base_s>();
```

При этом внешний проект может расширить настройки:

```cpp
struct tile_graphics_s : public simul::graphics_base_s {
  std::string graph;
  std::string menu_graph;
  uint32_t demo_graph_toggle_ms = 0;
};
```

Полезные metadata для каждого блока:

```cpp
enum class apply_mode {
  immediate,
  next_frame,
  recreate_system,
  restart_required
};

struct settings_meta {
  std::string section;
  bool saveable = true;
  apply_mode apply = apply_mode::immediate;
};
```

Layering может быть таким:

```text
defaults <- project file <- user settings file <- CLI overrides
```

Первый практичный шаг на будущее:

1. Вынести в `libs/simul` минимальный `engine_boot_config`.
2. Добавить `settings_registry` с typed blocks и metadata на уровне блока.
3. Оставить field-level diff на потом.
4. Переносить `sound_system` почти целиком в `libs/simul`, затем `assets_system`, затем общий базовый render слой.

Важно: настройки окна, keymapping, graphics profile и sound device скорее относятся не к "engine config", а к runtime/game settings, потому что они должны уметь дампиться, перегружаться и применять изменения через стандартные сообщения системам.
