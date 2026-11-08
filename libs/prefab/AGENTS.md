# prefab — карта правки

Config-driven entity construction. `prefab_registry.h` — template registry/build/apply,
`resource.h`/`src/prefab/resource.cpp` — demiurg resource. Цель `devils_engine::prefab`.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
заголовки и CMakeLists.txt.

## Границы и контракты

- Project регистрирует component types и custom/reference builders; библиотека не знает состав игры.
- Наследование data — field-level слоями base→derived. list/callback/reference заменяются целиком.
  Не сводить их к одному merge, меняющему смысл рецептов.
- Callback names резолвятся через act::registry; external references — resolver consumer.
- on_construct достраивает derived state из SpawnArgs; persistent authoritative components и
  transient indices имеют разные роли.
- Spawn/apply — structural phase ECS. Не выполнять construction параллельно с views без
  разрешённой world mutation boundary. Resource reload не продлевает lifetime raw captured pointers.

## Проверка

`prefab_test`, `spawn_script_test`; для schema/load — `aesthetics_serialization_test`,
`demiurg_resource_loader_test`. Проверять inheritance и refusal до публикации частичной entity.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.
