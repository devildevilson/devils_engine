# audio_spatial_lab

Ручная A/B-лаборатория сравнивает текущий `sound::system2`/miniaudio path и OpenAL на одном
детерминированном mono S16 PCM, одной listener frame, одинаковой linear-clamped attenuation и одной
траектории. Это отдельный executable, не gameplay scene и не новый backend abstraction.

Сценарий длится 28 секунд:

1. `0–8s` — полный горизонтальный orbit: проверка left/right и front/back;
2. `8–16s` — полный вертикальный orbit с постоянным радиусом `4`: проверка только elevation и
   direction-dependent coloration, но не distance attenuation;
3. `16–22s` — front-axis distance pulse `4 → 10 → 1 → 4`;
4. `22–28s` — up-axis distance pulse `4 → 10 → 1 → 4`, идентичный предыдущему по distance curve.

Два pulse нужны для прямой проверки axis parity: при одинаковой linear attenuation громкость вдоль
`-Z` и `+Y` должна меняться одинаково. Если perceived distance меняется на vertical orbit, но два
pulse совпадают, причиной является direction-dependent panning/HRTF coloration, а не расчёт distance.

Источник сочетает детерминированный broadband noise, тоны 220/880 Hz и короткий click каждые 0.5s.
Doppler исключён: listener/source velocity нулевые, OpenAL Doppler factor равен нулю.

## Сборка и запуск

```sh
cmake --build build-debug --target audio_spatial_lab -j2
./build-debug/subprojects/audio_spatial_lab/bin/audio_spatial_lab --list-devices
```

Для честного headphone A/B нужно запускать отдельные passes с одинаковыми `--device`, `--gain`,
`--min-distance`, `--max-distance`:

```sh
./build-debug/subprojects/audio_spatial_lab/bin/audio_spatial_lab --backend miniaudio
./build-debug/subprojects/audio_spatial_lab/bin/audio_spatial_lab --backend openal --hrtf off
./build-debug/subprojects/audio_spatial_lab/bin/audio_spatial_lab --backend openal --hrtf on
```

OpenAL pass печатает, доступно ли `ALC_SOFT_HRTF`, реально ли HRTF включился и имя выбранного HRTF.
У текущего miniaudio engine spatializer отдельного HRTF-переключателя нет, что лаборатория сообщает
явно. Поэтому полезны оба OpenAL passes: `off` сравнивает базовую spatialization, `on` показывает
потолок OpenAL Soft для наушников.

`--dry-run` не открывает устройство и печатает fingerprint PCM и sample points траектории. Он также
зарегистрирован как headless CTest.

## Что записывать после каждого pass

| Критерий | Miniaudio | OpenAL HRTF off | OpenAL HRTF on |
| --- | --- | --- | --- |
| Непрерывность horizontal azimuth | | | |
| Front/back различимость | | | |
| Above/below различимость | | | |
| Near-field стабильность | | | |
| Совпадение attenuation curve | | | |
| Front/up distance-pulse parity | | | |
| Окраска тембра/усталость | | | |
| Clicks/dropouts/level jumps | | | |

Сравнивать следует в одних наушниках и одной системной громкости. Лаборатория не утверждает, что
один backend звучит лучше до ручного прослушивания на реальном устройстве.

## Результат прослушивания 2026-08-12

- OpenAL HRTF on заметно улучшает понимание направления на источник.
- Built-in spatializer miniaudio звучит близко к OpenAL HRTF off.
- У miniaudio источник выше и ниже listener почти неразличим.

Последний пункт не вызван потерей координаты в `sound::system2`. Miniaudio 0.11.25 преобразует все
три координаты в listener-space и использует `Y` при вычислении distance. Но его default stereo
channel map — `SIDE_LEFT`/`SIDE_RIGHT`, то есть speaker directions `(-1,0,0)` и `(1,0,0)`. При
дефолтных `directionalAttenuationFactor=1` и `minSpatializationChannelGain=0.2` directional gains
до общего distance gain равны:

```text
left  = max((1 - unit_position.x) / 2, 0.2)
right = max((1 + unit_position.x) / 2, 0.2)
```

Следовательно, `(0,+r,0)` и `(0,-r,0)` дают ровно `(0.5,0.5)`. Vertical orbit имеет постоянный
радиус `4`, поэтому совпадает и distance gain: неизменный уровень miniaudio на этой фазе ожидаем.
Точно так же базовый stereo panner сам по себе не кодирует front/back; эти признаки требуют HRTF
или реальной speaker layout с height/front/back channels. Отдельный up distance pulse теперь
проверяет, что изменение самого `Y`-расстояния даёт ту же attenuation curve, что и front pulse.

Steam Audio остаётся возможным, но необязательным будущим HRTF experiment: для текущего решения он
слишком велик относительно ещё не проверенной проблемы. Сначала следует сравнить front/up pulses и
не путать direction-dependent окраску HRTF на постоянном радиусе с ошибкой distance attenuation.
