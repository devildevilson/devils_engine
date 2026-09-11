# network — рабочая карта

Документ для агента, который правит `libs/network`. Человеку — `README.md`: там подробно объяснены
механизмы и результаты измерений. Архитектурные решения и порядок следующих срезов живут в
`NETWORKING.md`, воспроизводимый журнал — в `NETWORKING_STATUS.md`, общий контекст проекта — в
корневом `AGENTS.md`. Здесь только то, что нужно, чтобы ПРАВИТЬ библиотеку и не размыть её границы.

`network` — не готовый `NetworkManager`. Это набор ограниченных, caller-driven механизмов, которые
проект компонует с симуляцией и транспортом. Живая композиция находится в `NET06`, `NET07` и
`NETLAB01`; не переносить её целиком в библиотеку без отдельного решения о владельце.

---

## 1. Как устроена библиотека

### Слой причин симуляции и восстановления

| Вопрос | Файл |
|---|---|
| собрать записи одного тика, стереть порядок прихода, получить immutable bundle | `tick_journal.h` |
| принять модульный sequence, отличить новый/дубликат/старый/слишком далёкий | `sequence_window.h` |
| хранить строго возрастающие bundle под count+byte budget | `bounded_history.h` |
| хранить и выбирать последний checkpoint не позже тика | `checkpoint_ring.h` |
| восстановить checkpoint K и проиграть каждый bundle K+1..N | `replay.h` |
| канонический state schema API | `state_schema.h` — только aliases к `utils::serial` |
| корень полного состояния и локализация несовпавшей секции | `state_digest.h` |

### Слой репликации

| Вопрос | Файл |
|---|---|
| latest-state gate, baseline store, default key-sorted delta | `replication.h` |
| детерминированный key+fixed-point и turn code | `fixed_point.h` |
| hot upstream: запросы клиента к authority | `intent_wire.h` |
| hot downstream: relevant set, cadence, transform frames, correction threshold | `transform_wire.h` |

### Слой сессии

| Вопрос | Файл |
|---|---|
| content root, compatibility, logical membership, transactional recovery | `session.h` |
| фиксированные handshake bytes и две ordered state machine | `session_wire.h` |
| единственный credential, которым владеет движок: reconnect credential | `credential.h` |
| silence/loss/backoff machine, session holds, feasibility по retention | `reconnect.h` |

### Граница доставки

| Вопрос | Файл |
|---|---|
| детерминированная модель opaque-message доставки и fault injection | `in_memory_link.h` |
| публичная GNS-граница и lifetime-контракты | `gns_transport.h` |
| единственная не-header-only реализация, работа с native GNS | `src/network/gns_transport.cpp` |
| нейтральный umbrella include | `network.h` |

Цели сборки разделены намеренно:

- `devils_engine::network` — `INTERFACE`, C++23 + `utils`, без GNS headers/linkage;
- `devils_engine::network_gns` — опциональный concrete adapter, включается
  `DEVILS_ENGINE_BUILD_NETWORK_GNS`; `gns_transport.h` подключается явно и НЕ входит в `network.h`.

Полезный порядок чтения впервые: `tick_journal.h` -> `bounded_history.h` -> `replay.h` ->
`session.h` -> `session_wire.h` -> `reconnect.h`; затем отдельно `intent_wire.h` и
`transform_wire.h`; GNS читать последним. Такой порядок показывает семантику до механики доставки.

---

## 2. Инварианты

Нарушение любого пункта — дефект, даже если узкий тест зелёный.

1. **Проект владеет смыслом.** Библиотека не знает ECS, gameplay verb, длительность тика,
   ownership/legality intent, выбор authority и постоянную client/server topology. Всё это приходит
   типами и policy/callback seams.
2. **Транспорт владеет только доставкой opaque bytes.** `session`, `reconnect`, replay и hot codecs
   не открывают socket и не зовут GNS. `gns_transport` не аутентифицирует, не восстанавливает мир и
   не выбирает authority.
3. **`gns_peer` не identity.** Native handle и generational `gns_peer` локальны одному transport.
   Principal, logical peer, session и authority epoch переживают transport reconnect и не выводятся
   из GNS handle.
4. **Один owner, явный drive.** Core-структуры и GNS adapter сами не заводят worker. Время, вызовы
   `advance`/`pump`/`poll`, clock instants, nonces и randomness предоставляет caller. Не прятать
   clock, RNG или event loop в библиотеке.
5. **Ожидаемый отказ — значение.** Hostile input, capacity, stale/duplicate, malformed bytes и
   backend refusal возвращают именованный status и оставляют оговорённое состояние неизменным.
   `utils::error` — только для нарушения lifecycle/programmer contract или исчерпания пространства,
   которое невозможно безопасно продолжить.
