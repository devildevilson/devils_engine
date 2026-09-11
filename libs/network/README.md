# network

Каркас сетевой сессии для детерминированной tick-based симуляции: идентичность участников,
каноническая история входов, checkpoints и replay, регулярная репликация состояния и ограниченная
граница доставки сообщений.

Библиотека не описывает игру и не выбирает её сетевую топологию. Она описывает, **как две установки
договариваются об одной сессии, как authority превращает предложения участников в единственную
историю и как другая сторона доказывает, что восстановила то же состояние**. Значение intent,
соответствие principal игровой сущности, legality, interest management и содержимое полного
checkpoint принадлежат проекту.

В основе лежат три различия:

| Не одно и то же | Почему |
|---|---|
| transport connection и session | connection исчезает при обрыве; логическая session и principal переживают reconnect |
| intent и canonical bundle | intent — предложение; bundle — то, что authority действительно приняла и применила |
| causal state и transform frame | causal state определяет следующий tick; transform frame — необязательная latest-value репликация |

**Цели сборки** разделены по зависимостям:

| Цель | Что внутри | Зависимости |
|---|---|---|
| `devils_engine::network` | нейтральные templates, session/wire codecs, history, replay, replication | C++23, `utils` |
| `devils_engine::network_gns` | concrete adapter GameNetworkingSockets | `network`, GNS |

Основная цель header-only и не тянет GNS. Адаптер подключается явно через
`<devils_engine/network/gns_transport.h>`; `network.h` остаётся нейтральным umbrella header.

---

## Как этим пользоваться

Задача -> механизм -> результат.

**Открыть новую сессию.** Поднять transport connection, провести `client_hello ->
authority_challenge -> client_response`, проверить compatibility до identity и только затем создать
logical membership. Transport peer после этого остаётся лишь текущим маршрутом к уже известному
principal.

**Принять ввод игрока.** Декодировать `intent_batch`, вывести principal и acting entity из
аутентифицированной session, проверить tick/ownership/legality/rate и передать допущенные записи в
`tick_journal`. В конце тика journal стирает порядок прихода и выдаёт immutable canonical bundle.

**Воспроизвести ту же реальность на follower.** Получать каждый canonical bundle по reliable ordered
каналу, применять его перед соответствующим simulation step и хранить roots состояния по тикам.
Пустой tick тоже приезжает пустым bundle: отсутствие сообщения не может означать «ничего не
произошло».

**Показать актуальные сущности без полного state frame каждый tick.** Authority ведёт отдельный
`relevant_set` для каждой session, надёжно публикует его изменения и отправляет quantized transform
frames по latest-value каналу. Follower читает frame только против той generation набора, для которой
он был построен.

**Вернуться после обрыва.** Создать новую transport connection, предъявить reconnect ticket вместе с
proof-of-possession для нового handshake transcript, получить checkpoint `K` и bundles `K+1..N`,
восстановить detached candidate, проверить roots в `K` и `N` и одним `noexcept` publish заменить live
state.

**Проверить session без настоящей сети.** Подать те же opaque messages в `in_memory_link`, явно
двигать transport time через `advance()` и инъецировать loss, delay, duplication и reorder. Эта
модель проверяет видимый приложению delivery contract, но не симулирует UDP, ACK, MTU или congestion
control.

---

## Общая модель

```text
                                   ┌──────────────────────┐
 local/UI/AI intents ─────────────>│                      │
 remote intent proposals ─────────>│ authoritative tick   │
                                   │                      │
                                   │ validate → seal      │
                                   │ apply → step         │
                                   └──────────┬───────────┘
                                              │
                         ┌────────────────────┼────────────────────┐
                         ▼                    ▼                    ▼
                 canonical bundle       state root          checkpoint
                         │                    │                    │
                         └────────────── history ─────────────────┘
                                              │
                                              ▼
                                      recovery / replay

 presentation state ── relevant set ── transform frames ──> client presentation
```

Canonical bundle и checkpoint относятся к причинной timeline. Transform frames идут рядом: они
могут уточнить представление или prediction, но потеря отдельного frame не должна делать следующий
simulation tick неопределённым.

Session может назначить один peer authority, однако сами transport endpoints остаются peers. Смена
authority — отдельный протокол election/epoch, а не свойство connection.

---

## Верхний уровень интеграции

