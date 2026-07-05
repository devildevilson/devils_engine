# input

`libs/input` - слой ввода и окна поверх GLFW. Он собирает код, связанный с
клавиатурой, мышью, окном, Vulkan surface integration, именами клавиш и
абстрактными input events.

Главная идея библиотеки - убрать сырой пользовательский ввод из остальных
систем. Gameplay/UI/симуляция должны спрашивать не "нажата клавиша W?", а
"активно ли событие move_left/action/use?". Привязка события к клавишам живет в
input layer.

## Основные Части

`libs/input` сейчас состоит из трех блоков:

- `core.h/.cpp` - тонкая обертка над GLFW: init, окно, мониторы, callbacks,
  курсор, clipboard, Vulkan helpers;
- `key_names.h/.cpp` - registry имен клавиш: canonical, US layout, local/native;
- `events.h/.cpp` - статический слой абстрактных input events поверх scancode
  state.

Геймпады/джойстики пока не реализованы. В коде есть только комментарии о том,
что поддержка может понадобиться позже.

## Core / GLFW Wrapper

`input::init` - RAII-инициализация GLFW:

```cpp
std::unique_ptr<input::init> in;
in = std::make_unique<input::init>(&error_callback);
```

Конструктор вызывает `glfwInit`, ставит error callback и проверяет
`glfwVulkanSupported`. Деструктор вызывает `glfwTerminate`.

Window API покрывает текущие потребности движка:

- `create_window`;
- `destroy`;
- `hide` / `show`;
- `should_close` / `set_should_close`;
- `window_size`;
- `framebuffer_size`;
- `window_focused`;
- `window_iconified`;
- `maximize_window`;
- `restore_window`;
- `set_window_monitor`;
- `window_pos`;
- `window_content_scale`;
- `window_title`;
- `window_monitor`;
- `set_icon`.

`framebuffer_size` возвращает размер framebuffer в пикселях. Это важно для
HiDPI и Vulkan swapchain: размер окна и размер framebuffer могут отличаться.

## Callbacks

`core` exposes GLFW callbacks через typedef'ы и setter-функции:

- window size/content scale/refresh;
- framebuffer size;
- focus/iconify/maximize;
- key;
- character input;
- cursor position/enter;
- mouse button;
- scroll;
- file drop.

Некоторые setter'ы имеют отдельные имена, например
`set_framebuffer_size_callback`, потому что несколько GLFW callback'ов имеют
одинаковую C-сигнатуру и обычная перегрузка была бы неоднозначной.

`input::poll_events()` вызывает `glfwPollEvents()`.

В `tile_frontier` callbacks используются как C-функции без captures; события
накапливаются в file-local структуру, а позиция мыши опрашивается через
`input::cursor_pos`.

## Vulkan Helpers

`input` также содержит GLFW/Vulkan integration:

- `vulkan_supported`;
- `get_required_instance_extensions`;
- `get_instance_proc_addr`;
- `init_vulkan_loader`;
- `get_physical_device_presentation_support`;
- `create_window_surface`.

Это нужно `painter` и `tile_frontier`: окно и surface создаются через input, а
рендер получает нужные Vulkan/GLFW функции без прямой зависимости на GLFW в
каждом месте.

Отдельная важная деталь текущего Vulkan bootstrap: `get_instance_proc_addr()`
возвращает реальный `vkGetInstanceProcAddr` через GLFW. Это используется
рендером для Vulkan-Hpp dynamic dispatch.

## Cursor, Clipboard, URL

Дополнительные helpers:

- `cursor_pos`;
- `set_cursor_input_mode`;
- `set_raw_mouse_motion`;
- `create_cursor`;
- `create_default_cursor`;
- `destroy_cursor`;
- `set_cursor`;
- `clipboard_string`;
- `set_clipboard_string`;
- `open_internet_URL`.

`set_raw_mouse_motion` включает raw mouse motion только если GLFW сообщает, что
режим поддерживается.

`open_internet_URL` сейчас вызывает platform command (`start`, `open`,
`xdg-open`) через `std::system`.

## Key Names

`key_names` решает задачу сопоставления клавиша-название.

Есть три уровня имен:

- canonical name - стабильное имя для конфигов и сохранений;
- US layout name - человекочитаемое QWERTY/US отображение;
- local/native name - имя с учетом текущей раскладки/платформы, если доступно.

Примеры canonical names:

```text
key_w
minus
f10
right_super
kp_1
space
escape
```

Примеры US layout names:

```text
W
-
F10
Right Super
Num 1
Space
Esc
```

Основные функции:

- `key_name_canonical(scancode)`;
- `key_name_us_layout(scancode)`;
- `key_name_local(scancode)`;
- `glfw_key_name_canonical(key)`;
- `glfw_key_name_us_layout(key)`;
- `glfw_key_name_local(key)`;
- `glfw_key_from_canonical(name)`;
- `key_from_canonical(name)`.

