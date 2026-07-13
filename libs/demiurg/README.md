# demiurg

`libs/demiurg` - экспериментальная система ассетов и ресурсов движка. Ее
задача - собрать файлы из модулей в единый реестр ресурсов, дать стабильные
handles на эти ресурсы и управлять staged загрузкой/выгрузкой через общий
resource loader.

В текущей архитектуре durable-валюта ресурса - `resource_handle` (пара
`{const resource_system*, utils::id hash}`, где hash - это хеш логического id, а
не указатель). Handle резолвится через `system->get(hash)` и переживает
пересборку registry: тот же id снова резолвится в новый объект. Сырой
`resource_interface*` по-прежнему ок для одномоментного чтения в одном потоке,
но межпоточные сообщения и хранимые ссылки должны держать `resource_handle`.
Ресурсная система строит registry один раз на старте или при загрузке набора
модулей, а gameplay/render/sound слои дальше держат handles и запрашивают
состояние через `resource_loader`.

## Основные Понятия

В `demiurg` есть четыре центральных сущности:

- **Модуль** - область на диске: папка или zip/mod архив.
- **Ресурс** - файл внутри модуля, превращенный в объект `resource_interface`.
- **Ресурсная система** - registry типов и найденных ресурсов.
- **Загрузчик ресурсов** - reconciler, который доводит ресурс до нужного
  состояния загрузки или выгрузки.

Модуль отвечает только за перечисление файлов и чтение байт/текста. Ресурсная
система решает, какой тип ресурса соответствует пути и расширению. Конкретный
тип ресурса знает, как грузить и выгружать свои данные.

## Modules

Базовый интерфейс модуля - `module_interface`.

Он предоставляет:

- `open()` / `close()`;
- `resources_list(resource_system*)`;
- `load_binary(path, out)`;
- `load_text(path, out)`.

Сейчас есть две реализации:

- `folder_module` - рекурсивно обходит папку;
- `zip_module` - читает zip/mod архив через minizip.

Путь, который получает ресурс, относительный к корню модуля. Например файл

```text
resources/engine/render_config/materials/tile.tavl
```

в модуле `resources/engine/` становится ресурсом с path:

```text
render_config/materials/tile.tavl
```

Модуль также задает `module_name`, чтобы ресурсы из разных модулей можно было
связывать в replacement/supplementary цепочки.

## Module System

`module_system` хранит список подключенных модулей для одного root path.

Основной flow:

```cpp
demiurg::module_system modules(root);
modules.load_modules({ demiurg::module_system::list_entry{"engine", "", ""} });

demiurg::resource_system resources;
resources.parse_resources(&modules);
```

`load_default_modules()` пытается загрузить список модулей из json. Если имя
списка пустое, fallback сейчас ищет `core.zip`, затем `core/`.

`load_modules()` принимает список entries, проверяет наличие путей, для файлов
может сверить SHA-256 hash, затем создает `folder_module` или `zip_module`.

`module_system::parse_resources(resource_system*)` открывает все модули,
просит каждый перечислить ресурсы и закрывает модули.

## Resource Id

`resource_interface::set()` выводит resource id из относительного пути файла.

Пример:

```text
mesh/actors/wolf.mesh
```

дает:

```text
path = "mesh/actors/wolf.mesh"
id   = "mesh/actors/wolf"
ext  = "mesh"
```

Id не включает расширение. Это важно для составных ресурсов: файлы с одинаковым
id, но разными расширениями, могут входить в один resource type. Например тип
может зарегистрировать расширения `"obj,mtl"`, где `.obj` является основным
файлом, а `.mtl` - supplementary частью того же id.

Ресурсы в разных модулях с одинаковым id становятся replacement variants одного
логического ресурса. Ресурсы в одном модуле с одинаковым id, но разными
расширениями, становятся supplementary chain вокруг основного ресурса.

## Resource Types

Перед парсингом модулей сторонние системы должны зарегистрировать свои типы:

```cpp
resources.register_type<texture_resource>("texture", "png,jpg");
resources.register_type<mesh_resource>("mesh", "mesh");
resources.register_type<sound_resource>("sound", "wav,ogg,mp3");
```

Имя типа не просто метаданные. `resource_system::find_proper_type()` ищет
registered type name среди сегментов resource id и проверяет расширение.

Например для типа:

```cpp
register_type<mesh_resource>("mesh", "mesh")
```

файл должен лежать в type-named сегменте пути:

```text
mesh/test.mesh
actors/mesh/test.mesh
```

А файл:

```text
test.mesh
```

не будет подобран типом `mesh`, потому что в id нет сегмента `mesh`.

Это текущий важный контракт: тип ресурса выводится из пути, а не только из
расширения.

