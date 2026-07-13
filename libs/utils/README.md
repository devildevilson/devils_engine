# libs/utils

`libs/utils` - общий набор низкоуровневых инструментов, которые используются остальными
подпроектами движка. Сюда попадают структуры данных, аллокаторы, потоковые примитивы,
хеширование, сериализация, работа с файлами, временем, строками и разные небольшие
инфраструктурные функции.

Идея библиотеки прагматичная: если в конкретной системе появляется удачная обобщаемая
механика, которую можно переиспользовать в других местах, ее стоит переносить сюда. Первый
очевидный кандидат на такой перенос - сериализатор из `libs/aesthetics`, который делает
байтовый дамп C++ агрегатов для ECS snapshot.

## Что сейчас умеет подпроект

### Базовый слой

`utils/core.h` сейчас является перегруженным фундаментальным заголовком. В нем лежат:

- ошибки, предупреждения, логирование и assert-обертки с `std::source_location`;
- небольшие математические helpers: округление, `align_to`, `next_power_of_2`, min/max и
  похожие функции;
- функции печати и трассировки;
- пути приложения/кеша/проекта;
- получение имени CPU;
- CRC32C helpers;
- перегрузки для преобразования строк разных Unicode-представлений.

Это удобно как единая точка входа, но заголовок уже смешивает диагностику, математику,
платформенные пути, Unicode и хеширование. Его стоит оставить как совместимый umbrella-header,
но внутренне разнести на более узкие файлы.

### Файлы, строки, время и локаль

- `fileio.h` дает простые операции чтения/записи/append для строк, байтов и typed-векторов,
  а также проверки `exists`, `size`, `is_directory`, `is_regular_file`.
- `string-utils.hpp` содержит split/trim/slice, case-insensitive поиск, парсинг чисел и
  dice-строк.
- `time-utils.hpp` содержит timestamp helpers, форматирование UTC/local времени, счетчики
  длительности, `app_clock`, `app_timer`, `game_date` и экспериментальную календарную
  систему.
- `locale-utils.hpp` хранит компактный код локали в 32-битном контейнере.
- `patterns.h` содержит проверки строковых паттернов: число, IP, валидное имя, ключ
  локализации, split по паттерну.
- `utf/utf.hpp` содержит валидацию и конвертацию UTF-8/UTF-16/UTF-32.

### Хеши, id и случайные числа

- `hash.h` содержит constexpr-миксеры и Murmur3 x86 32-bit для строковых id.
- `string_id.h` разделяет два сценария: stateless `string_hash` через rapidhash и
  `string_pool`, который выдает плотные последовательные id для заранее зарегистрированных
  строк.
- `type_traits.h` содержит type-id, function traits, проверки контейнеров/мап и другие
  compile-time helpers.
- `prng.h` содержит набор PRNG семейств: splitmix, xorshift/xoroshiro/xoshiro, PCG, CMWC и
  функции смешивания seed.
- `dice.h` строит поверх PRNG броски кубиков и выбор интервала.
- `sha256.h`/`sha256cpp.h` дают C и C++ обертки SHA-256.

### Сериализация и сжатие

- `named_serializer.h` оборачивает JSON через glaze, binary через zpp_bits и умеет
  сериализовать отражаемые структуры в Lua-table строку.
- `compression.h` дает zstd-сжатие общего назначения с уровнями `fast`, `normal`, `high`,
  `best` и vector-обертками.

Текущий слой сериализации полезен для конфигов и структур, но он не закрывает полностью
задачу "быстрый сырой snapshot состояния". Эту роль лучше выделить отдельно и перенести
из `libs/aesthetics` байтовый serializer агрегатов в условный `utils/serialization`.

### Аллокаторы и управление памятью

- `stack_allocator` - линейная арена фиксированного размера с `clear`, offset helpers и
  `create` для trivially destructible типов.
- `stack_allocator_mt` - атомарная версия линейной арены.
- `fixed_pool_mt` - lock-free free-list по фиксированным блокам; используется для маленьких
  задач в `thread::atomic_pool`.
- `dynamic_stack_allocator` - растущая линейная арена с grow policy.
- `dynamic_allocator` - растущий контейнер для объектов разного размера с итерацией по
  размещенным блокам.
- `arena_allocator` - арена с заголовками аллокаций и частичным `free`.
- `block_allocator`/`block_allocator_mt` - блочные аллокаторы с free-list и возможностью
  выделять новые memory chunks.
- `memory_pool<T>` - typed pool с блочной дозагрузкой памяти и возвратом объектов в free-list.
- `cleanup` - RAII-вызов callback при выходе из scope.
- `safe_handle_t` - type-tagged `void*` handle с проверкой типа через `utils::type_id`.

Эти инструменты покрывают разные жизненные циклы памяти: frame/stack аллокации, пул задач,
пулы однотипных объектов и ручные арены. Сейчас их стоит документировать не как
взаимозаменяемые аллокаторы, а как разные политики владения и освобождения.

### Потоки и межпоточные каналы

В `devils_engine/thread` лежат два поколения инструментов.

Актуальный слой для межпоточного обмена:

- `spsc_queue<T>` - фиксированная SPSC FIFO-очередь. При переполнении возвращает `false` и
  дропает новое сообщение.
- `mailbox<T>` - latest-wins triple-buffer для снапшотов. Продюсер пишет в `write_slot()` и
  публикует, consumer забирает самый свежий слот через `consume()`.
- `byte_ring` - SPSC byte arena с монотонными позициями и contiguous allocation.
- `payload_channel<Msg>` - `spsc_queue<Msg>` плюс `byte_ring` для сообщений с byte payload.
  Сообщение должно хранить `pos` и `size`.