Для configs нужно использовать canonical names. Например:

```text
move_left = [ key_a, left ]
use = [ key_e, enter ]
```

Загрузка такого конфига должна парсить token через `key_from_canonical`, получать
`(glfw_key, scancode)`, затем вызывать `events::set_key`.

Runtime key state сейчас keyed by scancode. Это правильнее для привязок клавиш:
GLFW key зависит от раскладки, а scancode ближе к физической клавише.

## Events

`input::events` - статический слой абстрактных действий над клавишами.

Event id:

```cpp
using event_id = utils::id;
```

Создание id:

```cpp
auto use = input::events::make_event_id("use");
```

Привязка:

```cpp
const auto [key, scancode] = input::key_from_canonical("key_e");
input::events::set_key("use", scancode, key, 0);
```

Каждое событие имеет до `event_key_slots_count` клавиш, сейчас это 4 слота.
Каждая клавиша может ссылаться до `key_event_slots_count` событий, сейчас это
16 слотов.

`set_key(std::string_view, ...)` хранит исходное имя события и ловит hash
collision: если тот же `utils::string_hash` пришел с другим именем, будет
`utils::error`.

## Event State Machine

Raw GLFW callback должен вызывать:

```cpp
input::events::update_key(scancode, state);
```

А раз в tick/frame вызывается:

```cpp
input::events::update(dt);
```

`events::update` продвигает внутреннюю state machine клавиш.

Raw key states:

- `release`;
- `press`;
- `repeated`.

Input event states:

- `release`;
- `press`;
- `long_press`;
- `click`;
- `long_click`;
- `double_press`;
- `double_click`.

Есть masks:

- `event_state::press_mask`;
- `event_state::click_mask`.

Проверки:

```cpp
events::is_pressed(event_id);
events::is_released(event_id);
events::check_event(event_id, event_state::press_mask);
events::timed_check_event(event_id, event_state::press_mask, wait, period);
events::current_event_state(event_id);
events::max_event_state(event_id);
```

`timed_check_*` нужен для повторяющихся действий при удержании: сначала wait,
потом срабатывание с period, учитывая `engine_tick_time`.

Durations настраиваются через:

- `set_long_press_duration`;
- `set_double_press_duration`;
- `set_engine_tick_time`.

## Auxiliary Input Data

`events::auxiliary_data()` возвращает небольшой общий буфер:

- mouse wheel;
- text input buffer;
- last frame time;
- double click point/time;
- framebuffer scale.

Это вспомогательный слой для UI/интеграции, а не полноценная event queue.

## Что Уже Умеет

На данный момент `libs/input` умеет:

- инициализировать и завершать GLFW через RAII;
- создавать/уничтожать GLFW window без OpenGL context;
- получать monitor/window/framebuffer параметры;
- переключать fullscreen/windowed через `set_window_monitor`;
- регистрировать основные GLFW callbacks;
- poll'ить события;
- создавать Vulkan surface из window;
- отдавать Vulkan instance extensions и proc addr;
- управлять cursor mode, raw mouse motion и cursor object;
- читать/писать clipboard;
- открывать URL через platform command;
- сопоставлять scancode/key с canonical/US/local names;
- парсить canonical key name обратно в `(glfw_key, scancode)`;
- регистрировать abstract input events;
- привязывать событие к нескольким клавишам;
- отслеживать press/click/long/double states;
- делать timed checks для удержания;
- отдавать список events, привязанных к scancode.

Текущие потребители:

- `painter` использует Vulkan/GLFW helpers для instance extensions,
  presentation support и surface creation;
- `tests/tile_frontier` использует window creation, callbacks, cursor position,
  framebuffer size и poll events;
- render thread получает уже созданный `GLFWwindow*` и создает surface через
  `input::create_window_surface`.

## Что Еще Не Сделано

Открытые направления:

- загрузка/сохранение input bindings из config в `events::init`;
- полноценные control schemes/contexts: например `character`, `vehicle`,
  `aircraft`, `menu`, чтобы одно и то же gameplay event могло иметь разные
  bindings в разных состояниях управляемого объекта;
- стек или приоритет активных input contexts, чтобы menu/gameplay/debug console
  могли перекрывать друг друга;
- явный API для rebinding с конфликтами и UI feedback;
- поддержка mouse buttons/wheel как first-class bindings в `events`, а не только
  через внешние callbacks/auxiliary data;
- поддержка gamepad/joystick;
- event queue или frame snapshot для UI, если callback-driven integration
  станет слишком ad-hoc;
- тесты для state machine `events` и key name mappings;
- устранить статический global state, если понадобится несколько окон или
  несколько независимых input domains.

Граница библиотеки сейчас такая: `input` отвечает за платформенный ввод/окно,
имена клавиш и abstract events. Остальные системы должны работать с событиями
вроде `move_left`, `action`, `use`, а не с сырыми GLFW key/scancode напрямую.