6. **Сначала проверить, затем публиковать.** Decode, delta materialization и recovery работают через
   detached candidate/staging. Единственное изменение live state происходит после всех проверок;
   `recover_session` требует один `noexcept` publish.
7. **Classification не равна commit.** `sequence_window`, state/transform frame gates и похожие
   механизмы сначала классифицируют без мутации. Commit разрешён только после decode, validation и
   materialization.
8. **Канонический вход проверяется, а не только производится.** Exact length, отсутствие trailing
   bytes, zero reserved/absent fields, named enum values, sorted unique keys/sections/layouts — часть
   формата. Не принимать «понятный префикс» и не нормализовать двусмысленные bytes молча.
9. **Бюджет объявлен владельцем механизма.** Count и logical-byte budgets независимы; zero —
   осмысленная граница. Нельзя выводить protocol budget из RAM/устройства одной машины. Peer-controlled
   container не растёт по требованию удалённой стороны.
10. **Prepared API не растёт.** `try_encode`/`try_write`/`*_into`, receive output и GNS slabs работают
    в заранее подготовленной capacity. На отказе не публикуется partial result. Allocating convenience
    API остаются явно отдельными.
11. **Borrow остаётся borrow.** Decoded `span` указывает в receive buffer; link callback — только на
    время callback; history views живут до следующей мутации; `gns_received_message::payload()` — до
    `reset`/destruction. Не сохранять эти ссылки без копии или переноса owning handle.
12. **Bundle тика immutable и существует даже пустым.** Checkpoint K означает committed state ПОСЛЕ
    K; replay требует каждый K+1..N, включая пустые тики. Packet loss/arrival order не может менять
    логическую историю.
13. **Tick и sequence — разные порядки.** History/checkpoints используют обычный strict order.
    Модульный wrap и half-range ambiguity принадлежат только sequence windows. Не применять modular
    сравнение к simulation tick.
14. **Три вида hash не смешиваются.** Schema/layout fingerprint — короткая compatibility metadata;
    state digest — корень канонического causal state; credential MAC — authentication policy.
    Ни Murmur root, ни content hash не превращаются в доказательство личности.
15. **Digest считается по каноническому состоянию, не по wire-квантизации.** Quantization уменьшает
    трафик, но не стирает divergence. `state_digest` и checkpoint roots остаются на полных canonical
    bytes.
16. **Handshake упорядочен и terminal.** Compatibility проверяется ДО identity policy. Сообщение не
    своей фазы — `unexpected_message`; отказ завершает exchange. Выбранная envelope version закреплена
    на весь exchange, а поддержка старых версий — явный ограниченный диапазон с тестом, не правило
    `version <= current`.
17. **Wire width фиксирован только на wire.** Нейтральные session templates не угадывают размеры
    project ID, но `session_wire.h` использует точные integer widths/endianness. Изменение bytes — это
    версия протокола и compatibility matrix, а не локальный refactor.
18. **Reconnect — не transport retry внутри адаптера.** Два silence budget: suspect и lost; deadline
    равен `ticket.expires_at`; проверяется до расхода attempt; backoff детерминирован и без jitter.
    Старый traffic может оживить `suspect`/необработанный `lost`, но не `attempting` и дальше.
    `history_gap` ведёт к нормальному fresh rejoin.
19. **Engine credential только reconnect.** Join identity остаётся injected policy. Ticket MAC
    доказывает entitlement, presentation MAC — владение secret в этом transcript. Session secret
    выводится, не хранится; check order и constant-time compare не менять. Reissue не отзывает старый
    ticket до expiry; key storage/rotation здесь отсутствуют.
20. **Hot upstream не доверяет actor от клиента.** `network::intent` не равен project intent и не
    несёт session/peer/acting entity/provenance. Project translation seam добавляет их и проверяет
    ownership, legality и rate.
21. **Relevant set — disclosure boundary.** Downstream frame адресует только per-client dense slot.
    Membership update идёт reliable ordered, transform frame — unreliable sequenced; set generation
    обязана закрывать обе гонки между lanes. Shape fingerprint не включает adaptive cadence/budget.
22. **GNS release не ACK.** `poll_send_releases` сообщает возврат памяти, а не доставку. Accepted send
    может освободиться после disconnect недоставленным; gameplay acknowledgement живёт выше.
23. **GNS lifetime шире объекта adapter.** Borrowed GNS runtime живёт дольше transport И всех receive
    leases. Send callback удерживает slab, не `gns_transport*`. Один `gns_dispatcher` на native
    interface владеет ВСЕМИ `RunCallbacks` через `pump()` и переживает зарегистрированные transports.
