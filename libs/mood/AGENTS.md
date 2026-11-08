# mood — карта правки

FSM store и отдельные runtime conventions. Цель `devils_engine::mood`.
`system.h` хранит transitions/index; `runtime.h` задаёт step/apply/conventions;
`diagnostics.h` — opt-in снимки и trace; `fsm_resource.h` — resources.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Lookup runtime — по предвычисленным hash, строки остаются для loading/diagnostics.
- `system` не приобретает игровую политику idle/event/reenter: conventions принадлежат runtime.
- `step_outcome` различает matched-but-blocked и отсутствие transition. Не сводить результат к bool.
- Decide и apply разделены. Exit/transition/enter effects исполняются только в apply, в заданном
  порядке; блокирующая анимация и очереди событий — ответственность consumer.
- Validation проверяет ссылки/опечатки при load; diagnostics не должна менять выбор transition.

## Проверка

`fsm_config_test`, `mood_diagnostics_test`, `utils_general_test` и consumer FSM pipeline.
При изменении эффекта/контекста добавить соответствующие act/script tests.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.

## Технический аудит 2026-11-07

Нативные resource transitions уже без кавычек, actions в tuple; tavl v1.1.2 это поддерживает.
Новый parser extension ради снятия кавычек не нужен. F-07 исправлен: transitions должен
непосредственно содержать array. Unrelated/duplicate/missing/EOF — ошибки, fsm_config_test.
