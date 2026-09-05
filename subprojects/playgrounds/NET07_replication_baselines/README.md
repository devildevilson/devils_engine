# NET07 — replication baselines and deltas

Независимая площадка репликации поверх непрозрачной доставки NET06. Она не знает ECS, компонентов,
transform, interest management или wire codec: fake-проект сам определяет ключ, значение, версию и
полный набор реплицируемых сущностей.

## Вопрос площадки

Можно ли после потери и перестановки unreliable delta прийти к актуальному набору сущностей с помощью
явно адресованных baseline’ов, не применяя delta к «похожему» состоянию и не загружая world checkpoint?

Площадка компонует четыре независимых generic-примитива:

- `state_frame_header` разделяет simulation tick, application sequence, base/result baseline ID и ACK input;
- `state_frame_window` принимает только новую совместимую latest-state последовательность;
- `baseline_store` ограничивает immutable snapshots по количеству и логическому wire size;
- `try_materialize_delta` вызывает project codec над точной базой и публикует только готовый candidate.

Для канонического key-sorted состояния библиотека также предлагает `make_keyed_delta` и
`apply_keyed_delta`. Это необязательный codec: проект может подставить любую функцию
`(const Snapshot&, const Delta&) -> optional<Snapshot>`.

## Сценарий

Authority последовательно строит состояния `100..105`. In-memory transport:

- теряет delta `100 -> 101`;
- доставляет `101 -> 102`, который follower явно отклоняет из-за отсутствующей базы;
- передаёт reliable full replication baseline `102` по запросу follower;
- задерживает `102 -> 103` за `103 -> 104`, вызывая второй запрос baseline;
- после установки full baseline `104` классифицирует опоздавший кадр как stale;
- дублирует `104 -> 105`, из двух копий применяется ровно одна.

Итоговый fake entity set совпадает с authority. Full replication baseline содержит только состояние
данного replication stream; это не causal checkpoint мира и не заменяет NET04.

## Запуск

```bash
cmake --build build-debug -j4 --target NET07_replication_baselines network_replication_test
ctest --test-dir build-debug -R '^(NET07_replication_baselines_verify|network_replication_test::)' --output-on-failure
```

## Не входит

- ECS dirty tracking и автоматическое перечисление компонентов;
- entity/network ID mapping, visibility, interest, ownership и quantization;
- wire serialization, compression и MTU fragmentation;
- resend/ACK/congestion policy реального транспорта;
- correction, rollback и presentation smoothing.
