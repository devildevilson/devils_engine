# Engine, project, debug и пользовательские настройки

В bundled `resources/engine/config/` три независимых документа, которые bootstrap читает через
demiurg и объединяет в runtime view. Каталог engine — bootstrap registry, отделённый от модов;
это не означает, что настройки проекта стали общими правилами библиотеки.

| Документ | Секции и владелец |
| --- | --- |
| `engine.tavl` | `window`, `simulation`, `render`, `sound`, `ui`: запуск и настройка runtime, частоты систем, число worker threads, audio/render/UI machinery |
| `project.tavl` | `time`: игровая шкала и календарь; новые предметные секции добавляет проект |
| `debug.tavl` | `logging`, `metrics`: диагностика, включая startup Vulkan validation |

Bundled документы используют именованные секции верхнего уровня без внешних скобок.
Перенос секции в чужой документ вызывает предметный `utils::error`; ошибка типа не публикует
частично прочитанные значения. Отсутствующие поля получают C++ defaults. Составное `app_config`
пока сохраняет прежнюю форму для runtime consumers; источник секции определяется отдельным документом.
Общую библиотеку не нужно учить календарю: список project sections задаёт project resource adapter.

`app.tavl` заменён этими тремя файлами. Packaging удаляет прежнюю bundled копию перед копированием
ресурсов. Сборки tile_frontier/frontier_online сначала копируют общие resources, затем свои overlays;
проект может поставлять свой `resources/engine/config/project.tavl`.

## Пользовательские overrides

В project folder (рядом с `bin/`):

- `settings.tavl` — окно, sound device/gains/coloration и UI settings; engine/project topology не сохраняется;
- `debug.tavl` — отдельные пользовательские logging/metrics overrides;
- `key_bingings.tavl` — прежнее имя файла bindings сохранено.

Defaults debug берутся из bundled `resources/engine/config/debug.tavl`, после них применяются
пользовательские `debug.tavl`. Если отдельного debug-файла ещё нет, runtime переносит logging/metrics
из старого combined `settings.tavl`, сначала записывает debug, затем очищает player settings от
этих секций. Уже существующий debug-файл имеет приоритет; повторно из legacy settings он не заполняется.
Save/reload settings работают с обоими файлами. Топологические изменения требуют пересоздания runtime;
изменение Vulkan validation после создания instance по-прежнему сообщает о необходимости restart.

## Режим звука

В bundled `engine.tavl`, секция `simulation`:

```tavl
simulation = {
  sound_fps = 60
  sound_mixing = owner
  sound_output_buffer_ms = 50
}
```

- `owner` (default): декодирование, resampling и native mixing выполняет выданный sound-системе
  поток. Callback устройства только читает готовый f32 PCM из SPSC ring; при нехватке выдаёт тишину.
- `device`: decoder/update остаются в sound-потоке, native mixing выполняется в callback устройства.
  Это явный альтернативный режим; переключения по измеренному времени и дополнительного worker pool нет.

Выбор сохраняется при смене playback device. После init изменение режима требует пересоздания
системы. Backend transport callback нужен в обоих режимах; miniaudio resource manager имеет 0 job workers.
`owner` требует достаточно регулярного update. Выходное кольцо задаёт запас перед jitter и одновременно
добавляет задержку реакции, до 50 ms при default; учесть период backend. Малое кольцо при редком update
может давать тишину. Buffer принимается в пределах 1–1000 ms и не используется в `device` mode.
Progress/finished в owner mode относятся к уже произведённому PCM, который может ещё находиться
в output ring до его потребления устройством. Resource pins можно освободить: output владеет копией PCM.

Прямой API: `sound::system_config::mixing` и `output_buffer_seconds`.
Offline `owner`: update производит PCM, render его потребляет; offline `device`: render вызывает
native graph непосредственно. Это позволяет проверять обе ветки без физического audio device.

[Замеры 1/5 источников](../../docs/review/audio_cpu/SMALL_LOADS.md).
