# catalogue

`libs/catalogue` - ранний эксперимент с абстрактной оберткой над функциями,
прежде всего над mutating effect-функциями. Идея библиотеки - уметь
перехватывать вызовы, складывать их в бинарный поток, отдавать этот поток
потребителям и потенциально проигрывать его обратно.

Сейчас это не готовая система нетворкинга и не основной источник данных для
multiplayer. Более вероятная будущая роль `catalogue` - утилитарный слой:

- полное логгирование изменений состояния entity для debug и проверки скриптов;
- компактный RPC слой для небольшого числа функций;
- dry-run/preview режим, где effect-функции не меняют выбранные области движка;
- replay/demo/debug запись выбранных каналов.

Основной netcode, скорее всего, должен строиться вокруг input пользователя,
intent-потока или полного world state snapshot, а не вокруг полной записи всех
функциональных вызовов.

## Текущий Статус

Библиотека находится в состоянии прототипа. Внутри есть две линии кода:

- более старый черновик в `core.h`;
- более разнесенный API в `common.h`, `registry.h`, `channel_data.h`,
  `rpc_function.h`, `demo.h`.

Они местами дублируют понятия (`registry`, `buffer`,
`function_buffer_header`). Поэтому `catalogue` сейчас лучше воспринимать как
дизайн-площадку, а не стабильный API.

Внешних полноценных потребителей пока нет. `libs/act` уже содержит
`effect_sink` с комментарием, что реальная реализация может переехать в
`catalogue`; `tile_frontier` пока мутирует world напрямую и отмечает, что
`effect_sink/catalogue` будет подключен позже.

## Buffer Model

Базовая единица данных - `catalogue::buffer`.

Он состоит из:

- `headers` - массив `function_buffer_header`;
- `payload` - общий массив байт.

`function_buffer_header` сейчас содержит:

```cpp
struct function_buffer_header {
  uint32_t tick;
  uint32_t id;
  uint32_t offset;
};
```

Смысл:

- `tick` - игровой тик, к которому относится вызов;
- `id` - id функции;
- `offset` - смещение payload этой функции внутри общего byte buffer.

Идея простая: за tick собирается пачка вызовов, headers описывают порядок и
функции, payload хранит сериализованные аргументы.

Пока формат не финализирован:

- нет общего tick-buffer header в активной линии API;
- нет checksum;
- нет versioning;
- нет clear ownership contract для payload lifetime;
- нет стабильного формата на диск.

## Registry

`catalogue::registry` хранит описание функций по id.

Запись:

```cpp
struct registry::info {
  size_t id;
  std::string_view name;
  invoke_fn fn;
};
```

`invoke_fn` сейчас имеет вид:

```cpp
void (*)(const function_buffer_header&, std::span<uint8_t>);
```

Реестр нужен, чтобы по `function_buffer_header::id` найти функцию-декодер и
проиграть payload обратно.

В текущей реализации registry - это `gtl::flat_hash_map<size_t, info>`. Он не
является общей gameplay-function таблицей. Это отдельная таблица для
catalogue/RPC replay, а общий контракт gameplay-функций сейчас живет в
`libs/act`.

## Channels

`channel_data<id>` - статический канал записи.

В коде уже выделены channel ids:

- `INPUT_CHANNEL_ID`;
- `MUTATOR_CHANNEL_ID`;
- `META_CHANNEL_ID`;
- `SERVICE_CHANNEL_ID`.

И aliases:

- `input_channed_data`;
- `mutator_channed_data`;
- `meta_channed_data`;
- `service_channed_data`.

Каждый канал хранит:

- `registry*`;
- текущий `buffer`;
- до 8 `consumer*`.

API канала:

- `init(registry*)`;
- `add_consumer(consumer*)`;
- `consume()`;
- `clear_buffer()`.

`consume()` просто передает текущий buffer всем зарегистрированным consumers.
Это не очередь и не owning pipeline. Потребитель сам решает, копировать ли
данные, писать ли на диск, отправлять ли по сети или анализировать сразу.

## Consumers

`consumer` - интерфейс приемника buffer:

```cpp
class consumer {
public:
  virtual ~consumer() noexcept = default;
  virtual void consume(const buffer&) = 0;
};
```

Идея в том, что один и тот же канал может одновременно кормить несколько
потребителей:

- demo recorder;
- debug logger;
- network sender;
- script audit tool;
- statistics collector.

Пока есть только demo consumer prototype.

## RPC Function Wrapper

`rpc_function` - шаблонная обертка над свободной функцией.

