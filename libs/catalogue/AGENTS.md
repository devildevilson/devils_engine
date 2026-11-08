# catalogue — карта правки

Небольшие wrappers вызовов, deferred execution и диагностика. `introspection.h`,
`call_log.h`, `deferred.h`, `logging.h`, `phase.h` — live API; цель `devils_engine::catalogue`.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Не расширять библиотеку до serializer/replay/netcode и не восстанавливать старый RPC из exclude.
- Deferred strategies collect/elect/structural описывают порядок commit, identity domain отдельно
  от reusable strategy. Конкурентный порядок записи не должен стать порядком gameplay effects.
- Journals, output/scratch и statistics имеют явного владельца и bounded lifetime.
- `phase.h` — passive metadata, не второй scheduler. Introspection/diagnostics не меняет исполнение.
- Gameplay semantics остаётся в act/consumer, target-group resolving — в resolve.

## Проверка

`catalogue_call_log_test`, `catalogue_deferred_test`, `catalogue_introspection_test`,
`catalogue_phase_metadata_test`. При изменении commit policy проверить
`aesthetics_pipeline_test` и соответствующий simul/consumer path.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.
