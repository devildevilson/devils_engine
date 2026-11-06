# sound — карта правки

Production audio — miniaudio `sound::system`, цель `devils_engine::sound`.
`system.h`/`src/sound/system.cpp` — tasks/voices/update; `sound_resource.h` — immutable generations;
`*_decoder.*` — memory decoders; `common.h` — backend-neutral math/PCM.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
[README.md](README.md).

## Границы и контракты

- OpenAL/system2 — архив, compatibility path не восстанавливать.
- Producer получает shared pin sound generation ДО broker publish. Queued/active task удерживает
  immutable blob до terminal removal; unload снимает только ссылку ресурса.
- Raw resource pointer не удерживает decoded/encoded audio. Не разыменовывать его позднее на worker
  вместо pin. Same rule важен при reload и смене источника.
- Listener/front/up/scale и volume groups имеют явную consumer convention; не менять axes только
  в одном backend/helper.
- Audio update и device callback имеют разные ограничения. Decoder/converter allocation и seek
  измерять по реальному voice budget, не предполагать allocation-free по имени метода.
- Opus decoder уже существует. VoIP capture/stream, HRTF, environmental acoustics остаются отдельными
  задачами; optional coloration shelf не доказывает elevation/HRTF.

## Проверка

`sound_system_test`; для broker pin/lifetime — профильные simul/consumer tests.
Hardware smoke зависит от устройства; для spatial/perceptual changes — AU02 listening/visual
fixture. Нулевой exit headless теста не подтверждает качество звучания.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.

## Технический аудит 2026-11-07

Технический аудит требует проверить ma_engine default device и отдельно создаваемый m_device:
noAutoStart не означает noDevice. Также учитывать native resource-manager/device threads в общем
пределе восьми потоков. PCM rings уже ленивые, но активные/повторно использованные voices удерживают
capacity; для всех стандартных rings при 1 s/f32/48 kHz верхняя сумма около 70.3 МиБ без прочего heap.
См. TECHNICAL_AUDIT.md и TECH-12; аппаратная проверка здесь ещё не выполнена.
