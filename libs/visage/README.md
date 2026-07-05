# libs/visage

`libs/visage` - UI-система движка на базе Nuklear. Идея подпроекта: описывать интерфейс через Lua-скрипты, выполнять их в отдельной UI-песочнице и отдавать результат в рендер как простой набор вершин, индексов и draw-команд.

Lua-биндинги к Nuklear живут в `libs/bindings`. Отдельного README для `bindings` пока нет: для текущего дизайна это вспомогательный слой, который экспортирует безопасное Lua-окружение, базовые utility-функции и таблицу `nk`.

## Граница Ответственности

`visage` отвечает за:

- Lua state и sandbox environment для UI;
- Nuklear context;
- обработку снапшота пользовательского ввода;
- вызов Lua entry-point каждый кадр;
- конвертацию Nuklear-команд в POD-буферы для рендера;
- шрифты, MSDF-атласы и связь `font -> texture_id`;
- базовую стилизацию и SDF-эффекты текста;
- вывод отладочной информации движка, если host передает ее в Lua.

`visage` не должен напрямую менять игровой state. UI может вызвать host API, но этот API должен быть фасадом: отправить message в другую подсистему, запросить настройку, прочитать состояние или попросить main выполнить контролируемое действие. Например, `visage` не должен сам решать, какие ресурсы demiurg загружает и когда.

Целевой host API для Lua:

- управление игрой: выход, fullscreen/windowed, смена разрешения, сохранение настроек;
- звук: play/stop/state, устройство вывода, громкость;
- чтение игрового состояния: режим приложения, loading progress, статистика мира;
- чтение состояния ресурсов из demiurg;
- debug overlay и метрики движка.

## system

`visage::system` - основной runtime-объект UI. Он владеет:

- `sol::state`;
- `sol::environment`;
- `nk_context`;
- `nk_buffer` для draw commands;
- Lua entry function;
- списком зарегистрированных шрифтов;
- CPU-ареной SDF-эффектов текста;
- выходными буферами `vertices_`, `indices_`, `commands_`.

Порядок работы за кадр:

1. `input(input_snapshot_t)` - передает ввод в Nuklear.
2. `set_env_number(...)` или host-specific API - обновляет значения, доступные Lua.
3. `update(time, timestamp, rng_state)` - вызывает Lua entry-point.
4. `convert()` - вызывает `nk_convert()` и заполняет POD-буферы вывода.

Lua entry-point загружается через `load_entry_point(path)`. Скрипт должен вернуть функцию, которую `visage::system` вызывает каждый кадр с аргументами:

- `time` - длительность кадра;
- `timestamp` - монотонное UI-время;
- `rng_state` - непрозрачное состояние PRNG, предоставленное host'ом.

Ошибки Lua вызываются через `sol::protected_function`: ошибка в UI-скрипте логируется warning'ом, а `visage` пытается закрыть незавершенные Nuklear windows, чтобы не оставить `nk_context` в сломанном состоянии.

## Lua Sandbox

`bindings::create_env()` создает sandbox environment:

- `_G` указывает на сам env;
- разрешены базовые функции вроде `assert`, `pairs`, `pcall`, `tostring`;
- копируются безопасные библиотеки `coroutine`, `string`, `table`, `math`, `utf8`;
- `math.random` и `math.randomseed` удаляются;
- `os` ограничен временем: `clock`, `date`, `difftime`, `time`.

`bindings::basic_functions()` добавляет namespace `base`: упаковку чисел, deterministic PRNG helpers, dice/interval, создание таблиц, очереди обхода, `perf`, `script_stack`, информацию о platform/project.

В `visage::system` дополнительно ставится Lua hook по числу инструкций. Сейчас он служит защитой от зависшего скрипта: если UI слишком долго исполняется, hook формирует ошибку с source/name/currentline. Это не полноценный бюджет исполнения, но уже ловит бесконечные циклы.

## Nuklear Bindings

`libs/bindings` экспортирует таблицу `nk` и набор подтаблиц:

- windows: `begin`, `begin_titled`, `endf`, `window.*`;
- layout: `layout.row_dynamic`, `layout.row_static`, `layout.space_*`, templates;
- groups, trees, widgets;
- basic controls: `text`, `label`, `button`, checkbox/radio/option, sliders, progress, color picker, property, knob;
- edit string, plot, combo, tooltip, popup, contextual/menu APIs;
- enums Nuklear для flags, align, colors, symbols и прочего.

Контекст Nuklear сейчас передается в bindings через `bindings::setup_nk_context(ctx)`, то есть фактически глобальным указателем внутри binding layer. Для одного UI-контекста это работает, но для нескольких независимых `visage::system` или UI над юнитами это потребует пересмотра.

Image API пока не оформлен. Часть функций уже принимает `nk_image*`, но функции, которые должны сами построить image из ресурса или региона текстуры, стоят заглушками.

## Шрифты

Шрифт представлен как `visage::font_t`:

- метрики;
- glyph table;
- fallback glyph;
- размер атласа;
- `nk_user_font`;
- texture id GPU-слота.

`font_resource` - многошаговый ресурс:

1. `0 -> 1`: читает TTF в память;
2. `1 -> 2`: генерирует MSDF/MTSDF atlas и метрики через `font_atlas_packer`;
3. `2 -> 3`: заливает atlas в GPU через `painter::gpu_texture_resource`.

CPU-шаги нужны main/UI сразу, потому что Nuklear должен знать метрики glyph'ов. GPU-шаг идет через обычный путь demiurg/assets/render и в итоге выставляет `gpu_index`. После этого `font_t::set_texture_id(gpu_index)` обновляет `nk_user_font::texture.id`.