Эти примитивы уже стали основой message broker в `subprojects/tile_frontier`: mailbox для
latest-state каналов, SPSC queue для команд, payload channel для больших byte payload.

Старший/общий слой:

- `pool` - классический thread pool на `std::queue<std::function<void()>>`;
- `stack_pool` - thread pool, который размещает job-объекты в `stack_allocator`, чтобы
  уменьшить динамические аллокации;
- `atomic_pool` - более свежий pool на bounded atomic queue и `fixed_pool_mt`;
- `atomic.h` - `atomic_min/max`;
- `lock.h` - spin mutex, light spin mutex, busy-wait sleep helpers и интерфейс семафора.

### События и загрузочные стадии

- `event_consumer.h` и `event_dispatcher.h` содержат intrusive consumer-list dispatcher и
  bucket-based event dispatcher с single-thread и mutex-вариантом.
- `loader.h`/`load_stage.h` описывают цепочку стадий загрузки с прогрессом и простым
  semaphore-like ожиданием.

Старый `actor_ref` перенесён в корневой `exclude/`: в актуальном `tile_frontier`
межпоточный контракт задаёт единый broker поверх `thread::mailbox`,
`thread::spsc_queue` и `thread::payload_channel`.

### Пространственные структуры и geometry

`geometry.h` задает dependency-free геометрические примитивы и общий контракт
`query(shape, visit)`: форма должна уметь `contains(point)` и `overlaps(aabb)`.
Поддерживаются AABB, sphere/disk, up-cylinder, ray, cylinder и OBB.

Поверх этого лежат:

- `kd_tree.h` - статическое k-d дерево над точками, рассчитанное на rebuild раз за кадр и
  запросы nearest/radius/query;
- `grid.h` - dense и hash grid с инкрементальными insert/remove/update, стабильными handles и
  единым query-контрактом;
- `aabb_tree.h` - динамический BVH для расширенных объектов с AABB, стабильными handles,
  SAH-вставкой, refit и балансировкой.

Это уже оформленный spatial toolkit для gameplay/AI/physics-like запросов без зависимости
`utils` от glm.

### Прочие структуры данных

- `list.h` - intrusive forward/ring lists и atomic stack helpers.
- Старые `double_buffer`/`interpolation_buffer`/`shared_data_system`, JSON-era
  `serializable`, сломанный `prng_string` и неиспользуемый `perf` перенесены в
  корневой `exclude/`. Активные замены — broker channels, tavl/
  `aesthetics::serial` и catalogue introspection.

## Особенности реализации

- Библиотека держит `utils` максимально низкоуровневым слоем: большинство структур
  payload-agnostic и не знают о конкретных подсистемах движка.
- Многие инструменты написаны под фиксированные бюджеты и предсказуемое поведение:
  bounded queue, preallocated mailbox slots, stack/fixed pool аллокации.
- Spatial слой намеренно не тянет glm: вектор задается шаблонным типом с `operator[]`.
- В потоковых каналах явно разделены семантики: latest-wins snapshot, reliable/lossy FIFO и
  byte payload.
- В проекте присутствует код разных поколений. Часть заголовков является рабочим
  фундаментом, часть - экспериментальными заготовками, часть - legacy после появления broker.

## Что стоит сделать дальше

1. Разнести `utils/core.h`.
   Оставить его как compatibility include, но вынести содержимое в узкие заголовки:
   `diagnostics/log.h`, `math.h`, `platform_paths.h`, `unicode.h`, `crc.h`.

2. Разделить публичный и experimental API.
   `actor_ref` и `context_stack` уже прошли аудит и перенесены в корневой `exclude/`.
   Для старых event dispatcher, части loader-слоя и похожих заготовок ещё нужно решить:
   поддерживаем, переносим в `legacy/experimental` или удаляем после миграции пользователей.

3. Оформить `thread` как отдельный набор контрактов.
   Сгруппировать актуальные broker primitives отдельно от thread pool реализаций:
   `thread/channel/*` для `mailbox/spsc_queue/byte_ring/payload_channel` и
   `thread/pool/*` для `pool/stack_pool/atomic_pool`.

4. Перенести serializer из `libs/aesthetics`.
   Сделать в `utils` отдельный serializer для байтовых дампов C++ агрегатов, а в
   `aesthetics` оставить ECS-специфичные адаптеры, список компонентов и события snapshot.

5. Сгруппировать аллокаторы по сценариям.
   Например `utils/memory/linear`, `utils/memory/pool`, `utils/memory/block`, плюс короткий
   документ "какой аллокатор брать для какого lifetime".

6. Выделить spatial слой.
   `geometry`, `kd_tree`, `grid`, `aabb_tree` уже образуют отдельный модуль. Их можно
   перенести под `utils/spatial/` и покрыть едиными тестами query-контракта.

7. Уменьшить тяжесть частых заголовков.
   `core.h`, `type_traits.h`, `named_serializer.h` тянут много зависимостей. После разбиения
   стоит проверить include graph и сократить transitive includes для горячих библиотек.

8. Добавить/расширить тесты.
   Уже есть тесты для современных thread primitives. Дальше стоит добавить focused-тесты на
   аллокаторы, spatial query, string_pool/hash collision policy, compression roundtrip и новый
   aggregate serializer.

9. Пересмотреть контракт `file_io`.
   Функции чтения/записи и проверки файловой системы лучше перевести на возврат кода ошибки
   или `std::expected`-подобного результата вместо выбрасывания исключений/фатальных ошибок.
   Это сделает инструмент пригоднее для загрузчиков ресурсов, редакторских сценариев и
   фоновых потоков, где ошибка файла должна быть обычным результатом операции.
