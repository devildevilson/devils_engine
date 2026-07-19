# aesthetics

`libs/aesthetics` - экспериментальная ECS-библиотека проекта. Ее основная
роль - хранить компоненты разных типов по `entityid_t`, давать быстрые способы
обхода компонентных наборов, рассылать события изменения мира и поддерживать
снапшоты выбранных компонентов в байты.

Дизайн библиотеки строится вокруг нескольких идей:

- компонент каждого типа хранится в своем sparse/dense storage;
- `entityid_t` содержит индекс и версию, чтобы ловить устаревшие ссылки;
- обходы выбирают минимальное компонентное хранилище как базу итерации;
- долгоживущие выборки (`query`) обновляются через события создания/удаления
  компонентов;
- системы можно запускать по `update_event`, в том числе через простой
  многопоточный шаблон;
- сериализация делает полный бинарный дамп зарегистрированных компонентов.

## Entity Id

`entityid_t` по умолчанию является `uint32_t`. Нижние биты хранят версию, верхние
биты - индекс сущности. Количество бит версии задается
`DEVILS_ENGINE_AESTHETICS_VERSION_BITS`, сейчас по умолчанию это 10 бит.

Основные helpers:

- `make_entityid(index, version)`;
- `get_entityid_index(id)`;
- `get_entityid_version(id)`;
- `invalid_entityid`;
- `is_invalid_entityid(id)`.

`world::gen_entityid()` выдает новый id. Если сущность была удалена через
`remove_entity`, ее индекс попадает во free-list, а при повторном использовании
версия увеличивается. Поэтому старый id с тем же индексом больше не должен
разрешаться в компоненты.

## Хранение Компонентов

Для каждого типа компонента `world` создает `sparce_dence_set<T>`.
Название намеренно осталось в текущей форме кода.

Storage состоит из:

- `sparce_set`: индекс сущности -> dense index компонента + версия;
- `components`: плотный массив `T`;
- runtime type id через `aesthetics::component_type_id<T>()`.

Компонент создается через:

```cpp
auto id = world.gen_entityid();
auto* pos = world.create<position>(id, position{});
```

Получение и проверка:

```cpp
auto* pos = world.get<position>(id);
bool has = world.has<position, velocity>(id);
```

Удаление компонента делает swap-remove из плотного массива. Если удаляется не
последний dense-элемент, последний компонент переезжает в освободившееся место,
а sparse запись владельца обновляется.

Важная текущая цена: чтобы найти владельца последнего dense-элемента,
`entity_at_dense_index` сканирует `sparce_set`. Это сохраняет низкий overhead
памяти, но random/forward deletion может быть заметно дороже reverse/tail
удаления. Если это станет узким местом, вероятный следующий шаг - внутренний
sidecar dense-owner array.

## Views

`world::view<T...>()` возвращает ленивый обход сущностей, у которых есть все
запрошенные компоненты.

Особенности текущей реализации:

- базой итерации выбирается компонентный тип с минимальным `count<T>()`;
- итератор проходит raw sparse storage выбранного типа;
- для каждой сущности проверяется наличие остальных компонентов;
- результатом является tuple вида `(entityid_t, T*...)`;
- даже `const world::view<T>()` возвращает mutable pointers через
  `component_array::get_mutable`.

Последний пункт осознанный: const world фиксирует membership/структуру мира, но
разрешает менять payload компонентов. Это удобно для систем, которые получают
мир как read-only контейнер членства, но должны обновлять данные компонентов.

`view<T...>()` требует, чтобы allocator/storage для всех типов уже существовал.
Если storage не создан, текущая реализация кидает `utils::error`. Поэтому перед
некоторыми hot-path view имеет смысл заранее создать allocator через
`get_or_create_allocator<T>()`.

## Lazy Views

`world::lazy_view<T...>()` собирает множество всех entity id, у которых есть
хотя бы один из указанных типов компонентов. В отличие от обычного `view`, это
не intersection, а union по типам.

Результат все равно возвращает tuple `(entityid_t, T*...)`; часть указателей
может быть `nullptr`.

Этот режим полезен, когда нужно обработать все сущности, затронутые набором
компонентов, но не обязательно имеющие полный набор.

## Queries

`world::query<T...>()` - долгоживущая материализованная выборка intersection:
сущность находится в query, если у нее есть все `T...`.

Query подписывается на:

- `create_component_event<T>`;
- `remove_component_event<T>`.

При создании или удалении компонентов query обновляет свой внутренний
отсортированный массив `entityid_t`. Это делает последующий обход дешевым:
система проходит уже готовый список, а не пересканирует storage каждый тик.

`world::lazy_query<T...>()` - материализованная union-выборка: сущность находится
в списке, если у нее есть хотя бы один компонент из `T...`. При удалении
компонента она остается в query, пока у нее есть другой компонент из этого
набора.

