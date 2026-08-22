# Playgrounds

Здесь живут небольшие независимые площадки движка. Каждая лаборатория имеет код для сортировки,
человеческое имя и собственный `README.md`. Текущая программа работ описана в корневом
[`PLAYGROUNDS.md`](../../PLAYGROUNDS.md).

## Правила структуры

- `common/` содержит только уже доказанную общую оболочку: camera/runtime controls, debug views,
  capture и маленькие fixtures. Feature-код конкретной лаборатории туда заранее не переносится.
- Лаборатория владеет своим executable, resources и экспериментальным кодом. Она не линкуется с
  target другой лаборатории.
- Более поздняя лаборатория может начать с выборочной копии или зафиксированного baseline ранней,
  но дальнейшие правки остаются независимыми. Например, `PF06` использует доказанные возможности
  `PF01`–`PF05`, однако расширение ранней gallery не обязано менять `PF06`.
- Общий painter-контракт после доказательства переносится в `libs/painter`, а не образует скрытую
  цепочку наследования между лабораториями.
- README каждой площадки отвечает на вопросы: что проверяется, что запускать, куда смотреть, где
  менять параметры, что входит в первый срез и по каким признакам он закрыт.

## Каталог

| Код | Директория | Состояние | Назначение |
| --- | --- | --- | --- |
| `AU01` | `AU01_spatial_audio` | завершена, OpenAL path архивирован | исторический miniaudio/OpenAL spatial A/B |
| `AU02` | `AU02_directional_coloration` | работает | production miniaudio coloration A/B |
| `PF01` | `PF01_forward_plus` | закрыта | 3D laboratory shell и Forward+ |
| `PF02` | `PF02_shadows` | закрыта | shadow maps и их диагностика |
| `PF03` | `PF03_post_processing` | закрыта | расширяемая независимая post-effect gallery |
| `PF04` | `PF04_stencil_effects` | текущий фокус, outline + local mask + spatial window работают | stencil и masked effects |
| `PF05` | `PF05_scene_effects` | запланирована | SDF, decals, particles/weather, cel shading, billboards и world-space UI |
| `PF06` | `PF06_submarine_light_room` | запланирована | тёмная сцена для `submarine_coop` |
| `PF07` | `PF07_party_environment` | запланирована | динамическое окружение для `party_adventure` |
