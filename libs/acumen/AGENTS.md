# acumen — карта правки

GOAP, цель `devils_engine::acumen`. Карта: `common.h` — state/action/goal,
`system.h` + `src/acumen/system.cpp` — compute/decide, `astar.h` — search,
`cache.h` — solution cache, `registry.h`/`goap_resource.h` — загрузка.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- System резолвит act predicates/effects при construction; hot path не ищет имена заново.
- `decide_params::scratch` обязателен и принадлежит потоку; output plan принадлежит caller.
  Возвращаемая полная длина может превышать заполненный span: consumer обязан это учитывать.
- Ключ cache включает relevant state и тождество цели/системы. При общем cache salt систем
  должен отличаться; registry задаёт salt из system id. Не кешировать по одному голому start state.
- Cache необязателен; попадание и живой A* должны давать одну семантику плана.
- Планирование не применяет gameplay effects. Selection/commit/time budgets принадлежат host.

## Проверка

`acumen_test`, `acumen_script_test`, `goap_config_test`; после изменения act context/scratch —
также act tests. Новый bounded search должен отличать отказ/неполный план от успеха.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.