Query живет как объект и в деструкторе отписывается от событий. После загрузки
снапшота компоненты создаются мимо обычного `world::create`, поэтому create-события
не летят. `load_world` в конце шлет `snapshot_loaded_event` из `common.h`;
`query_t` и `lazy_query_t` подписаны на него и автоматически вызывают `rebuild()`
через `view<T...>()` / `lazy_view<T...>()`. Остальные project-owned кэши при
необходимости подписываются на то же событие самостоятельно.

## Events

События реализованы через typed receivers:

- `basic_reciever<Event_T>`;
- `world::subscribe<Event_T>(receiver*)`;
- `world::unsubscribe<Event_T>(receiver*)`;
- `world::emit(event)`.

Тип события получает runtime id через `aesthetics::event_type_id<T>()`.

Встроенные события:

- `create_component_event<T>`;
- `remove_component_event<T>`;
- `update_event`;
- `snapshot_loaded_event` (`serial::snapshot_loaded_event` оставлен как compatibility alias).

Система событий простая: `world` хранит вектор подписчиков по type id и вызывает
их синхронно в `emit`. Внутренней синхронизации сейчас нет, поэтому изменение
списка подписчиков и структурные изменения мира должны быть организованы
вызывающим кодом.

## Systems

Базовая система - это `basic_system`, то есть receiver для `update_event`.

```cpp
class my_system : public aesthetics::basic_system {
public:
  void update(size_t time) override;
};
```

`world::create_system<T>(...)` создает объект во внутреннем arbitrary container
мира и подписывает его на `update_event`. `remove_system` отписывает и удаляет.

`template_system<Comp...>` — range-map над стабильным ECS query. Система описывает
только `process(tuple,time)`; разбиение на задачи и barrier принадлежат внешнему
`aesthetics::run` из `system_runner.h`:

```cpp
integration.dt = dt;
drives.dt = dt;
aesthetics::run(pool, tick, integration, drives);
```

Runner сначала submit-ит чанки независимых систем в один pool phase, вызывающий
поток участвует в обработке, затем ставится ровно один общий `wait`. Worker'ы могут
начать первый submit ещё при формировании фазы, поэтому все аргументы одного `run`
должны быть независимы без предположений о взаимном порядке.

Поддерживаются три формы работы:

- `template_system` — range-query, автоматически режется через `distribute1`;
- `worklist_system` — explicit work-list с per-thread scratch, только enqueue без
  внутреннего barrier;
- обычный `basic_system::update(time)` — одна неделимая pool-задача, поэтому несколько
  независимых ST build/gather-систем могут исполняться одновременно;
- `single(fn)` — одна неделимая задача в пуле; несколько независимых serial gather
  могут исполняться параллельно друг другу.

`template_system_mt` и его `update()` сохранены как compatibility facade и сами
делегируют `run`; новый код для общей фазы использует обычный `template_system`.
Структурные create/remove/commit остаются отдельными шагами после barrier.

## Многопоточность

Дизайн учитывает многопоточную обработку, но `world` не является полностью
thread-safe контейнером.

Безопасный ожидаемый паттерн:

- структурные изменения мира выполняются в контролируемой фазе;
- перед worker-фазой формируются стабильные query/view/input списки;
- worker'ы пишут в непересекающиеся компоненты или в thread-local output;
- apply-фаза сортирует/сливает результаты детерминированно.

Такой паттерн используется в `subprojects/tile_frontier`: cognition выбирает акторов
по бюджету, worker'ы считают решения и intents, затем apply-фаза стабильно
меняет компоненты.

`run` не выводит зависимости из типов компонентов и не строит job graph. В частности,
переданный как неделимый `update(time)` не должен сам делать pool dispatch/wait:
ответственность за отсутствие data races между переданными системами остаётся на
проектном pipeline. В `tile_frontier` первая живая группа — `integration + drives`:
обе читают velocity, но пишут соответственно position и stats.

## Сериализация

`libs/aesthetics` содержит собственный бинарный serializer в
`aesthetics::serial`.

Нижний слой:

- `writer` и `reader` пишут/читают little-endian байты;
- выход за границы reader выставляет `ok=false`, а не кидает исключение;
- format host-independent для integer/float при условии IEEE-754 float/double.

Компоненты регистрируются макросом:

```cpp
SERIALIZABLE_COMPONENT(position)
```

Зарегистрированный компонент должен быть aggregate type. Поля сериализуются
через `reflect`, поддерживаются:

- arithmetic и enum;
- `std::string`;
- `std::array`;
- `std::vector` и другие sequence-like контейнеры;
- map-like контейнеры, включая `gtl::flat_hash_map`;
- `std::unique_ptr` / `std::shared_ptr`;
- `std::optional`;
- `std::pair`, `std::tuple`, `std::variant`;
- вложенные aggregate-типы.

