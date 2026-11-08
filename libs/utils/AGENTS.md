# utils — карта правки

Низкоуровневая база. Цели `devils_engine::utils` и `devils_engine::thread` сейчас указывают
на одну библиотеку. API — `include/devils_engine/{utils,thread,utf}`.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- `serialization.h`, `state_schema.h`, `serialization_sink.h` — общие bytes/schema.
  ECS-проекция остаётся в aesthetics, сетевой state projection — в network. Не копировать codec.
- `atomic_file.h` — same-directory transaction; не заменять проверенную atomic publication на
  запись прямо в destination. `fileio.h` сохраняет отдельную роль обычного I/O.
- `simulation_time.h`, `timeline.h` — integer gameplay time; presentation/wall time не источник
  authoritative ticks. `prng.h`/`shared.h` — единые hash/random primitives.
- `geometry.h`, `kd_tree.h`, `grid.h`, `aabb_tree.h`: Vec — template-параметр; point stores и box store
  имеют разные правила попадания. Conservative overlap допускает лишние посещения, не пропуски.
- `thread/mailbox.h`, `spsc_queue.h`, `byte_ring.h`, `payload_channel.h` — разные delivery/ownership
  contracts. Не заменять их общей queue без сохранения overflow и lifetime.
- Размер/выравнивание аллокаторов — контракт. Не оживлять actor_ref/context_stack из exclude.
- `core.h` — только диагностика через error.h; остальные функции требуют явных narrow includes.
  Карта — README.md. Не возвращать umbrella include type_traits/spdlog/print в core.

## Проверка

`utils_contract_test`, `utils_serialization_test`, `utils_general_test`, `thread_general_test`,
`simulation_time_test`, `timeline_test` — выбирать по изменению. После изменения codec/schema
добавить `aesthetics_serialization_test` и соответствующие network checkpoint/digest tests.
CMake проверяет зависимости zstd/ZLIB; не предполагать единственное имя системной zstd-цели.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.

## Технический аудит 2026-11-07

Ориентир ресурсов и critical-error policy — в корневом AGENTS и TECHNICAL_AUDIT.md.
Исправлены F-01 (pool teardown: drain + join до разрушения членов), F-03 (move переносит
выданные blocks; source пуст), F-08 (variadic min). Регрессии: thread_general_test,
fixed_pool_move_test, utils_contract_test. Destruction требует остановленных producers.
Core разделён на narrow headers с сохранением публичных имён; Lua converter принадлежит
bindings_base. Zpp_bits и glaze в живом коде отсутствуют; named_serializer удалён.
utils::error бросает critical_error; не подавлять этот тип ради успешного продолжения.