Сеть тесно связана с границей simulation tick, поэтому ожидаемый владелец session orchestration
работает в main thread. Он вызывает transport pump каждый frame, но меняет causal state только в
объявленной фазе fixed-step loop.

У этого слоя одна ответственность: превратить временные transport observations и bytes в
tick-addressed, проверенные данные для симуляции — и обратно. Концептуально он выдаёт не callbacks, а
несколько типизированных результатов:

| Результат | Кто читает | Семантика |
|---|---|---|
| session events | app/UI | connected, suspect, lost, admitted, refused, recovered |
| remote intent proposals | authority simulation | вход для gameplay validation будущего тика |
| authoritative bundles | follower simulation | причинная история, reliable и непрерывная |
| relevant-set/transform updates | prediction/presentation | latest-value, допускает потерю |
| ready recovery | main simulation | цельная транзакция: plan + owned checkpoint + bundles |

Checkpoint chunks не публикуются main loop по одному. Пока не собран весь документ и весь replay
range, recovery не существует как операция над симуляцией.

Ожидаемый порядок main loop:

```cpp
network.pump(now);                         // callbacks, receive, decode, state machines

fixed_step.consume_elapsed([&](tick_id tick) {
  apply_ready_recovery();                  // только цельная проверенная транзакция
  consume_authoritative_updates();         // bundle/reconciliation на границе тика
  admit_remote_intents();                  // principal -> actor, legality, rate

  const auto bundle = seal_tick(tick);     // local/UI/AI/network в одном порядке
  simulate(bundle);
  commit_state_root_and_checkpoint(tick);

  network.publish_committed_tick(bundle);
  network.publish_transforms();
});

consume_presentation_updates();
network.flush_outgoing();
```

Внутри `pump` обработчики не вызывают gameplay рекурсивно. Они декодируют, меняют session state
machines и складывают данные; main loop забирает их в своей фазе. Так число native callbacks в одном
frame не меняет порядок симуляции.

Отдельный I/O worker имеет смысл только после измерения main-thread pump. Если он когда-нибудь
появится, его граница — bounded SPSC-передача **owned opaque messages**; logical sessions, gameplay
admission, checkpoint publication и replay остаются у main thread.

---

## Session, identity и admission

В модели есть четыре разных имени:

| Имя | Время жизни |
|---|---|
| transport peer | одна connection в одном экземпляре transport |
| logical peer | участие в session |
| principal | аутентифицированный автор действий |
| authority epoch | версия права peer говорить от имени authority |

Ни native handle, ни IP-адрес не становятся principal. Reconnect выдаёт новый transport peer, но
сохраняет session, logical peer и principal. Сообщение authority принимается только при совпадении
session, logical authority и epoch.

### Compatibility до identity

`session_compatibility` сравнивает:

- формат handshake;
- версию протокола;
- content root полной разрешённой установки;
- schema полного causal state;
- schema intent;
- numeric profile.

Content root строится заранее из product/version и полных bytes core, project и mod-файлов в
каноническом load order. Это доказательство одинакового содержимого при принятом предположении о
SHA-256, но не доказательство личности.

Несовместимая установка отклоняется до challenge и до проверки credential. Это одновременно точная
диагностика и граница стоимости: identity policy не работает на peer, с которым симуляция всё равно
невозможна.

### Ordered handshake

```text
client_hello          compatibility + client nonce
authority_challenge   compatibility + authority nonce + opaque challenge
client_response       opaque credential + optional resume claim
session_accepted      logical membership and starting tick
session_refused       одна именованная terminal reason
```

Envelope фиксирует magic, ограниченную поддерживаемую версию, тип, reserved byte и точную длину.
Получатель не принимает понятный префикс с неизвестным хвостом. Выбранная hello-версия закрепляется
на весь exchange; поддержка старых форматов задаётся явным диапазоном, а не правилом «всякая версия
меньше моей совместима».

Transcript хеширует точные hello и challenge bytes с length prefixes. Credential для одного
exchange поэтому нельзя перенести в другой, даже если логические поля совпали. Любой refusal
terminal: повторная попытка начинается с новой connection.

### Join и reconnect — разные credentials

Join credential отвечает на вопрос «кто этот незнакомец?». Его issuer — политика проекта: platform
identity, offline keystore, dedicated-server token и т.п. Библиотека не задаёт его формат.

