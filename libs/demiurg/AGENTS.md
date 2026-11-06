# demiurg — карта правки

Modules, registry, stable resource handles, staged load/unload. `module_system.h`,
`resource_system.h`, `resource_base.h`, `resource_loader.h`, `resource_manifest.h`,
`resource_path.h` — основные точки чтения; folder/zip — отдельные sources.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- Durable handle — system + logical id/hash, не resource pointer. Handle переживает clear/reparse
  того же живого resource_system; сам system обязан жить дольше handle.
- Сырые pointer/view допустимы для немедленного чтения при стабильном registry; не переносить
  их между потоками или через unload/reparse. Общий pointer lifetime не заменяет pin поколения.
- Registry/override порядок определяет identity ресурса до исполнения Lua; не обходить modules
  прямым чтением файлов в production consumers.
- Resource dependencies и external GPU steps gate-ят публикацию usable state.
- Имя выводится из пути без расширения: x.lua и x.tavl рядом способны столкнуться по id.
- Module listing/fingerprint уже реализованы в resource_manifest/modules_listing; новый import-rule
  longest-segment matching, migration и persistent artifact cache — отдельные открытые задачи.
- Lua request/require bindings принадлежат simul/bindings consumer, не самому demiurg.

## Проверка

`demiurg_resource_loader_test`; после resource path/config изменений —
`originator_config_test`, `painter_shader_prepare_test`, `goap_config_test`, `fsm_config_test`.
Проверять staged refusal, dependency gating и stable handle при clear/reparse.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.

## Технический аудит 2026-11-07

F-04/F-05 исправлены: ZIP проверяет byte counts/short read/CRC; listing использует filesystem
extension и проверяет поиск до разыменования. Профили — versioned TAVL (module_profile.h),
единый reader/writer для listing и system, atomic publication. Пути относительны modules root,
порядок modules сохраняется. Legacy JSON мигрирует scripts/convert_module_profile.py; скрытого
fallback нет. Проверка — demiurg_profile_test. Dependency cycle warn/skip остаётся задачей аудита.