Идея wrapper'а:

- `call(args...)` - вызвать оригинальную функцию;
- `write(args...)` - сериализовать вызов в channel buffer;
- `log(args...)` - вызвать функцию и записать вызов;
- `read(header, bytes)` - прочитать payload и вызвать функцию;
- `reg()` - зарегистрировать reader в channel registry.

Id функции считается compile-time из имени:

```cpp
static constexpr uint32_t id = utils::murmur_hash3_32(name);
```

Для сериализации черновик использует `zpp_bits` в network endian.

Важно: текущий `rpc_function.h` выглядит как активный прототип и местами
рассинхронизирован с более старым `core.h`. Его нужно стабилизировать перед
использованием как public API.

## Demo Prototype

`demo` - проба потребителя для записи входных и mutator каналов.

Он содержит:

- `input_consumer`;
- `mutator_consumer`;
- `storage.input_buffer`;
- `storage.mutator_buffer`.

При создании `demo(consume_type::all)` он подписывает consumers на input и
mutator channels. Consumers просто копируют полученный `buffer` в storage.

`write_to_disk` и `load_from_disk` пока возвращают `false`: файловый формат demo
не реализован.

## Связь С act

`libs/act` уже содержит близкое понятие `effect_sink`.

`effect_sink` принимает:

```cpp
emit(effect_id, args)
```

и задуман как способ отделить mutating effect от прямой мутации мира. В будущем
реализация такого sink может жить в `catalogue`:

- dry-run sink ничего не применяет, только описывает возможный эффект;
- logging sink пишет вызов в mutator channel;
- replay sink читает channel buffer и применяет функции в порядке записи;
- audit sink сверяет скриптовые изменения с ожидаемой областью доступа.

Пока этот мост не реализован. В `tile_frontier` эффекты actor simulation еще
мутируют `aesthetics::world` напрямую.

## Возможные Роли

### Debug/Audit Logging

Самая практичная роль: записывать mutating calls и изменения entity state, чтобы
ловить ошибки скриптов.

Примеры вопросов, на которые такой лог должен отвечать:

- какая функция изменила компонент;
- в каком tick это произошло;
- какой actor/script/source_action был причиной;
- какие аргументы были переданы;
- была ли функция разрешена для этой области мира.

### RPC Layer

`catalogue` может быть компактным RPC helper'ом для небольшого числа функций:

- HTTP/service methods;
- matchmaking/control plane;
- debug commands;
- небольшие engine service calls.

Это не обязательно должно быть связано с gameplay netcode.

### Dry-Run

Dry-run нужен для скриптов и UI-preview:

- прогнать effect-функции без мутации мира;
- проверить, какие вызовы они хотели сделать;
- разрешить/запретить только часть областей движка;
- показать пользователю последствия.

В этом направлении `catalogue` должен сблизиться с `act::effect_sink`.

## Что Уже Умеет

На данный момент `libs/catalogue` умеет на уровне прототипа:

- описывать buffer из headers + payload;
- описывать consumer interface;
- хранить registry function readers по id;
- иметь несколько статических channels;
- добавлять consumers к channel;
- передавать текущий buffer всем consumers;
- очищать channel buffer;
- хранить demo storage для input/mutator buffers;
- вычислять compile-time id функции по имени;
- набрасывать wrapper-идею `call/write/log/read/reg` для free functions.

## Что Еще Не Сделано

Основной техдолг:

- выбрать один API и убрать дублирование между `core.h` и новым набором
  headers;
- стабилизировать `rpc_function` templates и привести сигнатуры к
  компилируемому public contract;
- определить точный binary format: tick headers, payload sizes, checksum,
  versioning, endian, alignment;
- реализовать запись/чтение demo на диск;
- решить, где живет real `effect_sink`: в `act`, `catalogue` или отдельном
  replay/audit слое;
- связать `act::value` и catalogue payload format;
- добавить dry-run sinks и policy для разрешенных областей мутации;
- добавить replay executor: пройти buffer headers, найти reader в registry и
  вызвать функции;
- добавить tests для registry/channel/demo/rpc wrapper;
- определить, какие данные вообще стоит писать для debug, RPC, demo и
  networking, чтобы не смешивать разные задачи в одном канале;
- исправить мелкие артефакты текущего прототипа вроде include guard в
  `demo.h`.

Граница библиотеки сейчас должна оставаться узкой: `catalogue` - это не
основной state storage и не основной netcode. Это utility layer для записи,
анализа, dry-run и ограниченного RPC вокруг выбранных функций.