Reconnect credential authority выпускает сама для уже принятой session. Он содержит два
независимых доказательства:

- `ticket_mac` подтверждает entitlement `(session, principal, epoch, validity window)`;
- `presentation_mac` подтверждает владение session secret в текущем handshake transcript.

Session secret выводится из authority key, session и principal, а не хранится в authority-side
таблице. MAC primitive инъецируется проектом. Библиотека не читает clock: `issued_at`, `expires_at` и
текущий authority instant передаёт caller. Reissue не отзывает старый ticket; до expiry он остаётся
действительным.

---

## Causal timeline

### Intent становится историей только после seal

`network::intent` — wire request, а не проектный `act::intent`. Он намеренно не несёт того, что
authority уже знает или обязана вывести сама: session, principal, acting entity и provenance.

После wire decode проект выполняет semantic admission. Принятые записи одного тика попадают в
`tick_journal`, где физический порядок прихода стирается канонической сортировкой. Семантический
duplicate и две разные записи с одинаковым ordering key — отказ, а не зависимость от stable arrival
order.

`consume()` передаёт owning immutable bundle. Только такой bundle входит в causal history и
рассылается follower-ам. Zero-record bundle остаётся явным тиком.

### History и checkpoints

`bounded_history` хранит строго возрастающие bundles под двумя независимыми бюджетами: количество и
логические bytes. При успешной вставке старые записи вытесняются детерминированно, самые ранние
первыми.

`checkpoint_ring` применяет ту же модель к self-contained project blobs. Библиотека не решает,
содержит blob raw canonical document, compressed container или другую форму, и не считает
`sizeof(T)` его сетевой стоимостью.

Checkpoint в `K` — состояние **после** тика `K`. Для достижения `N` необходим каждый bundle
`K+1..N`, включая пустые. История, начинающаяся с `K+2`, имеет дыру, которую невозможно угадать.

### Полное состояние и roots

Полный causal state состоит из project-owned versioned sections. Порядок объявления секций не
влияет на документ: schema сортирует их по ID и получает одну canonical byte form. Decode строит
detached staging, проверяет каждую секцию и весь host, после чего выполняет единственную замену live
state.

В checkpoint входят все причины следующего тика, например:

- simulation tick и дробный остаток timeline;
- PRNG cursors;
- entity/version counters;
- deferred causal work;
- project-owned world state.

Derived caches и presentation state в него не входят: они пересобираются после публикации.

State digest считается по полным canonical bytes. Section roots нужны для локализации несовпадения,
а не образуют вторую версию состояния. Schema/layout fingerprints описывают совместимость формата;
они не заменяют content root, state digest или authentication MAC.

### Transactional replay

Replay сначала проверяет весь диапазон, затем восстанавливает checkpoint и для каждого тика
`K+1..N` выполняет:

```text
apply canonical bundle T
step simulation T with presentation suppressed
verify state when required
```

Recovery запускается над detached candidate. Root проверяется после restore в `K` и после replay в
`N`; только затем один `noexcept` publish меняет live world. Ошибка decode, пропущенный bundle,
неверный root или отказ project callback не оставляют полувосстановленное состояние опубликованным.

---

## Два разных replay

Восстановление authoritative history и исправление client prediction похожи механически, но меняют
разные вещи.

Сетевое состояние всегда адресовано тиком. Если client уже предсказал `106`, а authority прислала
root для `100`, сравнивается retained local root в `100`, а не текущий state в `106`. Поэтому client
хранит ограниченное окно `(tick, root/checkpoint, local inputs)`: естественное запаздывание сообщения
не является divergence, оно лишь определяет, какую точку прошлого можно подтвердить или откуда
нужно пересчитать будущее.

### Session recovery

```text
authority checkpoint K + authority-sealed bundles K+1..N
  -> точное authoritative state N
```

Sealed bundles уже являются прошлым. Их нельзя повторно валидировать по новым правилам и выбрасывать.
Если такой bundle невозможно применить, recovery отказывается: это несовместимость, неполный
checkpoint или divergence, а не разрешение придумать другое прошлое.

### Prediction reconciliation

```text
authoritative state N + unacknowledged local intents N+1..current
  -> новое predicted future
```