24. **Не достраивать отсутствующего владельца по догадке.** Automatic orchestration
    connect->handshake->credential->checkpoint transfer->replay пока остаётся у consumer/лаборатории.
    Новый общий runtime появляется только после того, как его ownership и budgets названы отдельным
    срезом.

---

## 3. Рецепты правок

### Добавить нейтральный механизм

1. Назвать ровно одну ответственность и её non-goals. Если типу понадобились socket, ECS и clock
   одновременно, граница уже неверна.
2. Типы проекта передавать template/policy/callback, но не ослаблять contract неявными conversions.
3. Все peer-controlled collections снабдить count/byte/work budget; подготовку отделить от hot path.
4. Для обычных отказов завести enum status. Доказать тестом, что output/live state не изменились.
5. Если механизм отдаёт view/span/reference, рядом записать точный момент его инвалидирования.
6. Добавить header в `network.h`, только если он не тянет GNS или другую concrete dependency.
7. Добавить узкий doctest в `subprojects/tests` и связать его с `devils_engine::network`.

### Изменить wire format

1. Сначала выписать старые canonical bytes и решить: grammar совместима или breaking. Не менять
   константу версии как косметику.
2. Обновить encoder, decoder, enum bounds, exact-size/budget checks и state machine, которая выбирает
   версию. Encode и decode обязаны отвергать одну и ту же нелегальную форму.
3. Старый decoder сохраняется только для явно объявленных версий. Тестировать current success,
   каждую retained version и ближайшую breaking/unknown version.
4. Добавить malformed tests: truncation каждого поля, trailing bytes, unknown type/enum, nonzero
   reserved/absent values, oversized lengths, insufficient output capacity и смена версии в середине
   exchange.
5. Проверить borrowed payload lifetime и aliasing входного/выходного буфера. Handshake transcript
   должен поглощать ровно те bytes и в том порядке, которые реально отправлялись.
6. Обновить `README.md`, `NETWORKING.md`/`NETWORKING_STATUS.md`, если изменился контракт или его
   воспроизводимая матрица, а не только реализация.

### Изменить session/recovery/credential

1. Сначала проверить различие transport peer / logical peer / principal / authority epoch.
2. Compatibility refusal обязан произойти до вызова identity/crypto policy; закрепить это counting
   policy в тесте.
3. Recovery preflight проверяет всю K+1..N историю до restore; replay идёт в detached candidate с
   presentation suppressed; root проверяется в K и N до единственного publish.
4. Для credential сначала дешёвые declared-field/time проверки, затем ticket MAC, затем derivation и
   presentation MAC. Не добавлять clock read, secret table или concrete crypto primitive.
5. Для reconnect не смешивать coordinator, `session_hold_table`, `assess_recovery` и реальные
   transport calls: это четыре разных ответственности.

### Изменить GNS adapter

1. Читать `gns_transport.h` как lifetime contract, затем соседний тест, и только после этого `.cpp`.
2. Не добавлять GNS include/linkage в `devils_engine::network` или umbrella header.
3. Не использовать raw `RunCallbacks` рядом с dispatcher и не заводить второй dispatcher для одного
   interface. Callback не должен удерживать object pointer после unregister/destruction.
4. Сохранять разделение prepared slab budget, native backend queue budget, receive lease budget и
   caller work/output budget. Эти числа измеряют разные владения.
5. `receive` может вернуть non-OK вместе с уже полученными `count` messages — caller обязан суметь их
   обработать/освободить. Малый event output не должен терять ещё не сообщённое текущее состояние.
6. Endpoint reconnect — новый `connect` и новая generation; session resume выполняется выше.
7. Проверять не только internal pipe, но и real UDP localhost: lifecycle, refusal, lanes, loss,
   fragmentation, destruction with outstanding leases и port/bind behavior.

---

## 4. Проверка

Собирать из корня репозитория. Один изменённый механизм сначала проверяется своей целью:

```sh
cmake --build build-debug -j 8 --target network_session_wire_test
ctest --test-dir build-debug -R '^network_session_wire_test::' --output-on-failure
```

