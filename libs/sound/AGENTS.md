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

Public headers: `devils_public_headers`; `public_link_sound` линкует только
`devils_engine::sound` и проверяет его usage requirements без aggregate target.

`sound_system_test`; для broker pin/lifetime — профильные simul/consumer tests.
Hardware smoke зависит от устройства; для spatial/perceptual changes — AU02 listening/visual
fixture. Нулевой exit headless теста не подтверждает качество звучания.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Новые контракты записывать здесь; актуальные проверки и открытые решения — в audit/review.
История разработки не является обязательной памятью проекта.

## Native ownership и измерения

- Один system-owned selected playback device, engine получает `pDevice` и фактические rate/channels;
  start после engine/voices. Offline `system_config` открывает 0 devices, render вызывается owner.
- Stop callback перед voices → engine → resource manager → device → context. Init flags обязательны
  для partial cleanup. `ma_data_converter` освобождается вместе с task.
- Resource manager имеет 0 job threads / NO_THREADING: async native file jobs не используются.
  Новый native file API требует явного решения о jobs. Backend device callback остаётся отдельным.
- Стандартные rings/voices настроены на playback rate; pool membership хранится отдельно от rate.
  Input frames и written output frames при resampling не смешивать в draining.
- Минимум машины — 8 hardware threads, не cap процесса. Voice cap пока не выбран.
  PCM decoder копирует весь source на voice; учитывать cold/RAM peak отдельно от ring capacity.
- 48 CPU series и методика: [audio_cpu](../../docs/review/audio_cpu/README.md).
  `sound_cpu_probe` не CTest performance gate; запускать без параллельной сборки.
  Offline CPU estimate не подтверждает hardware timing или качество звучания.

## Где выполняется mixing

Default `mixing_mode::owner`: update на выданном потоке производит PCM в output ring, callback
только потребляет. `device` — явный режим native mixing в callback; decoder/update остаются у owner.
Ring SPSC, один producer и один consumer, lifetime до stop callback; release после stop.
Недостающий output = silence, callback не ждёт и не берёт graph work на себя. 50 ms ring даёт
lookahead/задержку; progress относится к произведённому PCM. Mode не менять на живой системе.
`system_config.output_buffer_seconds` задаёт owner ring; offline render проверяет тот же sink path.
[Runtime settings](../simul/CONFIGURATION.md), [актуальные 1/5 voice measurements](../../docs/review/audio_cpu/SMALL_LOADS.md).
