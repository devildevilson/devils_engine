# painter — карта правки

Rendering и GPU assets, цель `devils_engine::painter`. Начинать с `structures.h`, `queue.h`,
`graphics_base.h`, `assets_base.h`, `compute_context.h`.
Конфигурация graph/resources/steps — TAVL, shaders принадлежат consumer или общему rendering helper.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Render graph задаёт resources, pass ordering и synchronization. Не обходить его скрытыми GPU
  side effects. GPU lifetime включает in-flight frames, не только C++ owner.
- Texture/mesh hot unload и bindless slot reuse должны ждать fence-safe retirement.
- Conditional doublebuffer не допускает rebuild смежных submitted frames, если прежняя generation
  ещё читается; cadence/rebuild gate должен учитывать число in-flight frames.
- Compute context — отдельный windowless path; не тянуть window/scene host в compute consumers.
- Shader include resolution и prepare идут через demiurg; Vulkan/VMA targets объявлять явно.
- Копии shader/frame setup из PF/GN переносить только с сохранением graph schema и visual baseline.
  Frozen PF07/PF08 frames не переснимать без задачи на их обновление.

## Проверка

`painter_shader_prepare_test`, `painter_queue_plan_test`, `painter_asset_lifetime_test`,
`painter_atlas_layout_test`, `painter_temporal_history_test`; compute —
`painter_compute_context_test`. GPU/visual changes дополнительно проверять профильной PF/GN
площадкой с bounded --frames и validation; skipped GPU check не доказывает renderer.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.

## Технический аудит 2026-11-07

Минимальное устройство — Vulkan 1.0. 256 МиБ VRAM — диагностический ориентир; graphics_base и assets_base VMA allocators учитываются вместе,
включая retiring/staging/per-frame generations. Driver heap budget — не project quota.
Painter пока сохраняет отдельную exception policy; это не отменяет проверок external config в Release.
GPU-измерения в техническом аудите не выполнялись. `memory_statistics()` у обоих owners отдаёт
allocation/block bytes и counts по heap; `combine_memory_statistics` проверяет owners/device и складывает их.

TECH-10 не требует слияния graphics/assets VMA: их owners различны. Per-owner/per-heap
accounting реализован; правило не складывать process-wide heap budgets дважды — docs/review/LIFETIME_AND_RESOURCES.md.
