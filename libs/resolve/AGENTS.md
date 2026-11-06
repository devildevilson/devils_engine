# resolve — карта правки

Header-only bounded deterministic interaction resolving; цель `devils_engine::resolve`.
`work.h` — identity/records, `frontier.h` — work waves, `runner.h` — target groups,
`damage.h`/`retaliation.h` — neutral helpers, `journal.h` — outcomes; `resolve.h` — umbrella.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Параллельность только между независимыми target groups; внутри target порядок по semantic id.
  Callback пишет только свой target/owned outputs и не запускает nested pool submit/wait.
- Work identity не зависит от thread arrival order. Стабильные root/child/lane id — часть контракта.
- Outcome/child publication отделена от structural ECS changes; death не удаляет сущность посреди
  reader phase. Gameplay damage formulas и очередность карточного боя принадлежат consumer.
- Frontier/work/journal budgets явные; overflow/refusal нельзя выдавать за успешно завершённый бой.
- Retaliation следует существующему contract, а не бесконечной цепи встречных ответов.
- Это не replacement catalogue::mt или aesthetics message buffers: у механизмов разные обязанности.

## Проверка

`resolve_pipeline_test` и `cardgame` consumer checks (актуальные CTest names — через ctest -N).
Сравнивать semantic outcomes при 1/N workers, overflow и resume frontier, а не timing.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.
