# devils_engine

`devils_engine` - экспериментальный C++23 игровой движок / фреймворк. Это не
законченный продукт и не аккуратно отполированная библиотека, а большая рабочая
площадка, где рядом проверяются разные идеи: ECS, ресурсы, Vulkan render graph,
Lua/Nuklear UI, звук, AI-планирование, FSM, межпоточные каналы и общий lifecycle
крупных подсистем.

Этот README сейчас описывает только `libs/`. Папку `tests/`, особенно
`tests/tile_frontier`, стоит разобрать отдельным документом позже.

Цель файла - дать общую картину для человека: что за подпроект перед тобой,
зачем он нужен, насколько он живой, с чем связан и как им пользоваться на уровне
идеи. Технические детали, gotchas и память для будущих агентов лежат в
`AGENTS.md`.

## Общая Картина

Библиотеки в `libs/` можно мысленно разделить на несколько слоев.

Нижний слой:

- `options` - общие настройки сборки.
- `utils` - низкоуровневые инструменты: память, потоки, строки, файлы, хеши,
  PRNG, spatial-структуры.

Gameplay / simulation:

- `act` - общий контракт gameplay-функций.
- `acumen` - GOAP-планировщик.
- `mood` - маленькая FSM-система.
- `aesthetics` - ECS и снапшоты мира.
- `simul` - базовый update-loop для крупных симуляций.
- `flow` - пока только набросок будущей системы анимаций.
- `catalogue` - прототип записи/replay/RPC/dry-run вокруг вызовов функций.

Ресурсы и платформенные подсистемы:

- `demiurg` - модульная система ресурсов и staged loading.
- `input` - GLFW окно, ввод, key names, Vulkan surface helpers.
- `sound` - звук, сейчас основной путь через miniaudio.

Presentation:

- `painter` - Vulkan render graph и GPU-ассеты.
- `bindings` - Lua/sol2 bindings и Nuklear API.
- `visage` - Lua/Nuklear UI runtime, шрифты и UI draw output.

Корневой CMake собирает большинство этих библиотек и дает общий target
`devils_engine::devils_plane`.

`libs/flow` - исключение: он пока не подключен как CMake target.

## libs/options

`options` - это не C++ библиотека, а CMake-слой. Он нужен, чтобы не размазывать
одни и те же флаги компиляции по всем подпроектам.

Через `devils_engine::options` распространяются:

- требование C++23;
- общие warning/optimization flags;
- отключенный RTTI;
- AVX/архитектурные флаги;
- LTO для optimized-конфигураций;
- compile definitions с именем проекта и движка;
- include paths для нескольких header-only зависимостей.

Как об этом думать: это общий build contract. Если библиотека является частью
движка, она обычно линкует `devils_engine::options` и получает одинаковую
политику сборки.

## libs/utils

`utils` - фундаментальная библиотека с инструментами общего назначения. Она
сильно разрослась, потому что сюда попадали полезные вещи из разных экспериментов
движка.

Что в ней есть:

- диагностика, logging, assert/error helpers;
- работа с файлами, строками, временем, путями проекта;
- хеши, string ids, type traits;
- PRNG и dice helpers;
- аллокаторы и memory pools;
- thread pools и lock-free/SPSC primitives;
- `mailbox`, `spsc_queue`, `byte_ring`, `payload_channel`;
- UTF helpers;
- compression;
- spatial-структуры: geometry, grid, kd-tree, AABB tree;
- legacy/experimental вещи вроде старого `actor_ref`.

Самая важная свежая часть для текущей архитектуры - потоковые примитивы:

- `mailbox<T>` - latest-wins snapshot;
- `spsc_queue<T>` - фиксированная FIFO очередь;
- `payload_channel<Msg>` - очередь сообщений + byte arena;
- `byte_ring` - SPSC byte buffer.

Именно на них сейчас построен broker в `tile_frontier`.

Как пользоваться: брать отсюда низкоуровневый инструмент, когда он не должен
знать о конкретной подсистеме. Если код уже становится gameplay/render/sound
специфичным, ему лучше жить выше.

## libs/act

`act` - общий слой gameplay-функций. Его задача - дать единый способ описать,
зарегистрировать и вызвать функцию, которую потом могут использовать GOAP,
FSM, скрипты, UI-preview или будущий replay/dry-run слой.

Главная идея: функция получает `act::exec_context`, а тип возврата определяет
ее категорию:

- `void` - effect;
- `bool` - predicate;
- число - numeric query;
- id/string/object - другие query-категории.

Сейчас реально реализован `native_function`, то есть C++ function pointer.
`script_function` и `lua_function` пока заготовки.

Важные сущности:

