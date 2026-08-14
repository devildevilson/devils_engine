# libs/sound

`libs/sound` - звуковой слой движка. Production path строится вокруг `sound::system` на miniaudio, `sound_resource` как demiurg-ресурса и набора декодеров, которые читают сжатые аудиоданные из памяти.

OpenAL больше не входит в live library: legacy OpenAL-реализация, ранее также называвшаяся
`sound::system`, `al_helper`, decoder buffer overloads,
линковка/DLL wiring и завершённая A/B-лаборатория архивированы в `exclude/openal_sound_legacy/` и
`exclude/audio_spatial_lab_openal/`. Backend-neutral PCM helpers перенесены в `common.h`.
После выбора backend miniaudio-класс переименован из `sound::system2` в канонический
`sound::system`; compatibility alias для `system2` намеренно не оставлен.

Headphone A/B 2026-08-12 закрыл выбор backend: miniaudio в целом звучит почти как исторический
OpenAL path, где HRTF, вероятнее всего, не включался. Front/up distance attenuation совпадает;
наблюдаемое различие — лишь небольшая direction-dependent coloration OpenAL.

## Основная Идея

Звуковая система не должна быть владельцем ассетов. Звуки загружаются через `demiurg`, main/gameplay держит handle на `sound::sound_resource`, а sound thread получает команду проиграть ресурс и читает из него `resource2`.

`resource2` - это легковесный playback view:

- `owner` - shared pin на immutable поколение данных;
- `id` - идентификатор ресурса;
- `type` - тип данных (`mp3`, `flac`, `wav`, `ogg`, `pcm`);
- `data` - `span<const char>` на байты ресурса.

`sound_resource::load_cold()` строит новый immutable `resource_blob` и публикует его атомарно.
Producer берёт `sound_resource::pin()` до отправки `command_sound_play`; команда переносит pin через
broker, а `system::task` удерживает его до terminal cleanup. `unload_warm()` атомарно снимает только
ссылку ресурса: queued/active задачи безопасно доигрывают старое поколение, а новые запросы его уже
не получают. Это закрывает и unload race, и окно между публикацией команды и её потреблением.

## sound_resource

`sound::sound_resource` наследуется от `demiurg::resource_interface` и является CPU-only ресурсом:

- `warm_and_hot_same = true`;
- `binary = true`;
- `load_cold()` читает файл через demiurg module;
- тип выводится из расширения;
- `pin()` возвращает shared immutable generation;
- `view()` строит owning `resource2` из текущего поколения.

В `tile_frontier` звуки регистрируются как тип `sounds` с расширениями `mp3,flac,wav,ogg,opus`.
Main запрашивает нужные звуки до `warm`, хранит handles и отправляет через broker handle плюс
producer-side pin. Handle остаётся identity/diagnostic fallback; playback bytes принадлежат pin.

## Декодеры

Общий интерфейс `sound::decoder` умеет:

- `seek(frame)`;
- читать PCM frames в память;
- сообщать формат, число каналов, sample rate и общее число frames.

Сейчас есть декодеры:

- `mp3_decoder` на `dr_mp3`;
- `flac_decoder` на `dr_flac`;
- `wav_decoder` на `dr_wav`;
- `ogg_decoder`;
- `pcm_decoder`.

В актуальном `system`-пути `make_decoder(data_type, id, data)` создает декодер из памяти для `mp3`, `wav`, `flac` и `ogg`. `pcm` теперь подключён (2026-07-05): короткие звуки (`< 5с`) `sound_resource` декодирует целиком в PCM в `load_cold` (данные `resource2` = сырые кадры, `type=pcm`, + метаданные `sample_format/channels/sample_rate/frames_count`), а `system` играет их отдельной веткой через `pcm_decoder` (passthrough), минуя `make_decoder`. Метаданные аудио в `resource2` заполняются всегда. Сырые `.pcm`-ФАЙЛЫ по-прежнему не загружаются (нет заголовка/формата).

## system

`sound::system` - текущая miniaudio-реализация. Она владеет:

- `ma_context`;
- `ma_device`;
- `ma_engine`;
- пулом mono voice instances;
- пулом stereo voice instances;
- списком активных `sound_task`;
- scratch buffers для декодирования и конвертации;
- настройками playback device, sample rate, channel count и decode budget.

Устройство можно выбрать по имени. Если запрошенное устройство не найдено, система логирует предупреждение и создает default playback device. Список устройств доступен через `system::playback_devices()`.

Playback callback у miniaudio вызывает `ma_engine_read_pcm_frames()`. Сами звуки подключены к engine как `ma_sound` поверх кастомного data source.

## Voice Instances И Data Source

`system` заранее создает два пула:

