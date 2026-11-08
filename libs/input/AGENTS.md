# input — карта правки

GLFW core/window helpers, input events и action bindings. `core.h`, `events.h`,
`key_names.h`, `bindings.h` + одноимённые src — карта. Цель `devils_engine::input`.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Keyboard key и platform scancode — разные значения. Canonical keyboard names требуют живого
  GLFW; synthetic mouse names/scancode_N имеют отдельный fallback.
- Validate весь settings snapshot до применения. Apply заменяет перечисленные actions целиком,
  очищая хвостовые слоты; пустой список отвязывает action.
- Collect сохраняет именованные bindings; голый hash не восстанавливает исходное имя.
- Callbacks собирают input, а gameplay intents и настройка control schemes живут выше.
- Не добавлять Vulkan/render зависимости в нейтральные event helpers ради удобства viewer.

## Проверка

Public headers: `devils_public_headers`; `public_link_input` линкует только
`devils_engine::input` и проверяет его usage requirements без aggregate target.

`input_bindings_test`, `tile_frontier_config_test`, `simul_settings_files_test`;
изменение native callbacks/window lifecycle дополнительно требует реального window smoke.
Не выдавать headless проверки synthetic mouse за проверку всей клавиатуры GLFW.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.