Для внешних не-aggregate типов, например glm-подобных векторов, предусмотрена
точка расширения `serial::adapter<T>`. Adapter задает стабильное имя типа для
fingerprint и функции `write/read`.

Hash-map контейнеры сериализуются в отсортированном по ключу порядке, чтобы
байты были детерминированными независимо от capacity и порядка итерации.

## Snapshot Format

`dump_world` пишет полный снапшот зарегистрированных компонентов:

```text
[magic:u32][fingerprint:u32]
[cur_index:u64][removed_count:u64][removed entity ids...]
[block_count:u32]
block*: [component_hash:u32][byte_len:u32][count:u32][(entityid_t, component)*]
```

В снапшот попадает:

- состояние генератора id (`cur_index`, free-list removed entities);
- все компоненты зарегистрированных типов;
- полные `entityid_t`, включая версии;
- компонентные данные.

Схема защищена fingerprint'ом: он строится из hash имени компонента и layout
hash типа. Если magic или fingerprint не совпали, `load_world` возвращает
`false` и не кидает исключение.

Загрузка ожидает чистый `world`. Она пишет компоненты напрямую в storage, мимо
`world::create`, чтобы не будить системы на полупостроенном мире. После успешной
загрузки эмитится `serial::snapshot_loaded_event`.

## Sink Layer

`sink.h/.cpp` - верхний слой упаковки уже готового payload.

Он не знает, что внутри payload: это может быть `dump_world`, network delta или
любой другой бинарный блок. Формат контейнера:

```text
[magic u32][version u16][algo u8][flags u8]
[raw_size u64][payload_size u64][checksum u64]
[optional screenshot_size u32 + screenshot bytes]
[payload]
```

`seal` упаковывает payload, считает checksum по сырому payload и опционально
сжимает данные. Если сжатие не уменьшило размер, payload хранится как есть.

Готовые политики:

- `disk_policy`: нормальное сжатие + возможность embedded screenshot;
- `network_policy`: быстрое сжатие без screenshot.

Удобные wrappers:

- `pack(world*)` / `unpack(bytes, world*)`;
- `save_to_file(world*, path)` / `load_from_file(world*, path)`.

## Что Уже Умеет

На данный момент `libs/aesthetics` умеет:

- создавать entity id с index/version схемой;
- переиспользовать удаленные entity индексы с увеличением версии;
- хранить компоненты в per-type sparse/dense storage;
- создавать, получать и удалять компоненты по `entityid_t`;
- находить компоненты сущности через typed и raw API;
- обходить intersection компонентов через `view<T...>()`;
- обходить union компонентов через `lazy_view<T...>()`;
- поддерживать event-driven `query<T...>()` и `lazy_query<T...>()`;
- рассылать typed events;
- создавать простые `basic_system` через `update_event`;
- запускать query-based системы в один поток или через `thread::atomic_pool`;
- копировать underlying sparse/dense данные для внешних систем;
- делать бинарный dump/load зарегистрированных компонентов;
- сериализовать сложные aggregate-типы и внешние типы через adapter;
- проверять snapshot magic/schema и checksum контейнера;
- упаковывать снапшоты для disk/network сценариев.

Тесты в `tests/aesthetics_world_test.cpp` покрывают:

- sparse/dense хранение без отдельного dense entity list;
- восстановление entity versions в raw iterator;
- mutable component payload через const world view;
- обновление `query` по create/remove events;
- поведение `lazy_query` при удалении последнего matching component.

Тесты в `tests/aesthetics_serialization_test.cpp` покрывают:

- round-trip компонентов, entity refs и состояния генератора id;
- защиту от неверного magic/fingerprint;
- сериализацию vector/string/array/unique_ptr/flat_hash_map;
- adapter для внешнего не-aggregate типа;
- `snapshot_loaded_event`;
- empty world snapshot;
- детерминированную сериализацию hash-map;
- sink pack/unpack, compression, embedded screenshot и checksum failure.

## Что Еще Не Сделано

Текущая ECS уже пригодна как рабочий экспериментальный слой, но остаются
открытые направления:

- более строгая политика structural changes во время многопоточной обработки;
- sidecar dense-owner array или другая оптимизация random deletion;
- migration/versioning для старых snapshot-схем, сейчас mismatch просто
  отвергается;
- автоматическая пересборка `query_t` и `lazy_query_t` по
  `snapshot_loaded_event`: перенести объявление события из `serialization.h` в
  низкоуровневый заголовок, добавить snapshot receiver в query и общий
  `rebuild()` для заполнения container после загрузки;
- расширение сериализации на частичные дампы, дельты или selective component
  sets, если это понадобится для save/network workflows.

Границу библиотеки сейчас лучше держать такой: `aesthetics` отвечает за ECS
storage, component iteration, локальные события, простые gameplay systems и
снапшоты ECS-данных. Конкретная gameplay-логика, actor scheduling и
межсистемный протокол должны оставаться уровнем выше.
