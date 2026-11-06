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
  runtime error → 1. Пока мигрированы GN01/GN05; остальные имеют историческое поведение.
- Общие execution algorithms принадлежат libs/originator, publication/streaming runtime —
  будущему владельцу residency, а не произвольной площадке. Shell/capture/CLI — common.
- GN09 сейчас компилирует streaming/memory/viewer из GN03 и использует его render resources.
  Это действующая связь: изменение GN03 проверять с GN09 consumers; не переименовывать её
  как независимую библиотеку без CMake/resources/lifetime migration.
- Viewers ограничивать --frames при smoke; без него они интерактивны. Снимать визуальные
  эталоны только при задаче на обновление look. PF07 frozen frames не переснимать автоматически.
- Перед заявлением об успехе узнать имена тестов через ctest -N. GN09 registered verify на
  64 seeds и NEXT на 20 — длинные проверки; явный короткий smoke не выдавать за полный прогон.
