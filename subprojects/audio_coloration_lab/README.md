# audio_coloration_lab

Минимальная miniaudio-only проверка `AUD-17`. Она использует production `sound::system`, один
детерминированный mono signal и две constant-radius орбиты. OpenAL и HRTF здесь намеренно нет:
сравниваются только одинаковые проходы с bounded coloration `on`/`off`.

```sh
cmake --build build-debug --target audio_coloration_lab -j2
./build-debug/subprojects/audio_coloration_lab/bin/audio_coloration_lab --coloration off
./build-debug/subprojects/audio_coloration_lab/bin/audio_coloration_lab --coloration on
```

Default reference signal (`--signal hum`) — непрерывный 110 Hz harmonic buzz с 64 обертонами до
7.04 kHz. Он имеет стабильную энергию ниже и выше shelf frequency, поэтому эффект должен читаться
как изменение окраски одного и того же гудка. `--signal noise` оставляет deterministic white-noise
контроль. Оба сигнала имеют 20 ms fade-in/fade-out и не содержат периодических click transients.

Слушать в наушниках прежде всего отметки front/back на горизонтальной орбите и above/below на
вертикальной. После орбит `16–20s` идут изолированные one-second `ABOVE/BELOW` holds: у них
одинаковы радиус, stereo panning и distance gain, поэтому при `--coloration off` они должны звучать
одинаково, а при `on` отличаются только high-shelf cue. Радиус всегда равен 4, поэтому изменение
громкости от distance curve отсутствует.
Ожидаемый эффект очень мал: behind чуть темнее, above едва светлее, below едва темнее. Если cue
воспринимается как изменение расстояния или заметный тембральный эффект, параметры слишком сильные.
Текущий зафиксированный профиль: behind `-2.25 dB`, above `+0.65 dB`, below `-0.85 dB` на
`2.5 kHz`; общий `--strength` в диапазоне `[0,2]` масштабирует cue, не меняя distance/panning.

Полезные опции: `--signal hum|noise`, `--strength 0.5`, `--list-devices`,
`--device "exact name"`, `--gain 0.25`,
`--dry-run`.
