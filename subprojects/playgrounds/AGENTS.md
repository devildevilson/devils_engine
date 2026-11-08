# Площадки: карта работы

Общие правила — [корневой AGENTS.md](../../AGENTS.md), каталог — [README.md](README.md),
очередь обслуживания — [TASKS.md](../../TASKS.md), аудит — [PROJECT_AUDIT.md](../../PROJECT_AUDIT.md).

- У каждой площадки один исследуемый контракт и локальные resources. Сохранять её baseline и
  предметную семантику при переносе механики. Старые варианты API originator не считаются
  сломанными только из-за появления новых.
- GN01 — pipeline/determinism, GN02 — planet package, GN03 — volume/streaming,
  GN04 — device queue, GN05 — constraints, GN06 — structure/motif/site, GN07 — brief→graph→plan.
  GN07 не линкует originator; новый такой перенос требует отдельного обоснования.
- GN09 legacy и NEXT — отдельные worlds/resources. Для NEXT актуален NEXT_CAVES_PLAN.md,
  старые boat exclusion/resource/time guarantees не действуют. Структурная проверка не заменяет
  визуальную оценку автора. Не вводить новый scheduler поверх tools/chain.
- `common` уже содержит camera/pacer/overlay и отдельные headless targets:
  `devils_engine::playground_cli` (CLI11 + parse exit policy),
  `devils_engine::playground_generator` (resource session, без UI/Vulkan).
  Не подключать `playground_common` к headless executable только ради этих headers.
- CLI schema и defaults — у executable; общая policy: --help/-h → 0, parse refusal → 2,
  runtime error → 1. GN01–GN07 и четыре executable GN09 используют CLI11.
  Tuple/override helpers проверяют полное число и arity; help выполняется до resources.
- GN01/GN02/GN03/GN04/GN05/GN06 используют `generator_resources`: session владеет modules
  и registry; держать её живой дольше pipelines/entries, порядок modules задаёт consumer.
- Общие execution algorithms принадлежат libs/originator, publication/streaming runtime —
  будущему владельцу residency, а не произвольной площадке. Shell/capture/CLI — common.
- GN03/GN09 используют `playground_streaming` (headless streaming/memory) и
  `playground_chunk_viewer` (viewer/resources). Реализации и render files принадлежат common;
  GN09 не компилирует соседние GN03 .cpp. Namespace gn03 сохранён для совместимости.
  Проверять ownership/lifetime вместе с обоими consumers; новый scheduler не добавлять.
- Viewers ограничивать --frames при smoke; без него они интерактивны. Снимать визуальные
  эталоны только при задаче на обновление look. PF07 frozen frames не переснимать автоматически.
- Перед заявлением об успехе узнать имена тестов через ctest -N. GN09 `gn_smoke` (seed 7)
  отделён от `gn_campaign` (legacy 0–63, NEXT 0–19), с отдельными timeouts.
  Короткий smoke не выдавать за campaign; PERF-01 остаётся отложенным.

- Для расширения generation algorithms сначала сверять [GN → originator audit](../../docs/review/GN_ORIGINATOR_TOOLS.md).
  GN07 исключён; GN09 legacy/NEXT — отдельные consumers. Общий kernel и общая Lua schema —
  разные задачи; независимые verify обходы не заменять вызовом проверяемого генератора.
- При SIMD-конфигурации GLM конструкторы векторов могут не быть constexpr. Неизменяемые
  таблицы glm::vec хранить как static const; не отключать выбранный ISA ради их компиляции.
