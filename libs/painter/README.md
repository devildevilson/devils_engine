# libs/painter

`libs/painter` - основной слой отрисовки движка. Сейчас это Vulkan-обертка вокруг декларативного render graph, таблицы GPU-ассетов и набора низкоуровневых утилит для создания Vulkan-объектов. Библиотека должна позволять собрать несколько независимых `graphics_base` в разных местах приложения и запускать в них разные пайплайны, не завязывая весь рендер на один глобальный объект.

Текущая реализация еще экспериментальная: часть старого кода осталась рядом с новым путем, а рабочий путь сейчас строится вокруг `graphics_base`, `render_config_storage`, `common_steps` и `assets_base`.

## Основная идея

Render graph описывает не конкретный C++-код кадра, а набор именованных данных:

- значения и счетчики;
- ресурсы и их буферизацию;
- render targets;
- samplers и descriptor layouts;
- материалы, геометрию и draw groups;
- шаги графа;
- execution passes;
- render graphs как последовательность passes.

Парсер собирает эти данные в `render_config_storage`. После этого `graphics_base::commit_parsed_resources()` переносит описание в рантайм, создает Vulkan-ресурсы, descriptor sets, семафоры и подготавливает базу к созданию конкретного `render_graph_instance`.

Важное свойство: `graphics_base` больше не обязан сам читать папку с конфигами. Конфиг может прийти либо с файловой системы, либо из `demiurg::resource_system` через `render_config_source`. Это нужно для движкового реестра ресурсов и для будущего подключения внешних проектов без прямого знания внутренней структуры библиотеки.

## Render Graph

Render graph делится на два слоя:

1. `render_config_storage` - чистое распарсенное описание без GPU-объектов.
2. `render_graph_instance` - runtime-инстанс с command buffers, render passes, framebuffers, pipelines и локальными семафорами.

`graphics_base` хранит общие ресурсы графа: `resource_containers`, `resources`, `descriptors`, `samplers`, `materials`, `geometries`, `draw_groups`, `steps`, `passes`, `graphs`. При переключении графа создается новый `render_graph_instance`, затем `graphics_base` ждет fences своих кадров и меняет активный инстанс. GPU-ресурсы при этом не пересоздаются, если граф входит в resident-набор.

Resident-графы задаются через `set_startup_graph()`, `add_resident_graph()` или `set_resident_graphs()` до `commit_parsed_resources()`. В этом режиме `graphics_base` считает union used-set по выбранным графам и создает только ресурсы и дескрипторы, которые реально нужны этим графам. Индексы ресурсов остаются глобальными и стабильными: неактивные элементы не компактятся, а просто не получают backing storage.

## Ресурсы И Буферизация

Render-graph ресурс описывает роль, формат, размер, счетчик и тип буферизации. Типы вроде `singlebuffer`, `doublebuffer`, `triplebuffer`, `swapchain`, `frames_in_flight` и `per_update` приводятся к числу runtime-кадров через `resource::compute_buffering()`.

`graphics_base::create_resources()` группирует совместимые ресурсы в `resource_container`:

- буферы могут быть упакованы в общий контейнер с subbuffer offsets;
- изображения могут быть размещены слоями одного image container;
- host-visible роли получают mapped memory;
- present-ресурс создается отдельно как swapchain;
- usage mask собирается из steps, passes и descriptors.

Для буферов отдельно учитывается Vulkan alignment: uniform, storage и texel buffers выравниваются по лимитам физического устройства. Логический размер subbuffer остается отдельным от aligned stride, чтобы descriptor range не выходил за реальные границы ресурса.

Доступ к текущим буферам идет через методы `get_current_buffer_resource_frame()`, `get_current_image_resource_frame()`, `get_current_instance_resource_frame()` и `get_current_indirect_resource_frame()`. Это основной способ внешнему коду записывать данные в host-visible per-update/per-frame ресурсы без ручного вычисления offset'ов.

## Descriptors И Текстуры

`descriptor` описывает binding'и render-graph ресурсов и может дополнительно содержать asset texture binding. Обычные binding'и указывают ресурс, usage, sampler и shader stages. Если sampler задан, layout становится `combinedImageSampler` с immutable sampler.