`visage::system` поддерживает несколько именованных шрифтов через `add_font(name, font_t*)`. Lua выбирает шрифт и размер через `nk.push_font(...)`.

## Текст И SDF-Эффекты

`nk.push_font()` расширен в `visage::system`, потому что bindings не должны зависеть от `font_t`.

Поддержаны варианты:

- `nk.push_font(12)` - только размер;
- `nk.push_font{ font="italic", size=26 }` - именованный шрифт;
- `nk.push_font{ size=34, bold=0.1, softness=..., outline={ color={r,g,b,a}, width=... } }`.

SDF-эффекты складываются в frame arena, а offset записывается в Nuklear userdata. На `convert()` эти параметры запекаются в `gui_draw_command_t`, затем render step передает их в UI shader как push constants.

## Render Output

Рабочий путь вывода UI сейчас находится в `render_output.h`, а не в старом `draw_resource`/`draw_stage` пути. `visage::system::convert()` производит:

- `gui_vertex_t` bytes: `pos`, `uv`, `color`;
- 16-битные индексы;
- `gui_draw_command_t` commands.

`gui_draw_command_t` содержит:

- `elem_count`;
- scissor rect;
- `texture_id`;
- mode: `solid`, `msdf`, `image`;
- параметры SDF-эффекта.

Main отправляет эти буферы render-потоку через broker/write_buffer, а `painter::draw_ui` читает host-visible render-graph ресурсы и выполняет `drawIndexed` по командам.

Mode вычисляется по `texture.id`:

- `0` - solid Nuklear shapes;
- texture id одного из зарегистрированных шрифтов - MSDF text;
- любой другой id - image.

## Интеграция В tile_frontier

В `tile_frontier` `visage` работает в main thread:

- main собирает `input_snapshot_t` из input callbacks;
- UI-скрипт находится в `resources/ui/entry.lua`;
- host регистрирует namespace `app` в `script_env()`;
- `app` содержит звук, window/game controls, input action state, app state и loading progress;
- метрики actor simulation передаются как env numbers;
- UI output пишется в render write-buffer channel.

Текущий Lua script уже рисует:

- boot/splash screen;
- loading screen с progress bar;
- game UI;
- actor stats/debug panel;
- sound player demo;
- fullscreen/quit controls;
- демонстрацию шрифтов, outline/bold SDF text и PRNG.

## Что Сейчас Может

На текущем срезе `libs/visage` умеет:

- создать Lua sandbox и Nuklear context;
- загрузить Lua entry-point из файла;
- принимать мышь, колесо, текстовый ввод и базовые клавиши редактирования;
- выполнять UI Lua каждый кадр;
- защищенно обрабатывать Lua errors;
- ловить потенциально зависший Lua через instruction hook;
- экспортировать большую часть immediate-mode API Nuklear в Lua;
- задавать тему Nuklear из Lua-таблицы;
- регистрировать несколько MSDF-шрифтов;
- менять размер и стиль текста из Lua;
- генерировать UI vertices/indices/draw commands без зависимости от Vulkan в public output;
- отдавать UI в `painter::draw_ui`;
- давать host'у доступ к `script_state()` и `script_env()` для регистрации игрового API.

## Техдолг И Направления

- Описать полноценное понятие `image` для Nuklear: ресурс demiurg, GPU texture slot, регион/UV, размеры, lifetime и Lua-facing handle. Сейчас image-функции частично заглушены.
- Убрать раздражающую Lua-функцию `endf` и заменить ее на более нормальное имя `fin`.
- Попробовать расширить Lua API через closable variables/functions Lua 5.4, чтобы UI-код мог безопаснее закрывать окна/groups/popups и не держать ручные begin/end пары.
- Добавить runtime reload Lua UI: пересоздание или очистка state/env, перезагрузка entry files, сохранение/сброс нужного UI state и корректное восстановление Nuklear context.
- Ввести реальные бюджеты Lua: лимит времени/инструкций на кадр, политика ошибок, планирование `collectgarbage`, метрики GC и понимание, когда дергать сборщик внутри движка.
- Пересмотреть error policy: код `visage` по возможности не должен ронять движок; явные падения и `assert` стоит свести к минимуму. Для Lua, возможно, заменить обычный `assert` на локальный инструмент, который останавливает выполнение текущего скрипта, возвращает точное место ошибки и не ломает весь runtime.
- Разработать систему числовых units для размеров/координат Nuklear: DPI scaling, привязка к размеру окна, проценты/relative values и корректное восстановление позиции окон при изменении размеров, например `width = 80%`.
- Подумать над UI над юнитами: использовать ли `visage` с несколькими контекстами, отдельный lightweight renderer, или специализированную систему world-space labels/widgets.
- Профилировать и бюджетировать Nuklear: стоимость Lua update, `nk_convert`, аллокации буферов, количество вершин/индексов/команд, рост временных буферов.
- Убрать глобальный `nk_context*` из bindings или явно ограничить `bindings` одним активным UI-контекстом. Несколько UI-контекстов сейчас будут конфликтовать.
- Оформить read-only API к игровому состоянию и demiurg resource system: UI должен читать состояние ресурсов, но не должен напрямую запускать загрузку/выгрузку.
- Довести debug overlay до системного инструмента: FPS, frame timings, actor stats, resource loading, render graph state, sound state, memory/channel budgets.
- Разобраться со старым `draw_resource`/`draw_stage` кодом: удалить или перенести идеи в новый `render_output`/`draw_ui` контракт.