## Resource System

`resource_system` хранит:

- зарегистрированные типы ресурсов;
- список активных ресурсов `resources`;
- список всех созданных resource objects `all_resources`;
- per-type allocator'ы;
- per-type exemplary list.

Основные операции:

- `register_type<T>(name, ext)`;
- `parse_resources(module_system*)`;
- `append_resources(module_system*)`;
- `get(id)` / `get(utils::id hash)`;
- `get<T>(id)`;
- `handle(id)` / `handle(utils::id hash)` -> `resource_handle`;
- `resource_hash(id)` (static) -> `utils::id`;
- `find(prefix)`;
- `filter(substring)`;
- `filter_typed(type_name, out)`;
- `clear()`.

`parse_resources()` очищает старый registry, открывает модули, создает ресурсы,
затем строит итоговый sorted список `resources`. Если несколько ресурсов имеют
один id, они связываются ring-list'ами:

- `replacement` - варианты из разных модулей;
- `supplementary` - несколько файлов одного составного ресурса;
- `exemplary` - список всех ресурсов одного registered type.

Итоговый `resources` содержит по одному активному resource pointer на id.
`all_resources` содержит все созданные варианты, включая replacement и
supplementary entries.

`append_resources()` добавляет ресурсы в уже существующий registry без полного
пересчета override-цепочек. Это рассчитано на отдельные подреестры без
пересекающихся id, например writable cache module. При коллизии id новый ресурс
пропускается с warning.

## Resource Handle

`resource_handle` - стабильный handle на ресурс:

```cpp
struct resource_handle {
  const resource_system* system;
  utils::id hash; // хеш логического id, не указатель

  resource_interface* get() const noexcept;    // system->get(hash)
  template <typename T> T* get() const noexcept; // + проверка loading_type_id
  explicit operator bool() const noexcept;
};
```

Ключевая идея: handle хранит ХЕШ логического id, а не указатель, поэтому
переживает `clear()` + повторный `parse_resources()` - тот же id снова
резолвится в свежесозданный объект. Внутри `resource_system` за это отвечает
`resources_by_hash` (индекс `utils::id -> resource`), который перестраивается в
конце `parse_resources()`/`append_resources()` (`rebuild_hash_index`).
Коллизия разных id с одним хешем - `utils::error`.

Handle создаётся через `resource_system::handle(id)` / `handle(hash)`; хеш можно
посчитать заранее статикой `resource_hash(id)`. Потребители (`flow`,
межпоточные сообщения `tile_frontier`) держат handle вместо сырого указателя.

## Resource Interface

Все конкретные ресурсы наследуются от `resource_interface`.

Базовые поля:

- `path`;
- `id`;
- `ext`;
- `module_name`;
- `type`;
- `loading_type_id`;
- `module`;
- `raw_size`;
- `dependencies`;
- flags;
- atomic `_state`.

Ресурс обязан реализовать базовые действия:

```cpp
void load_cold(const utils::safe_handle_t& handle) override;
void load_warm(const utils::safe_handle_t& handle) override;
void unload_warm(const utils::safe_handle_t& handle) override;
void unload_hot(const utils::safe_handle_t& handle) override;
```

Для обычного трехуровневого ресурса состояния такие:

- `cold` - ресурс только известен в registry;
- `warm` - данные загружены/подготовлены на CPU;
- `hot` - ресурс готов к использованию внешней системой, например GPU.

Флаг `warm_and_hot_same` означает CPU-only ресурс: effective final state равен
`warm`, и отдельный hot-переход не нужен.

Флаг `binary` используется типами ресурса как признак бинарного/текстового
чтения.

## Multi-Step Resources

`resource_interface` уже поддерживает обобщенную лестницу состояний
`0..top_state()`.

Обычные ресурсы используют default mapping:

- `0 -> 1`: `load_cold`;
- `1 -> 2`: `load_warm`;
- `2 -> 1`: `unload_hot`;
- `1 -> 0`: `unload_warm`.

Сложный ресурс может переопределить:

- `top_state()`;
- `load_step(from, handle)`;
- `unload_step(from, handle)`;
- `is_external_step(from)`.

Это используется для staged ресурсов, где тяжелая CPU-подготовка должна
произойти на assets thread, а финальный commit - на render/GPU thread. Например
pipeline или font atlas может идти через цепочку:

```text
cold -> parsed -> prepared -> GPU
```

`final_state()` учитывает `warm_and_hot_same`. `usable()` означает
`state() >= final_state()`.

## Dependencies

У ресурса есть `dependencies`: список `resource_interface*`.

Загрузчик доводит зависимости до `dep->final_state()` перед продвижением
ресурса вверх. Это позволяет, например, pipeline ждать shader source или
prepared shader resource.