| Что меняли | Минимальные цели/тесты |
|---|---|
| journal/history/ownership | `network_tick_journal_test`, `network_sequence_history_test`, `network_hot_path_test` |
| schema/checkpoint/replay/digest | `utils_serialization_test`, `network_state_schema_test`, `network_checkpoint_replay_test`, `network_state_digest_test`, `network_session_test` |
| baseline/delta | `network_replication_test`, `network_hot_path_test`, `NET07_replication_baselines` |
| fixed point / upstream hot bytes | `network_intent_wire_test`, `network_backend_session_test` |
| relevant set / downstream hot bytes | `network_transform_wire_test`, `NETLAB01_multi_process` |
| session / wire / credential / reconnect | `network_session_test`, `network_session_wire_test`, `network_credential_test`, `network_reconnect_test`, `NETLAB01_multi_process` |
| in-memory delivery | `network_hot_path_test`, `network_backend_session_test`, `NET06_in_memory_transport` |
| GNS adapter | `gamenetworking_sockets_capability_test`, `network_gns_transport_test`, `network_backend_session_test`, `NETLAB01_multi_process` |

Фокусный прогон после библиотечной правки:

```sh
ctest --test-dir build-debug -R 'network|NET0|NETLAB' --output-on-failure -j 4
```

Перед закрытием изменения, затрагивающего canonical bytes, lifetime, concurrency boundary или GNS,
повторить соответствующий прогон в `build-release`. Текущие ожидаемые количества тестов не копировать
сюда: они меняются; источник правды — `NETWORKING_STATUS.md` и `ctest -N`.

`network_native_float_cross_compiler_probe` на Linux сравнивает GCC/Clang, но оба могут использовать
один glibc libm. Его равенство не доказывает cross-libm детерминизм. Real-UDP тесты доказывают GNS
boundary на localhost; in-memory тесты — детерминированную модель, а не внутренности GNS.

---

## 5. Ловушки

- `state_schema.h` больше не владеет сериализацией. Исправление canonical reader/writer/section
  composition обычно принадлежит `libs/utils`, после чего нужны и utils-, и network-тесты.
- `network.h` специально не включает `gns_transport.h`. Не считать это пропуском.
- `bounded_history::entries()` — view, возвращаемый по значению; брать его заново после мутации.
  Адрес конкретного slot стабилен только до eviction/clear/destruction.
- `tick_journal::consume()` передаёт ownership. Recycle разрешён только после ухода bundle из всех
  immutable histories.
- Logical byte size задаёт owner; это не `sizeof(T)` и не allocator capacity.
- Full checkpoint и replication baseline — разные вещи. Baseline может быть дельтой представления и
  не обязан содержать все причины следующего тика.
- `make_state_digest` над live host может аллоцировать; prepared путь — canonical bytes плюс
  `try_murmur64_digest`. Section roots — диагностика, не отдельная истина.
- Для float equality default `operator==` может расходиться с canonical bytes (`-0`, NaN payload).
  Использовать fieldwise policy, согласованную с сериализацией; не сравнивать padded struct через
  raw bytes.
- Unreliable-sequenced GNS filtering не заменяет application frame gate: native message number и
  application sequence/tick — разные пространства.
- `poll_connections` — снимок наблюдённых изменений, не lossless callback log. Terminal peer живёт
  до явного `close`.
- `close_listener` инвалидирует listener и всех его child peer IDs. `shutdown` final и idempotent;
  новые операции после него не разрешены, но outstanding leases/releases сохраняют контракт.
- GNS `AllocateMessage(0)` всё равно аллоцирует native header. Prepared payload slab не означает
  нулевые allocation внутри third-party library.
- Нельзя судить о reconnect feasibility только по размеру history: checkpoint cadence и наличие
  первого K+1 bundle работают вместе.
- Fresh join после `history_gap` — штатный результат, не повод увеличить budget автоматически.
- Длинные объяснения WHY оставлять рядом с механизмом и синхронизировать с `README.md`; этот файл
  должен оставаться картой правки, а не вторым журналом кампании.

---

## 6. Живые потребители и источник истины

- `subprojects/playgrounds/NET06_in_memory_transport` — late authoritative input,
  checkpoint/replay/digest через детерминированную доставку.
- `subprojects/playgrounds/NET07_replication_baselines` — baseline/delta/recovery session.
- `subprojects/playgrounds/NETLAB01_multi_process` — реальная композиция session wire, reconnect,
  intent/transform hot paths и GNS между процессами; fixture code здесь остаётся project policy.
- `subprojects/tests/network_backend_session_test.cpp` — один и тот же session contract через
  in-memory boundary и GNS.
- `subprojects/tests/gamenetworking_sockets_capability_test.cpp` — исследование самой pinned GNS;
  это не публичный adapter contract.

Если README, комментарий и тест расходятся, не выбирать удобную версию. Сначала найти, какой контракт
зафиксирован воспроизводимым тестом и `NETWORKING_STATUS.md`; если расходится сам тест, исправлять
ожидание только после проверки направления данных, ownership и порядка события — здесь уже был случай,
когда неверным оказался тест epoch orientation, а код был прав.