Неподтверждённые локальные intents — ещё не история. После authoritative correction они выполняются
заново и могут перестать быть legal: цель исчезла, ресурс потрачен, действие больше недоступно. Такой
отказ изменяет только предсказанное будущее клиента.

Для reconciliation нужен acknowledged input horizon: клиент обязан знать, какие свои input sequence
authority уже обработала. Без него невозможно отделить подтверждённые inputs от тех, которые следует
переиграть. `state_frame_header` содержит это понятие; полноценный correction wire contract должен
донести его вместе с authoritative anchor.

---

## Регулярная репликация

### Baseline и delta

Replication baseline — complete snapshot выбранного представления, но не обязательно полный causal
checkpoint. `state_frame_header` раздельно несёт simulation tick, application sequence, exact base и
result baseline IDs и acknowledged input sequence.

Delta применяется только к названному retained base. Candidate публикуется под result ID после
успешного project codec; missing base, stale version, duplicate ID и budget overflow ничего не
меняют.

Доступный default codec работает над key-sorted state и versioned values. Create, update и erase
имеют явные preconditions; проект может подставить другую snapshot/delta форму.

### Hot upstream: intent batches

Intent batch адресует base tick один раз и кодирует недавнее окно через маленькие offsets. Повторение
нескольких тиков — часть delivery strategy: потерянный packet заменяется следующим batch без
отдельного retransmit protocol.

Каждый intent kind заранее объявляет форму payload. Registered IDs превращаются в индекс
отсортированного registry; fingerprint этого списка входит в compatibility, поэтому сдвиг индексов
между версиями приводит к отказу session до первого тика.

### Fixed point

Мировая координата делится на integer cell key и code внутри клетки, поэтому разрешение не ухудшается
далеко от нуля. Quantum задаётся отрицательной степенью двойки: умножение IEEE-754 double на степень
двойки точно, а decode->encode возвращает тот же code по построению.

Wire quantization не является средством устранения divergence. Два уже разошедшихся состояния могут
квантизоваться в разные соседние codes; state roots поэтому никогда не считаются по quantized
transform values.

### Hot downstream: relevant set и transforms

Authority ведёт per-session relevant set. Frame адресует сущность плотным slot, а не глобальным
entity ID, поэтому relevance служит не только bandwidth optimization, но и disclosure boundary:
клиенту вообще не сообщается идентификатор сущности вне его набора.

Три решения независимы:

| Решение | Механизм |
|---|---|
| кто видим клиенту | relevant set |
| какие поля едут | transform class layout |
| как часто они едут | cadence policy |

Membership публикуется reliable ordered и имеет generation. Latest-value transform classes обычно
идут unreliable и несут generation, против которой были построены. Frame может обогнать membership
или отстать от уже применённого update; обе гонки отклоняются вместо чтения slot в неверном наборе.
Класс вправе объявить reliable delivery, но это его явная cadence policy, а не скрытое решение
transport.

Latest-value gate ведётся отдельно для каждой cadence class. Dense mode экономит slot indices на
полном последовательном диапазоне; sparse mode перечисляет изменившиеся slots. Потеря frame или
budget refusal допустимы, потому что следующий frame supersedes старый и correctness не зависит от
прихода каждого из них.

Correction threshold выводится из fixed-point quantum и сравнивается в code space. Он не является
придуманным epsilon. Сам transform frame при этом не заменяет authoritative checkpoint/state frame,
необходимый для полного prediction reconciliation.

---

## Доставка и lanes

Библиотечный message — opaque bytes плюс delivery metadata. Транспорт не добавляет session ID, tick,
component schema или snapshot semantics: форматом владеет соответствующий wire codec.

Типичная раскладка:

| Класс | Доставка | Причина |
|---|---|---|
| handshake, admission, canonical bundles, membership | reliable ordered | пропуск меняет смысл последующих сообщений |
| checkpoint chunks | reliable bulk с отдельным бюджетом | большой transfer не блокирует текущую timeline |
| intent batches | unreliable с временной избыточностью | следующее окно повторяет недавние предложения |
| transform frames | unreliable latest-value | новый frame заменяет старый |

Delivery lane и application acceptance — разные уровни. Native unreliable-sequenced filter не
заменяет transform/state frame gate; GNS message number не является simulation tick или application
sequence.

### In-memory link