Текущий контракт предполагает DAG. Циклы не поддерживаются как корректный
граф, хотя `request()` защищен от бесконечной рекурсии через уже заведенные
entries.

Важно: ресурс с dependencies нужно продвигать через `resource_loader`, а не
звать `res->load()` напрямую. Прямой вызов обходит dependency gating.

## Resource Loader

`resource_loader` - однопоточный reconciler. Он живет в actor/thread, который
владеет asset management.

Публичный API:

```cpp
loader.request(resource, target_state);
loader.update(external_jobs);
loader.external_done(resource);
```

`request(res, target)` задает желаемое состояние. Target clamp'ится к
`res->final_state()`. Если запрос уже существует, последний target побеждает.
При первом request loader также рекурсивно заводит requests для dependencies.

`update(out)` проходит по pending entries и двигает каждый ресурс на один шаг к
target:

- локальные шаги выполняются сразу на вызывающем потоке;
- external шаги добавляются в `out` как `external_job`;
- ресурс с external job помечается `in_flight`;
- после завершения внешнего шага владелец вызывает `external_done(res)`.

External job сейчас означает переход, который loader не должен делать сам:
обычно GPU/render-owned commit или release. В default 3-state ресурсе
`warm -> hot` считается external, если не выставлен `warm_and_hot_same`.
Многошаговый ресурс может переопределить `is_external_step(from)`.

Это разделение уже используется в `tile_frontier`: assets actor делает CPU-load
и CPU-prepare, затем отправляет GPU transition в render actor, а render actor
после commit отправляет ack.

## Module Listing

`modules_listing` - вспомогательный слой для просмотра доступных модулей и
списков модулей в root folder.

Он умеет:

- просканировать root;
- найти папки и zip/mod файлы;
- посчитать hash для архивов;
- загрузить `.json` списки;
- сохранить list-файл;
- вернуть available lists и module entries.

Это не основной runtime registry, а tooling/helper вокруг выбора набора модулей.

## Что Уже Умеет

На данный момент `libs/demiurg` умеет:

- представлять модуль как папку или zip/mod архив;
- перечислять файлы модуля как resource candidates;
- читать из модуля binary/text данные;
- регистрировать resource types с именем и набором расширений;
- выводить тип ресурса из сегмента пути и расширения;
- создавать typed resource objects через per-type allocator;
- строить sorted registry ресурсов;
- возвращать ресурс по id и typed id;
- искать ресурсы по prefix или substring;
- хранить replacement/supplementary/exemplary ring chains;
- добавлять непересекающийся подреестр через `append_resources`;
- хранить dependencies между ресурсами;
- вести atomic state ресурса;
- поддерживать 3-state и custom N-step resource FSM;
- загружать/выгружать ресурсы через `resource_loader`;
- отдавать external jobs для render/GPU-owned шагов;
- давать стабильный `resource_handle` (по хешу id), переживающий rebuild registry;
- разворачивать `.tavl` list-файлы (`//---`) в отдельные суб-ресурсы `path:name` / `path:index`;
- диагностировать циклы dependency graph при `request()`;
- быть источником lua resource API (`request`/`require`/`find`/`filter` в UI-sandbox, host-side).

Текущие реальные потребители:

- `subprojects/tile_frontier` строит engine registry для config/render_config/shaders
  и отдельный assets registry для game resources;
- `painter` использует demiurg resources для shader sources, render config
  source, pipeline cache, mesh/texture resources;
- `sound` использует demiurg-backed sound resources;
- `visage` использует multi-step `font_resource`;
- `tests/demiurg_resource_loader_test.cpp` проверяет external GPU step и
  dependency gating.

## Что Еще Не Сделано

Открытые направления:

- полноценный первый запуск/asset manager, который строит полный registry
  проекта и сохраняет/переиспользует результаты;
- import-rule таблица «type IS the path» (longest-segment match) - сейчас тип
  выводится через `find_proper_type` по сегменту пути;
- lua `find` пока prefix-, не by-kind (по типу); `:name` для lua-модулей не определён;
- более строгая политика override порядка модулей и priorities;
- migration/versioning metadata для ресурсов;
- фоновая загрузка с thread pool и per-tick budget;
- cancellation/priority для pending loader requests;
- более строгий контракт для zip resources, когда тип не зарегистрирован до
  parse.

Граница библиотеки сейчас такая: `demiurg` отвечает за discovery ресурсов из
модулей, построение registry, typed resource handles и staged load/unload FSM.
Конкретное декодирование форматов, GPU upload, звуковой decode и gameplay
интерпретация ресурсов должны жить в типах ресурсов и подсистемах выше.
