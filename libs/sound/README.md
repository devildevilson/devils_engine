# libs/sound

`libs/sound` - звуковой слой движка. Актуальный путь сейчас строится вокруг `sound::system2` на miniaudio, `sound_resource` как demiurg-ресурса и набора декодеров, которые читают сжатые аудиоданные из памяти.

В библиотеке еще остались следы предыдущего OpenAL-пути (`sound::system`, `al_helper`, OpenAL overload'ы у декодеров и CMake-зависимость). Этот код не считается текущим целевым контрактом и должен быть почищен отдельно после окончательного решения по miniaudio.

## Основная Идея

Звуковая система не должна быть владельцем ассетов. Звуки загружаются через `demiurg`, main/gameplay держит handle на `sound::sound_resource`, а sound thread получает команду проиграть ресурс и читает из него `resource2`.

`resource2` - это легковесный view:

- `id` - идентификатор ресурса;
- `type` - тип данных (`mp3`, `flac`, `wav`, `ogg`, `pcm`);
- `data` - `span<const char>` на байты ресурса.

Данные должны жить дольше задачи воспроизведения. Сейчас это обеспечивается тем, что ресурс остается warm, пока им пользуется приложение. Координация выгрузки звуков с активными задачами еще требует отдельного правила.

## sound_resource

`sound::sound_resource` наследуется от `demiurg::resource_interface` и является CPU-only ресурсом:

- `warm_and_hot_same = true`;
- `binary = true`;
- `load_cold()` читает файл через demiurg module;
- тип выводится из расширения;
- `view()` возвращает `resource2`.

В `tile_frontier` звуки регистрируются как тип `sounds` с расширениями `mp3,flac,wav,ogg`. Main запрашивает нужные звуки до `warm`, хранит map `name_hash -> sound_resource*` и отправляет этот указатель в sound thread через broker.

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

В актуальном `system2`-пути `make_decoder(data_type, id, data)` создает декодер из памяти для `mp3`, `wav`, `flac` и `ogg`. `pcm` в этом пути пока не подключен: для `data_type::pcm` сейчас возвращается пустой decoder.

## system2

`sound::system2` - текущая miniaudio-реализация. Она владеет:

- `ma_context`;
- `ma_device`;
- `ma_engine`;
- пулом mono voice instances;
- пулом stereo voice instances;
- списком активных `sound_task`;
- scratch buffers для декодирования и конвертации;
- настройками playback device, sample rate, channel count и decode budget.

Устройство можно выбрать по имени. Если запрошенное устройство не найдено, система логирует предупреждение и создает default playback device. Список устройств доступен через `system2::playback_devices()`.

Playback callback у miniaudio вызывает `ma_engine_read_pcm_frames()`. Сами звуки подключены к engine как `ma_sound` поверх кастомного data source.

## Voice Instances И Data Source

`system2` заранее создает два пула:

- mono voices - для позиционных `sfx` и `talk_pos`;
- stereo voices - для UI/music/non-spatial сценариев.

Mono sounds создаются со spatialization, stereo sounds - с `MA_SOUND_FLAG_NO_SPATIALIZATION`. Для позиционного звука используется текущая модель miniaudio:

- linear attenuation;
- rolloff `1.0`;
- min gain `0.0`;
- max gain `1.0`;
- max distance `default_sound_max_distance`.

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

`system2` поддерживает:

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

`sound_simulation` владеет `sound::system2` и работает через общий broker:

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
- пересоздавать `system2` при выборе другого устройства в `tile_frontier`.

## Техдолг И Направления

- Почистить остатки OpenAL: старый `sound::system`, `al_helper`, OpenAL overload'ы у декодеров, CMake-зависимость и связанные include'ы. До решения по miniaudio 3D этот код лучше не смешивать с новым контрактом.
- Проверить качество и ограничения 3D-звука miniaudio на реальных сценах. Сейчас модель простая: mono positional, linear attenuation, distance culling.
- Добавить более сложные модели трехмерного звука: категории источников, приоритеты, virtual voices, occlusion/obstruction, doppler policy, distance curves и настройки listener/world scale.
- Добавить систему звуковых эффектов окружения: реверберация, фильтры, затухание, low-pass/high-pass и обработка в зависимости от помещения/среды.
- Профилировать `system2::update()`. Подозрительные места: создание decoder/converter на первом update задачи, seek, декодирование, `ma_data_converter_process_pcm_frames`, запись в ring buffer и уборка задач/voice instances.
- Оформить контракт выгрузки `sound_resource`: нельзя освобождать `data`, пока active task держит `resource2::data`.
- Подключить PCM в новом `resource2`/`system2` пути или убрать `pcm` из публичного списка форматов до готовности.
- Добавить формат Opus и загрузку с диска через `opusfile`.
- Добавить специальный источник постоянного/потокового звука, например live-поток из микрофона для VoIP.
- Вместе с VoIP-источником добавить capture device: создать устройство записи, читать голос с микрофона, собирать Opus-пакеты, публиковать статус/метрики, дать настройки громкости, добавить базовую фильтрацию и проверить готовые решения/библиотеки для этой части.
- Добавить тип звука в broker-команду play. Сейчас `tile_frontier` стадийно отправляет все как `sfx`.
- Доделать `set_source_volume()` и общую модель групп громкости: master/music/sfx/ui/dialogue.
- Добавить тесты на progress с `start > 0`, `after` sequencing, underrun accounting, device fallback и snapshot state.
