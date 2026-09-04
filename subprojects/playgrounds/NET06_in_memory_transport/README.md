# NET06 — deterministic in-memory transport

Первая площадка сетевой сессии. Она запускает два независимых fake simulation host и передаёт между
ними непрозрачные сообщения без сокетов и GameNetworkingSockets. Её задача — сделать задержку, loss,
reorder, duplicate, bandwidth, backpressure и reconnect полностью воспроизводимыми до подключения
настоящего транспорта.

## Вопрос площадки

Можно ли описать логические гарантии transport/session boundary независимо от GNS и проекта так, чтобы
один записанный fault schedule всегда создавал тот же packet trace и тот же итог симуляции?

Площадка проверяет reusable-шаблон `network::in_memory_link<Message, SizeOf, FaultPolicy>`. Шаблон знает
только непрозрачный тип сообщения, его логический размер и результат fault-policy. Intents, state frames,
игроки, ECS и сериализация существуют только в fixture площадки.

## Запуск

```bash
cmake --build build-debug -j4 --target NET06_in_memory_transport
./build-debug/subprojects/playgrounds/NET06_in_memory_transport/bin/NET06_in_memory_transport --verify

cmake --build build-release -j4 --target NET06_in_memory_transport
ctest --test-dir build-release -R '^NET06_in_memory_transport_verify$' --output-on-failure
```

`--trace` дополнительно печатает воспроизводимый transport trace основного сценария.

## Первый срез

- две независимые очереди направлений и 256 логических lanes;
- reliable ordered: повторная передача после loss, ровно одна доставка и порядок внутри lane;
- unreliable: допустимые loss, duplicate и reorder;
- меньший номер lane получает bandwidth раньше, поэтому intents не стоят за bulk;
- отдельные count/byte budgets на каждое направление и явные статусы отказа;
- disconnect уничтожает queued/in-flight/inbox данные, reconnect создаёт новый epoch;
- trace фиксирует acceptance, передачу байтов, fault, retry, scheduling и delivery;
- повтор одного scripted fault schedule сравнивается событие в событие.

Основной fixture отправляет десять reliable intent bundles и пять unreliable state frames. Один intent
теряется на первой попытке и восстанавливается retry; state frames приходят с перестановкой, дублем и
одной окончательной потерей. Follower применяет intents строго по tick и в конце получает тот же causal
state, что uninterrupted authority. State-frame sequence acceptance отдельно подтверждает duplicate и
устаревший после reorder кадр.

## Не входит

- ACK/packet/MTU моделирование и попытка повторить внутренности GNS;
- wire serialization и fragmentation;
- baseline/delta replication — это NET-07;
- настоящий GNS adapter и networking thread — это NET-08;
- handshake, authority, authentication и reconnect recovery policy.

In-memory link моделирует наблюдаемые обещания будущего backend, а не его внутренний протокол. NET-08
должен будет прогнать ту же session fixture через GNS, не меняя simulation/state code.
