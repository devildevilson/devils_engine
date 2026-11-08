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
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.

## Технический аудит 2026-11-07

Восемь аппаратных CPU-потоков — минимум машины, не cap процесса. Main/systems/pool/native
backends учитываются раздельно; hardware_concurrency minus reserved — не общий thread budget. Host владеет
thread и RAM/job admission budgets, не добавляя scheduler внутрь originator.
Открытые TECH-03/04 и измерения — в TECHNICAL_AUDIT.md.

## Владельцы конфигурации

Bundled config/engine, config/project, config/debug читаются отдельно через resource adapters;
apply_to переносит только разрешённые секции в composed runtime view. AppConfigResource реализует
apply_to; configuration_layers.h проверяет root sections и atomic publication. Не добавлять
предметные project sections в generic engine schema. Пользовательские logging/metrics отделены
от player settings и мигрируют один раз в debug.tavl. Карта — [CONFIGURATION.md](CONFIGURATION.md).
Audio mixing выбирается engine simulation.sound_mixing = owner|device до init, сохраняется при
recreate device; default owner, callback читает output ring. Автоматического fallback нет.