- `act::registry` - таблица функций по id;
- `act::exec_context` - immutable контекст вызова;
- `act::effect_sink` - будущий шов для dry-run/log/replay effects;
- `act::intent` - компактный результат thinking-слоя для поздней apply-фазы;
- `act::value` - универсальный payload для границ вроде replay/network/script.

Как об этом думать: `act` не решает, что делает игра. Он задает общий язык, на
котором разные decision-системы могут говорить с gameplay-кодом.

## libs/acumen

`acumen` - GOAP-планировщик. Он не владеет миром и не меняет компоненты. Его
работа - взять текущее состояние актора/мира в виде набора булевых флагов,
найти цепочку символических действий и вернуть план вызывающему коду.

Он связан с `act` так:

- state metrics - это `act` predicates;
- action effect names резолвятся через `act::registry`;
- сам планировщик effect не исполняет.

Что уже есть:

- bitset-состояние;
- masked/scoped state;
- actions с requirements и symbolic next state;
- goals;
- A* поиск;
- caller-owned scratch;
- exact solution cache.

Как пользоваться: gameplay-код считает `act::exec_context`, `acumen` выбирает
действие, а вызывающий слой сам превращает выбранный action в `act::intent` или
другую команду apply-фазы.

Как об этом думать: это мозг для выбора “что хочу сделать”, но не руки, которые
меняют мир.

## libs/mood

`mood` - простая FSM по текстовым строкам. Она нужна для состояния актора:
например idle, wander, flee, eating, attack и переходы между ними.

Переход выглядит примерно так:

```text
current_state + event [guard1, guard2] / action1, action2 = next_state
```

Есть два слоя:

- `mood::system` - хранит распарсенные переходы и быстро отдает candidates по
  `(state, event)`;
- `mood::runtime` - добавляет conventions: `any_state`, `idle`, `on_entry`,
  `on_exit`, top-down guard checking.

Guards и actions берутся из `act::registry`, поэтому `mood` и `acumen` могут
использовать один общий набор gameplay-функций.

Важно: `mood` не хранит состояние конкретного актора. Состояние хранит внешний
код, обычно компонент в ECS или локальная структура simulation layer.

Как об этом думать: `acumen` может выбрать событие/намерение, а `mood` помогает
перевести actor state machine в правильное состояние и вызвать entry/exit/effect
логику.

## libs/flow

`flow` - пока только набросок будущей системы анимаций. Он не собирается как
CMake target и не имеет полноценного runtime API.

Задуманная роль:

- `mood` отвечает за gameplay state;
- `flow` отвечает за то, как этот state выглядит во времени;
- `painter` в итоге получает готовые visual данные.

Например:

- gameplay state: `attack1`;
- flow state: конкретный clip, sprite frame sequence, directional sprite или
  material animation.

В будущем `flow` может заниматься:

- 2D frame animation;
- 2.5D directional sprites;
- skeletal clips;
- blending/crossfade;
- animation callbacks через `act`;
- visual interpolation на render-time.

Как об этом думать сейчас: это зафиксированное направление дизайна, а не готовая
библиотека.

## libs/aesthetics

`aesthetics` - экспериментальная ECS-библиотека.

Она умеет:

- выдавать entity id с index/version схемой;
- хранить компоненты в per-type sparse/dense storage;
- создавать, получать и удалять компоненты;
- обходить компоненты через `view<T...>()`;
- делать union-обход через `lazy_view<T...>()`;
- держать live `query<T...>()`, обновляемые событиями create/remove;
- рассылать typed events;
- запускать простые systems по `update_event`;
- делать бинарный snapshot мира;
- упаковывать snapshot payload в disk/network контейнер.

Главный практический контракт: `world` не является полностью thread-safe миром.
Ожидаемый паттерн такой:

1. собрать стабильные query/input списки;
2. посчитать в worker'ах;
3. сложить результаты в intents/output;
4. детерминированно применить изменения в apply-фазе.

Как об этом думать: это хранилище и обход компонентов, а не весь gameplay
framework. Scheduling, actor thinking и межпоточные каналы должны жить выше.

## libs/simul

`simul` - минимальный слой для симуляций, которые крутятся в update-loop.

Сейчас там фактически два типа:

- `simul::interface`;
- `simul::advancer`.

`interface` задает:

- `init()`;
- `update(time)`;
- `stop_predicate()`.

`advancer` добавляет fixed timestep loop, счетчик тиков, frame time, `run()` и
`stop()`.

В `tile_frontier` от этой формы наследуются main simulation, render simulation,
assets simulation и sound simulation. Но важная деталь: полноценный broker и
actor topology пока живут в `tests/tile_frontier`, а не в `libs/simul`.