- mono voices - для позиционных `sfx` и `talk_pos`;
- stereo voices - для UI/music/non-spatial сценариев.

Mono sounds создаются со spatialization, stereo sounds - с `MA_SOUND_FLAG_NO_SPATIALIZATION`. Для позиционного звука используется текущая модель miniaudio:

- linear attenuation;
- rolloff `1.0`;
- min gain `0.0`;
- max gain `1.0`;
- max distance `default_sound_max_distance`.

Built-in miniaudio 0.11.25 spatializer не является HRTF renderer. Он не теряет `Y`: координата
проходит listener transform и участвует в distance attenuation. Ограничение возникает при panning:
default stereo endpoint использует `SIDE_LEFT`/`SIDE_RIGHT` directions `(-1,0,0)`/`(+1,0,0)`, поэтому
равнодистанционные позиции `+Y` и `-Y` получают одинаковые gains. Front/back обычный stereo panner
тоже не кодирует. Не исправлять это перестановкой осей. Открытый небольшой experiment — добавить
очень мягкую miniaudio-native coloration для источников сзади/сверху/снизу; полноценный HRTF
отложен до pre-release spatial pass проекта `submarine_coop`.

`AUD-17` реализует этот experiment как optional post-spatialization high shelf только на mono
voices. Listener-relative response не использует distance/radius: front нейтрален, зафиксированный
профиль — behind `-2.25 dB`, above `+0.65 dB`, below `-0.85 dB` на `2.5 kHz`. Общий `strength`
в `[0,2]` масштабирует весь cue; итог всё равно ограничен `[-3,+1] dB`. Коэффициент плавно движется к
target внутри audio callback (`80 ms` по умолчанию); sound thread передаёт только atomic targets.
Публичная конфигурация sanitizes strength до `[0,2]`, behind до `[-3,0] dB`, elevation до
`[-1,+1] dB`, shelf frequency
до `[500,12000] Hz`, smoothing до `[10,500] ms`. Эффект default-off и не применяется к
non-spatial tasks. `tile_frontier` хранит профиль в `sound.directional_coloration`, но оставляет его
выключенным: top-down camera listener не является честной 3D head orientation.

Для ручного решения есть miniaudio-only
`subprojects/playgrounds/AU02_directional_coloration`: production system,
одинаковый deterministic mono signal и constant-radius horizontal/vertical orbit. Сравнивать два
запуска `--coloration off` и `--coloration on`; OpenAL dependency не возвращается. Унаследованные
периодические click transients удалены после первого прослушивания, оба сигнала имеют 20 ms краевые
fades. После двух орбит lab даёт изолированные one-second above/below holds с одинаковыми
panning/radius/distance. Default signal — continuous harmonic hum (`110 Hz`, 64 обертона до
`7.04 kHz`): стабильный спектр по обе стороны shelf лучше показывает изменение тембра; deterministic
white noise остаётся через `--signal noise`.

Каждый `sound_instance` содержит `ma_sound` и кастомный ring-stream data source. Data source хранит PCM ring buffer, read/write cursors, счетчики прочитанных/записанных frames и underrun count. Он не знает о task id, sequencing или ресурсах.

## Задачи Воспроизведения

Публичная команда воспроизведения описывается `sound::task`:

- `id` - task id;
- `res` - `resource2`;
- `type` - `music`, `talk`, `talk_pos`, `ui_effect`, `sfx`;
- `pitch`, `volume`;
- `start` - нормализованная позиция в источнике `[0,1]`;
- `after` - id предыдущей задачи для gapless/sequence continuation;
- `pos`, `dir`, `vel` - позиционные параметры.

`setup_sound()` валидирует task, отбрасывает пустые/неизвестные ресурсы, слишком далекие позиционные звуки и дубли task id. После этого задача попадает в `m_tasks`.

В `update()` система:

1. создает decoder и miniaudio converter для еще не инициализированной задачи;
2. применяет `start` через decoder seek;
3. выбирает output channel count по типу звука;
4. выдает voice instance из mono/stereo пула или создает кастомный instance;
5. регистрирует сегмент в stream;
6. дозированно декодирует frames в scratch buffer;
7. конвертирует их в playback format;
8. пишет PCM в ring data source;
9. запускает `ma_sound` при первом декодированном блоке;
10. удаляет завершенные задачи и возвращает voice instance в пул.

`decode_frames_per_update` ограничивает объем работы на один `update()`. Если он равен нулю, используется playback sample rate. Дополнительно декодирование ограничивается свободным местом ring buffer, размером scratch buffers и оставшейся длиной сегмента.

## Статус И Управление

`system` поддерживает:

- `remove_sound(task_id)`;
- `play_sound(task_id)` / `stop_sound(task_id)` для ручного управления уже подготовленной задачей;
- `set_sound(task_id, place)` для seek по нормализованной позиции;
- `stat_sound(task_id)` и `stat_sound(task_id, task_status&)`;
- `snapshot(vector<task_status>&)`;
- `update_sound(task_update)` для обновления позиции/направления/скорости;
- `set_listener_pos()`;
- `set_listener_ori()`;
- `set_listener_vel()`;
- `set_master_volume()`.

Прогресс в `task_status` считается как абсолютная позиция в исходном звуке `[0,1]`, а не как доля проигранного сегмента. Это важно для звуков, начатых с `start > 0`: UI-плеер видит реальную позицию в исходнике.

`after` используется для продолжения на том же voice instance. Следующая задача ждет, пока предыдущий сегмент будет полностью декодирован в shared stream, после чего callback продолжает читать PCM без знания о task sequencing.

## Интеграция В tile_frontier

`sound_simulation` владеет `sound::system` и работает через общий broker:

- main -> sound: play, stop, update, device list, recreate device, master gain;
- sound -> main: latest-wins snapshot состояния звуков.

Sound thread не хранит свои копии ресурсов. Он получает `sound_resource*`, берет `view()` и создает `sound::task`. Если ресурс еще не warm или тип неизвестен, команда пропускается с warning.

Main сразу возвращает UI opaque handle на основе task id, а затем читает `command_sound_state` snapshot, чтобы `app.sound_state(handle)` мог вернуть текущий progress или `nil`.

## Что Сейчас Может

На текущем срезе `libs/sound` умеет:

- создавать miniaudio context/device/engine;
- выбирать playback device по имени и fallback'иться на default;
- перечислять playback devices;
- проигрывать mp3/wav/flac/ogg из memory-backed demiurg resources;
- стримить сжатые данные через decoder + converter в PCM ring source;
- держать preallocated mono/stereo voice pools;
- играть позиционные mono sounds и non-spatial stereo sounds;
- обновлять listener position/orientation/velocity;
- обновлять position/direction/velocity у активного sound task;
- ограничивать дальние позиционные звуки по max distance;
- поддерживать start position `[0,1]`;
- поддерживать простую sequence continuation через `after`;
- публиковать snapshot состояния задач;
- менять master volume;
- пересоздавать `system` при выборе другого устройства в `tile_frontier`.

## Техдолг И Направления

- `AUD-17` закрыт: optional bounded shelf оставлен как дешёвый прежде всего front/back cue; ручная
  проверка показала, что elevation он надёжно не кодирует. Для height/front-back качества нужен HRTF,
  а не дальнейшее усиление shelf.
- Steam Audio/HRTF оценивать ближе к релизу `submarine_coop`, когда появятся реальные требования к elevation/front-back, voice budget и целевым платформам.
- Добавить более сложные модели трехмерного звука: категории источников, приоритеты, virtual voices, occlusion/obstruction, doppler policy, distance curves и настройки listener/world scale.
- Добавить систему звуковых эффектов окружения: реверберация, фильтры, затухание, low-pass/high-pass и обработка в зависимости от помещения/среды.
- Профилировать `system::update()`. Подозрительные места: создание decoder/converter на первом update задачи, seek, декодирование, `ma_data_converter_process_pcm_frames`, запись в ring buffer и уборка задач/voice instances.
- ~~Оформить контракт выгрузки `sound_resource`~~ (СДЕЛАНО 2026-08-12: immutable shared generations и producer-side pin).
- ~~Подключить PCM в новом `resource2`/`system` пути~~ (СДЕЛАНО 2026-07-05: короткие звуки → PCM в `load_cold`, отдельная ветка `pcm_decoder` в `system`; сырые `.pcm`-файлы намеренно не загружаются).
- ~~Координация unload с queued/active playback~~ (СДЕЛАНО 2026-08-12: immutable shared generations, producer-side broker pin и focused lifetime test).
- Добавить формат Opus и загрузку с диска через `opusfile`.
- Добавить специальный источник постоянного/потокового звука, например live-поток из микрофона для VoIP.
- Вместе с VoIP-источником добавить capture device: создать устройство записи, читать голос с микрофона, собирать Opus-пакеты, публиковать статус/метрики, дать настройки громкости, добавить базовую фильтрацию и проверить готовые решения/библиотеки для этой части.
- Добавить тип звука в broker-команду play. Сейчас `tile_frontier` стадийно отправляет все как `sfx`.
- Доделать `set_source_volume()` и общую модель групп громкости: master/music/sfx/ui/dialogue.
- Добавить тесты на progress с `start > 0`, `after` sequencing, underrun accounting, device fallback и snapshot state.
