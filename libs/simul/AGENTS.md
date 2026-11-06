# simul — карта правки

Общий host/runtime. `game_host.h`, `app_runtime.h`, `main_runtime.h`, `lifecycle.h`
задают lifetime; `standard_broker.h`/`messages.h` — обмен; `gameplay_timeline.h` — ticks;
`loading_runtime.h`/`startup_resources.h` — загрузка; `lua_resource_bindings.h` — resources в Lua.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Host владеет bootstrap/stop/join; worker не должен пережить broker/resources, которыми пользуется.
  Сохранять partial-init и отключаемые render/sound/assets topology.
- Gameplay time — exact integer ticks; wall/presentation clock не должен менять causal state.
- Broker channels различают latest snapshot, reliable commands и payload ownership; не заменять
  их одной очередью. Producer pin-ит sound resource generation до отправки команды.
- Immutable draw intents/scene state передаются через commit/publication boundary; render не
  применяет gameplay effects. `interaction_commit.h` — общая фаза, политика игры остаётся в project.
- Lua require/request идёт через demiurg, engine владеет разрешением путей и module cache.
- Streaming priority/cancellation/epoch publication — будущий runtime owner. Не переносить
  скрытый scheduler внутрь originator tools ради нужд viewer.

## Проверка

`simul_lifecycle_test`, `simul_settings_files_test`, `gameplay_timeline_test`, `turn_pipeline_test`;
при изменении resources/sound publication добавить `demiurg_resource_loader_test`,
`sound_system_test`; project wiring — tile_frontier/frontier_online smokes по затронутому пути.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.

## Технический аудит 2026-11-07

Восемь потоков — общий ориентир main/systems/pool/native backends. hardware_concurrency minus
reserved пока не обеспечивает этот предел на машине с большим числом CPU. Host должен владеть
thread и RAM/job admission budgets, не добавляя scheduler внутрь originator.
Открытые TECH-03/04 и измерения — в TECHNICAL_AUDIT.md.
