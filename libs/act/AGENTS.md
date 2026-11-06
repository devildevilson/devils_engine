# act — карта правки

Gameplay function boundary, `devils_engine::act`. Начинать с `function.h`, `registry.h`,
`exec_context.h`, `execution_scratch.h`, `intent.h`; compiler/resource glue — в `script_*`.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Функции разделены по return type (predicate/number/string/effect), не сводить к общему variant invoke.
- `exec_context` — immutable inputs по ссылке. Mutable VM state — per-worker execution_scratch;
  RNG зависит от seed/entity/tick и явно указанного purpose, без глобального draw-counter.
- Функции/registry не владеют ECS. World bridge и конкретные effects принадлежат consumer;
  решение производит intent, apply phase меняет мир в установленном порядке.
- `building_blocks.h` — декларативная регистрация native blocks; сохранять типы и policy purity.
- Describe/read-only queries не должны вызывать mutating backend. Не возвращать удалённый effect_sink.
- Registry function pointers и передаваемые string/view значения требуют живого владельца.

## Проверка

`act_building_blocks_test`, `act_call_context_test`, `act_packer_test`; для DS/resource seams —
`fsm_config_test`, `goap_config_test`, `stats_script_test`, `spawn_script_test`.
При изменении scratch/purity проверить `acumen_script_test` и consumer apply phase.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.