Как об этом думать: это пока каркас update-loop. В будущем сюда логично вынести
общий lifecycle больших подсистем: создать broker, init, wire channels, start
threads, stop, join.

## libs/demiurg

`demiurg` - система ресурсов и модулей.

Она решает несколько задач:

- найти файлы в папках или zip/mod архивах;
- понять, какой resource type соответствует файлу;
- создать typed resource object;
- собрать registry ресурсов;
- дать стабильный `resource_interface*`;
- двигать ресурс по staged loading state machine.

Основные понятия:

- module - папка или архив;
- resource - объект, созданный из файла;
- `resource_system` - registry типов и ресурсов;
- `resource_loader` - reconciler загрузки/выгрузки.

Обычная лестница ресурса:

```text
cold -> warm -> hot
```

Для CPU-only ресурсов `warm` и `hot` могут совпадать. Для сложных ресурсов
может быть больше шагов: например font resource идет через чтение TTF,
генерацию MSDF atlas и GPU upload.

Очень важная идея: GPU/render-owned шаги не делает assets loader напрямую. Он
выдает external job, а render thread выполняет GPU transition.

Как пользоваться:

1. зарегистрировать типы ресурсов;
2. распарсить modules;
3. получить `resource_interface*`;
4. попросить `resource_loader` довести ресурс до `final_state()`;
5. external jobs отправлять владельцу внешнего состояния, например renderer.

Как об этом думать: `demiurg` - это не конкретный декодер png/mesh/sound, а
общий механизм discovery, registry и staged lifecycle.

## libs/sound

`sound` - звуковой слой. Сейчас актуальное направление - miniaudio `system2` и
звуки как `demiurg` ресурсы.

В библиотеке еще остался старый OpenAL-путь. Его не стоит считать целевым API,
но удалять его без отдельного решения тоже не нужно.

Текущая модель:

- звук загружается как `sound_resource`;
- `sound_resource` хранит compressed bytes и возвращает `resource2` view;
- sound thread получает play-команду с ресурсом;
- `system2` создает decoder из memory-backed данных;
- PCM стримится в miniaudio voice через ring data source.

Поддерживаемые форматы в текущем направлении:

- mp3;
- wav;
- flac;
- ogg.

`system2` умеет:

- создать audio device;
- выбрать playback device по имени;
- fallback на default device;
- держать mono/stereo voice pools;
- играть positional mono sounds;
- играть non-spatial stereo sounds;
- обновлять listener;
- обновлять позицию/скорость active sound task;
- отдавать snapshot progress для UI;
- менять master volume.

Как об этом думать: sound system не должна владеть ассетами. Ассеты живут через
`demiurg`, а `sound` только воспроизводит уже warm resource view.

## libs/input

`input` - слой окна и ввода поверх GLFW.

Он отвечает за:

- init/terminate GLFW;
- создание и уничтожение окна;
- window/framebuffer callbacks;
- key/char/mouse/scroll/drop callbacks;
- fullscreen/windowed helpers;
- cursor и clipboard;
- Vulkan surface integration;
- имена клавиш;
- abstract input events.

Важная часть - key names:

- canonical names для конфигов (`key_w`, `minus`, `escape`);
- US layout names для отображения;
- local/native names для текущей раскладки/платформы.

Gameplay/UI должны работать не с raw GLFW key, а с abstract actions:

```text
use
quit
toggle_menu
move_left
```

`events` хранит привязки action -> scancode/key slots и считает состояния:
press, click, long press, double click и т.п.

Как об этом думать: `input` изолирует остальной движок от GLFW-деталей и дает
нормальные action ids вместо сырых клавиш.

## libs/painter

`painter` - основной Vulkan/rendering слой.

Он состоит из двух больших идей:

1. Render graph как данные.
2. GPU asset table для ресурсов, которые приходят через `demiurg`.

Render graph описывает:

- ресурсы;
- render targets;
- samplers;
- descriptors;
- materials;
- geometry;
- draw groups;
- steps;
- passes;
- graphs.

`graphics_base` превращает это описание в Vulkan runtime:

- buffers/images;
- descriptor layouts и descriptor sets;
- render passes;
- framebuffers;
- pipelines;
- command buffers;
- semaphores/fences;
- swapchain или no-present режим.

`assets_base` хранит GPU slots для ассетов:

- meshes;
- textures;
- placeholder texture;
- GPU indices, которые потом используют draw code и descriptors.

Через `demiurg` в `painter` уже живут:

- texture resource;
- mesh resource;
- shader source / prepared GLSL;
- render config source;
- pipeline cache resource.

Как об этом думать: `painter` не просто набор Vulkan helpers, а эксперимент с
data-driven render graph. Внешний код должен по возможности писать данные в
объявленные resources/draw groups, а не собирать кадр вручную.

