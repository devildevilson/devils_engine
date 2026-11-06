# flow — карта правки

Timed animation sampling, цель `devils_engine::flow`. `system.h`/`src/flow/system.cpp`
содержат library/state/playback; `animation_resource.h` — TAVL resource parsing.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Presentation и gameplay playback уже разделены: elapsed_mcs и elapsed_ticks — разные clocks.
  Gameplay action-on-entry не должна зависеть от render FPS или presentation sampling.
- State хранит next index, duration, resource_handle + mirror, UV delta и action id.
  Не хранить resource_interface* как durable image reference.
- Runtime отдаёт action events наружу; не вызывает act effects из render thread.
- Zero-duration chain ограничена step limit; loops/невалидный next обязаны завершать вызов bounded.
- Direction angle convention задаёт caller. Сохранять bucket boundaries и UV accumulation.
- Skeletal clips/blending/root motion — будущая работа с реальным consumer, не текущая гарантия.

## Проверка

`flow_test` проверяет parsing, buckets, playback, actions и time contracts.
При изменении image resource lifetime — также demiurg tests и consumer presentation path.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.