Asset-текстуры не являются render-graph ресурсами. Для них descriptor может объявить `texture_count`, `texture_sampler` и `texture_stage`. Такой binding заполняется render-потоком из `assets_base.texture_slots`, а индекс в массиве равен `gpu_index` ресурса.

Descriptor pool сейчас считается по фактическому содержимому активных descriptors, включая размер texture array. Это важно для больших таблиц текстур: фиксированный маленький pool быстро переполняется.

Для bindless-like таблицы используется постоянная placeholder-текстура `assets_base::default_texture`. Она лежит вне `texture_slots` и заполняет все еще не загруженные элементы descriptor array, чтобы Vulkan не видел null image view.

## Draw Groups И Команды

`draw_group` описывает буферы instance data и indirect command data. У draw group есть два режима:

- `host_visible` - CPU напрямую пишет instance/indirect данные в per-update ресурсы;
- `device_local` - задел под GPU-generated данные.

Меши связываются с draw group через `mesh_draw_group_pair`. Пара хранит mesh slot, draw group, max instance count, indirect offset и instance offset. Это позволяет одному draw step пробежать набор пар и выполнить indirect draw для каждой.

Сейчас реально используются прежде всего:

- `draw` / `draw_indexed` через draw groups и таблицу mesh slots;
- `draw_ui` для UI-буферов visage;
- `draw_indirect` / `draw_indexed_indirect` как частичный путь через render-graph ресурс;
- `dispatch_constant` / `dispatch_indirect` как ранняя compute-заготовка;
- copy/blit transfer steps для render-graph ресурсов.

`clear_color` и `clear_depth` пока остаются не реализованными step'ами. Часть draw-команд существует как API-скелет и требует проверки на реальных пайплайнах.

## Execution Passes

`execution_pass_instance` создает Vulkan render pass и framebuffers по описанию `execution_pass_base`. Subpass-разметка, attachment usage, load/store actions и barriers приходят из конфига. Перед началом render pass применяется первый набор pass barriers; внутри subpass используется `nextSubpass`, а после pass - финальные barriers.

Барьеры строятся из текущего `graphics_ctx::resources`: каждый runtime resource держит последний usage, а `make_barriers*()` переводит его в нужный Vulkan layout/access/stage. Для UI draw step явные barriers внутри render pass намеренно не вызываются: этот путь опирается на host-visible буферы, записанные до submit, и избегает illegal pipeline barrier внутри subpass.

`render_graph_instance` разбивает passes на execution groups. Для каждой группы создаются command buffers на число кадров в полете, а локальные и глобальные семафоры связывают зависимости между группами.

## graphics_base

`graphics_base` владеет runtime-частью одного независимого графического контекста:

- Vulkan handles: device, queues, command pool, descriptor pool, swapchain, pipeline cache;
- VMA allocator;
- render-graph ресурсы и их containers;
- descriptor layouts/sets;
- global semaphores и fences;
- активный `render_graph_instance`;
- счетчики frame/update/swapchain;
- pipeline cache read/write.

Он не владеет `VkInstance`, `VkDevice` и очередью как жизненным циклом всего приложения. Это позволяет создавать несколько баз поверх одного устройства или в разных подсистемах, если внешний код правильно управляет Vulkan lifecycle.

Поддержаны два режима presentation:

- `presentation_engine_type::main` - есть surface/swapchain;
- `presentation_engine_type::no_present` - headless/no-present режим без swapchain presentation.

## assets_base И GPU-Ассеты

`assets_base` - отдельная таблица GPU-ресурсов для ассетов, которыми управляет `demiurg`. Она не является render graph и не описывает кадр. Ее задача - дать внешним ресурсам стабильный `gpu_index`, который потом используют render code, texture descriptors и draw groups.

Сейчас есть две таблицы:

- `buffer_slots` для mesh buffers;
- `texture_slots` для texture images.

Типичный путь загрузки:

1. `demiurg` доводит ресурс до CPU-состояния.
2. Render thread получает external GPU transition.
3. Ресурс вызывает `load_warm()` с `gpu_load_context`.
4. `assets_base` резервирует слот, создает GPU storage, загружает данные через staging buffer и command buffer.
5. Ресурс получает `gpu_index`.
6. CPU-копия освобождается.

Встроенные demiurg-ресурсы:

- `gpu_texture_resource` - базовый GPU texture upload из RGBA8 памяти;
- `texture_resource` - png/bmp/jpg через stb в RGBA8, затем базовый upload;
- `mesh_resource` - простой vertex-buffer ресурс;
- `glsl_source_file` - текст GLSL + prepared SPIR-V cache;
- `shader_source_file` - готовый SPIR-V;
- `render_config_source` - текстовые `.tavl` файлы render config;
- `pipeline_cache_resource` - начальные байты Vulkan pipeline cache.

## Шейдеры

Шейдеры могут быть загружены двумя путями:

- из `demiurg::resource_system`, если `graphics_base::set_shader_source()` получил registry и prefix;
- напрямую с файловой системы как fallback для старых тестов.

Нормальный путь для `tile_frontier`: assets-поток компилирует `glsl_source_file` в SPIR-V заранее через `prepare_spirv()`, а render thread только создает `VkShaderModule` и pipeline. Если SPIR-V не подготовлен, render thread умеет скомпилировать GLSL сам, но логирует предупреждение: это fallback, а не целевой runtime-путь.

## Что Сейчас Может

На текущем срезе `libs/painter` умеет:

- создать Vulkan render runtime вокруг существующих instance/device/queue;
- работать в windowed и no-present/headless режиме;
- читать render config из папки или demiurg registry;
- создавать render-graph ресурсы с single/double/triple/frames-in-flight/swapchain буферизацией;
- упаковывать буферы и image layers в контейнеры через VMA;
- создавать samplers, descriptor set layouts, descriptor pool и descriptor sets;
- поддерживать immutable samplers и asset texture array;
- создавать render passes, framebuffers, graphics pipelines и command buffers;
- выполнять render graph по execution groups;
- переключать активный render graph без пересоздания resident ресурсов;
- писать host-visible per-update/per-frame данные из внешнего кода;
- регистрировать пары mesh/draw_group и рисовать indirect batches;
- рисовать UI через специальный `draw_ui` step;
- загружать mesh/texture ресурсы в GPU таблицу через demiurg external steps;
- использовать pipeline cache и сохранять его обратно на диск;
- подготавливать GLSL через shaderc с поддержкой demiurg include resolution.

В `tests/tile_frontier` это уже используется для отрисовки тайлов, акторов, UI, загрузки текстур, подготовки шейдеров на assets-потоке и runtime-переключения между resident render graphs.

## Техдолг И Направления

- Дореализовать и проверить все draw/dispatch/transfer команды. Сейчас часть команд есть как заготовка, часть используется только в узком сценарии.
- Разобрать queue model. Сейчас код фактически предполагает один основной `VkQueue`/queue family для graphics+transfer. Нужно поддержать отдельную transfer queue family, queue ownership transfers и fallback-бюджет загрузок, если доступна только одна очередь.
- Убрать `vkQueueWaitIdle`/`vkDeviceWaitIdle` из lifecycle операций конкретного `graphics_base`. Сейчас `graphics_base::commit_parsed_resources()` вызывает `vk::Queue(graphics).waitIdle()` перед пересозданием runtime-ресурсов. Любое ожидание завершения работ, принадлежащих этому `graphics_base`, должно идти только через систему `VkFence` этого `graphics_base` (`wait_all_fences()`/ожидание frame fence), без остановки всей очереди или устройства.
- Навести порядок после серии быстрых интеграционных правок: отделить живой `graphics_base` путь от старого `painter_base`/`execution_pass`, убрать мертвые include/source или явно пометить legacy.
- Стабилизировать внешний data model для проектов: описать минимальный набор `.tavl` сущностей, схемы валидации, ошибки и примеры без необходимости читать реализацию `structures.cpp`.
- Улучшить модель освобождения GPU-ассетов. `unload_hot()` у mesh/texture ресурсов сейчас сбрасывает `gpu_index`, но полноценное освобождение слота через render API еще не завершено.
- Довести device-local draw groups и GPU-generated indirect data до рабочего контура.
- Расширить поддержку форматов, mip levels, texture arrays, MSAA и специализированных render target ролей.
- Сделать более строгую валидацию render graph: несовместимые usage, размеры attachments, отсутствующие ресурсы, неверные queue transitions, неактивные resident dependencies.
- Отдельно описать и протестировать contract для UI buffers, texture descriptor array и shader preparation.