`in_memory_link` — двухсторонняя single-owner модель с явным transport step. Каждое направление
имеет count, byte и bandwidth budgets. Reliable lane сохраняет порядок и повторяет injected loss;
unreliable lane допускает loss, duplication и reorder согласно injected fault policy.

Trace ограничен отдельным budget и годится для сравнения воспроизводимых прогонов. Он не является
полным packet capture, если достигнут лимит.

### GameNetworkingSockets

`gns_transport` — caller-driven adapter над предоставленными
`ISteamNetworkingSockets`/`ISteamNetworkingUtils`. Он не вызывает global Init/Kill, не создаёт
session worker и не скрывает automatic reconnect.

Один `gns_dispatcher` обслуживает один native interface; все `RunCallbacks` проходят через его
`pump()`. Incoming connection сначала резервирует transport peer и сообщает `needs_accept`, после
чего caller явно выбирает `accept` или `close`.

Send копирует payload в заранее подготовленный per-lane slab. Возврат из `poll_send_releases`
означает освобождение памяти GNS, а не delivery ACK. Receive возвращает move-only lease без копии;
payload span живёт до `reset`/destruction lease, а GNS runtime обязан пережить все outstanding
leases.

Reconnect всегда создаёт новую connection и новую peer generation. Восстановление logical session
выполняют handshake, credential и recovery уровнем выше.

---

## Бюджеты, владение и горячий путь

Удалённая сторона не должна заставлять engine container расти. Поэтому независимо объявляются:

- число и logical bytes retained bundles/checkpoints/baselines;
- capacity journal одного тика;
- output capacity декодеров;
- число и bytes сообщений в delivery queues;
- send slots и bytes каждой GNS lane;
- outstanding receive leases;
- transport callback/receive work budget;
- session hold и handshake capacity.

Prepared API (`try_write`, `try_encode`, `*_into`, receive into span) не увеличивает outer storage во
время работы. Allocating convenience API остаётся отдельным. Budget отказ возвращает status и не
публикует partial document, delta, baseline или live state.

Borrowed views имеют короткий явный lifetime: decoded span — пока неизменен receive buffer; history
view — до следующей мутации; in-memory consume value — до возврата callback; GNS payload — до
освобождения lease. Всё, что пересекает frame, tick или thread boundary, должно владеть bytes либо
переносить owning handle.

---

## Решения списком

- Session orchestration принадлежит main thread; transport pump может идти каждый rendered frame,
  causal integration — только на fixed tick boundary.
- Authority определяет canonical history; physical arrival order никогда не становится gameplay
  order.
- Connection, logical peer и principal — разные идентичности.
- Compatibility проверяется до identity; reconnect credential — до ответа retention table, который
  мог бы сообщить постороннему, существует ли удерживаемая session.
- Clock, nonce generation, join identity provider и concrete MAC primitive инъецируются.
- Checkpoint — полный causal state после тика; baseline — полное состояние только выбранной
  replication projection.
- Empty tick — явный bundle.
- State replacement и session recovery транзакционны.
- Presentation подавляется во время authoritative replay.
- Unacknowledged local intent может быть отклонён при prediction replay; authority-sealed intent —
  уже история и не переоценивается.
- Relevant set — per-client disclosure boundary.
- Quantization уменьшает traffic, но не доказывает равенство состояния.
- Backpressure принадлежит каждому классу отдельно: потеря transform допустима, потеря canonical
  bundle — session fault.
- Никакой transport completion не считается gameplay acknowledgement без отдельного протокола.

---

## Чего пока нет

- общего project-facing `network_session_system`, компонующего transport, handshake, tick integration
  и recovery в main loop;
- production wire formats для canonical bundle, state frame, recovery plan и checkpoint transfer;
- полного client prediction reconciliation с acknowledged input horizon;
- project identity provider, authority key storage/rotation и ticket revocation до expiry;
- authority election/migration protocol;
- mid-session fresh join с initial authoritative state transfer;
- compression/download scheduler для больших checkpoints;
- реальной gameplay correction/interpolation поверх transform frames;
- public-network hardening, discovery, relay/P2P signaling и operational server layer.

Эти границы не заполняются скрытой политикой внутри низкого уровня. Новый механизм появляется тогда,
когда названы его owner, budgets, wire identity, точка публикации в main loop и поведение при отказе.
