# visage — карта правки

Lua/Nuklear UI host и render output. `system.h`/`src/visage/system.cpp` — state/env/frame;
`budget.h` — ограничения; `font_resource.h`, `font_atlas_packer.h`, `font.h` — fonts;
`render_output.h` — draw data. Nuklear bindings живут отдельно в libs/bindings.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- UI читает snapshots/resources и публикует commands. Не применять gameplay effects из UI/render.
- Lua entry point уже передаётся функцией, scripts/resources разрешаются через engine require.
  Не возвращать отдельный file loader в visage.
- Instruction/wall-time/GC/convert budgets и failure streak policy уже существуют. Доработка
  диагностики возможна, но задача «ввести бюджеты с нуля» устарела.
- Font/image durable references — resource_handle. Atlas/draw buffers нельзя освобождать, пока
  GPU читает поколение output; сохранять font metrics и descriptor lifetime.
- Nuklear bindings имеют global context ограничения: перед multi-context работой сначала проверить
  все обращения nk_context и consumer lifetime. Не обещать независимые UI states по одному env.

## Проверка

Public headers: `devils_public_headers`; `public_link_visage` линкует только
`devils_engine::visage` и проверяет его usage requirements без aggregate target.

Бюджеты/host проверяются через `simul_lifecycle_test` и профильные consumer smokes;
font/asset preparation — `painter_shader_prepare_test`. Отдельного функционального UI test executable сейчас нет.
Изменение nk output/шрифтов требует визуального прогона UI; наличие headless tests его не заменяет.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.
