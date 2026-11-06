# aesthetics — карта правки

ECS и его state projection. `world.h`/`src/aesthetics/world.cpp` — storage/views/queries;
`serialization.h` — ECS snapshot adapter; `sink.h` — effects/structural seam.
`system_runner.h`, `*_buffer.h`, `interaction_arena.h` — bounded execution/commit mechanisms.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Entity identity включает version; relation fields хранят полный id и проверяются через exists.
  Не вводить скрытый automatic relation cleanup.
- Queries, созданные до snapshot load, должны увидеть restored world: сохранять уведомление при load.
- Snapshot включает authoritative components, derived indices строятся заново. Общий байтовый codec
  уже в utils: не возвращать второй serializer сюда и не сериализовать raw padded structs.
- Structural changes и parallel component processing разделены barrier/apply boundary.
- `system_runner` группирует независимые systems без скрытого DAG. Collect/elect/message buffers
  имеют собственные ordering и overflow contracts; thread arrival order не является semantic order.

## Проверка

`aesthetics_world_test`, `aesthetics_serialization_test`, `aesthetics_pipeline_test`;
для channels — `aesthetics_message_buffer_test`, `aesthetics_collect_buffer_test`,
`aesthetics_elect_buffer_test`, `aesthetics_interaction_arena_test`; для flags —
`aesthetics_flag_set_test`. При codec changes также utils/network tests.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.