## libs/bindings

`bindings` - слой Lua/sol2 bindings.

У него пока нет собственного README, но по коду он отвечает за:

- создание sandbox Lua environment;
- безопасный набор базовых Lua функций;
- отключение `math.random` в пользу deterministic helpers;
- `base` namespace с PRNG/dice/table/perf helpers;
- `rng_state` usertype;
- reflect-based перенос C++ aggregate/container/map в Lua table и обратно;
- bindings для Nuklear API;
- подключение текущего `nk_context`.

Этот слой тесно связан с `visage`, но по смыслу он ниже: `bindings` дает Lua
инструменты, а `visage` владеет UI runtime.

Важное ограничение: Nuklear context сейчас передается через статический указатель
в binding layer. Для одного UI-контекста это нормально, но несколько независимых
UI-контекстов потребуют переделки.

Как об этом думать: это мост C++ <-> Lua/Nuklear, а не самостоятельная gameplay
система.

## libs/visage

`visage` - UI-система на Lua + Nuklear.

Она владеет:

- Lua state;
- sandbox environment;
- Nuklear context;
- entry-point Lua function;
- шрифтами;
- SDF/MSDF font data;
- output buffers для рендера.

Обычный кадр выглядит так:

1. host передает input snapshot;
2. host обновляет Lua-visible данные или регистрирует `app.*` API;
3. `visage::system::update(time, timestamp, rng_state)` вызывает Lua entry;
4. `convert()` превращает Nuklear commands в vertices/indices/draw commands;
5. render thread рисует это через `painter::draw_ui`.

Шрифты:

- `font_resource` грузит TTF;
- генерирует MSDF/MTSDF atlas;
- через `painter::gpu_texture_resource` заливает atlas в GPU;
- `visage::system` может держать несколько именованных шрифтов.

`visage` не должен напрямую менять gameplay state. Он дает host'у доступ к
`script_state()` и `script_env()`, чтобы приложение само зарегистрировало
контролируемый API: звук, настройки, quit/fullscreen, loading progress и т.п.

Как об этом думать: `visage` - presentation/UI runtime. Он рисует и вызывает
host API, но не должен становиться владельцем gameplay или asset policy.

## libs/catalogue

`catalogue` - ранний прототип слоя записи и воспроизведения вызовов функций.

Это не готовый netcode и не основная система сохранений.

Идея такая:

- есть buffer из headers + byte payload;
- header описывает tick, function id и offset payload;
- channel собирает вызовы;
- consumers могут писать demo, debug log, network packet или audit output;
- registry знает, как по id вызвать reader/invoker.

Возможные будущие роли:

- debug/audit logging;
- replay/demo recording;
- dry-run effect sink;
- компактный RPC helper;
- реализация для `act::effect_sink`.

Сейчас в библиотеке есть несколько пересекающихся прототипов API. Перед активным
использованием ее нужно стабилизировать.

Как об этом думать: это площадка для идеи “записать выбранные function calls в
байты и потом проанализировать или проиграть”, а не готовая инфраструктура.

## Связи Между Подпроектами

Самая важная текущая связка:

```text
act -> acumen
act -> mood
```

`act` дает общий словарь функций, `acumen` выбирает план, `mood` помогает
перевести actor state.

Ресурсная связка:

```text
demiurg -> painter
demiurg -> sound
demiurg -> visage
```

`demiurg` дает resource handles и staged loading. `painter`, `sound` и `visage`
дают конкретные resource types и умеют доводить их до usable состояния.

Presentation связка:

```text
input -> visage -> painter
bindings -> visage
```

`input` собирает окно/ввод, `visage` строит UI output, `painter` рисует.  
`bindings` дает Lua/Nuklear API, которым пользуется `visage`.

Simulation связка:

```text
simul + utils/thread primitives -> tile_frontier broker model
```

`simul` пока дает только update-loop форму, а реальная межпоточная topology
проверяется в `tests/tile_frontier` через broker на `utils/thread`.

## Текущее Состояние

- Самые фундаментальные библиотеки: `options`, `utils`, `act`, `demiurg`.
- Самые активные интеграционные библиотеки: `painter`, `sound`, `visage`,
  `input`, `aesthetics`, `acumen`, `mood`.
- Самые прототипные/незавершенные: `flow`, `catalogue`, часть старого кода в
  `painter`, `sound`, `utils`.
- Главная интеграционная проверка сейчас живет в `tests/tile_frontier`.

Этот файл должен оставаться общей картой. Если нужна точная память для будущих
агентов, gotchas, порядок shutdown, конкретные VUID, CMake edge cases или
текущие решения по `tile_frontier`, это лучше держать в `AGENTS.md`.
