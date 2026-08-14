# AU01 — spatial audio A/B

Завершённая историческая площадка, на которой production miniaudio сравнивался с direct OpenAL Soft
на одинаковом deterministic mono signal. Проверялись horizontal/vertical orbit, front/up distance
pulse, attenuation и OpenAL HRTF off/on.

## Результат

- Miniaudio выбран production backend.
- Front и up distance attenuation работают одинаково.
- Равнодистанционные `+Y/-Y` почти неразличимы без HRTF; это ограничение stereo panner, а не потеря
  координаты `Y`.
- OpenAL HRTF заметно улучшал локализацию, но сейчас является избыточной зависимостью.

## Где код

OpenAL и законченный A/B executable намеренно не возвращаются в live build. Их снимок находится в
[`exclude/audio_spatial_lab_openal`](../../../exclude/audio_spatial_lab_openal). Живая следующая
площадка — [`AU02_directional_coloration`](../AU02_directional_coloration).

Если перед релизом `submarine_coop` понадобится новый HRTF comparison, он должен получить отдельный
bounded slice с актуальным backend-кандидатом, а не оживлять этот OpenAL target незаметно.
