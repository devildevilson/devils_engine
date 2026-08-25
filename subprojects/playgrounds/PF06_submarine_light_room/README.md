# PF06 — submarine light room

Статус: **CLOSED** (2026-08-25). Зафиксированы `lighting states`, depth-bounded medium, suspended motes,
shadow maps, constrained tonemapping, helmet, runtime tuning и per-pass GPU timestamps.

Активная первая художественно направленная сцена Painter stack для `submarine_coop`. Это не gallery эффектов:
одна тесная подводная комната должна целиком менять характер при переключении состояния света, оставаясь одним
renderer/graph recipe со своими runtime-параметрами.

Площадка выборочно копирует зафиксированные механизмы `PF01`–`PF05`, но владеет собственными resources, shaders,
settings и executable. Она не линкуется с targets предыдущих лабораторий.

## Целевой кадр

Комната и короткий коридор имеют три смысловых состояния:

1. `blackout`: работающих источников нет. Fixed exposure не вытягивает стены и силуэты; открытая дверь читается как
   проход в настоящую темноту. UI/emissive gameplay markers могут жить отдельно, но renderer не выдумывает видимость.
2. `exploration`: слабые локальные источники, грибной/аварийный свет или фонарь дают небольшой direct light и
   room-local irradiance. Здесь максимально проявляются взвесь, неоднородность среды и медленный теневой узор.
3. `safe`: основной свет делает геометрию комфортно читаемой. Атмосфера физически остаётся, но её художественные
   усилители затухают по освещённости и перестают выглядеть наложенным horror-фильтром.

`exploration` — не одна жёсткая ступень: энергия слабых источников и room irradiance остаются независимыми runtime
осями. Биолюминесценция за иллюминатором может быть почти на нижней границе, а ручной фонарь локально переводит
только свой конус в хорошо читаемое состояние.

## Lighting model

Opaque geometry получает попиксельный direct light. Первый bounded baseline использует небольшой project-owned
список point/spot lights; если реальная сцена потребует десятки источников, он заменяется доказанным Forward+
assignment из PF01 без изменения material response.

«Простенький GI» здесь — room-local irradiance, а не экранная подсветка готового изображения. Каждый логический
объём имеет diffuse ambient probe/SH-подобный цвет, энергия которого выводится только из включённых источников в
этом объёме и умножается на project bounce coefficient. Поэтому источник может слегка проявить обратные стороны
предметов, но нулевая энергия оставляет комнату чёрной. Это дешёвая gameplay-stable аппроксимация multiple bounce,
не screen-space GI и не обещание межкомнатного переноса света.

Flashlight и направленный свет из иллюминатора имеют отдельные `1024²` reverse-Z shadow maps. Фонарик смещён от
камеры на `22 cm` вправо и `16 cm` вниз: строго camera-coincident источник прячет свою тень за силуэтом caster и
плохо проверяет технику. Surface использует небольшой `3×3` comparison-PCF с world-texel normal offset; half-res
medium делает один compare на пару соседних march samples и пропускает чтение вне cone/при выключенном источнике.
Не каждый декоративный источник обязан иметь тень: shadow-casting — отдельный ограниченный бюджет. Самостоятельный «ghosting теней» в baseline не входит — у
движущегося фонаря он даст оторванные следы. Темпорально накапливать имеет смысл volumetric scattering/шум с
reprojection и rejection, а не задерживать геометрическую границу тени.

## Подводная среда и давящая атмосфера

Среда считается до opaque depth и использует Beer–Lambert absorption плюс in-scattering. Один и тот же объёмный
интеграл создаёт глубинный цвет и god rays от shadowed spot/window light; отдельный screen-space лучевой фильтр не
должен светить через стены. Henyey–Greenstein `g`, density и absorption/scattering colors — runtime settings.

Два разных масштаба движения не смешиваются:

- сотни мелких suspended particles дают близкий параллакс и медленный drift;
- крупномасштабная малоконтрастная неоднородность коэффициента поглощения создаёт «муть»: дальние детали очень
  медленно проявляются и растворяются, не требуя fluid simulation. Это дополнительный дешёвый эффект к исходному
  списку и более естественный носитель давления, чем fullscreen grain.

Indoor pressure теперь складывается из двух независимых сигналов. Surface ridges медленно модулируют только
indirect/orientation fill и плавно стираются direct light. Их координаты строятся в tangent-frame из фактической
normal: один world-space flow проецируется на поверхность, поэтому floor использует свою XZ-плоскость, а стены
поворачивают тот же рисунок в собственную плоскость. Volume shadow теперь выполняет конкретную роль теневого
горизонта: в радиусе `4.5 m` от камеры сохраняются GI и ближайшие ориентиры, после чего каждый camera ray входит в
плотную переходную область шириной `2.4 m`. Она повышает extinction, подавляет локальный in-scattering и набирает
optical depth, скрывая дальнюю геометрию. Это не случайные объёмные фигуры. Медленное world-space signed field,
обычный broad noise и filaments только деформируют расстояние до границы и её плотность, поэтому «стена» неоднородна
и движется относительно мира, не превращаясь в screen-space vignette. У surface pattern два разных warp-состояния
непрерывно перетекают друг в друга, а у стены отдельные длинные фазы меняют изгиб границы и ширину прозрачных
filament-прорезей. Поэтому оба слоя эволюционируют, а не выглядят как одна сдвигающаяся текстура. Оба
pressure-сигнала затухают в safe;
`Shadow wall density > 1` остаётся усиленным horror/illness диапазоном.

Третий слой — не свет, а low-light response наблюдателя. После tone curve он расширяет только уже существующие
тёмные различия, сохраняя точный ноль и защищая яркий direct light. Поэтому exploration может выглядеть слабо
освещённым, но всё же показывать медленное движение surface/volume полей без завышенных GI и density. Response
умножен на source presence и выключается safe gate; накопленная shadow-wall depth дополнительно ограничивает
видимость за читаемым радиусом.
Ни blackout, ни safe от этого не меняются.

Helmet pass выполняется после tonemapping и depth-tested motes. Screen-space superellipse задаёт стекло и мягкий
обод; radial refraction, холодный tint, внутренняя кромка и два arc-блика растут только к краям, поэтому центр почти
не искажается. Небольшая неоднородность также ограничена ободом и не имитирует постоянно грязную камеру.
`strength=0` — точный passthrough. Wet material highlights остаются в lighting pass, а не рисуются поверх экрана.

## Render flow

```text
shadow-casting light depth
  -> opaque depth + normal/material
  -> per-pixel direct light + orientation fill + surface pressure
  -> depth-bounded participating medium + shadowed god rays + noisy visibility horizon
  -> suspended particles
  -> constrained exposure + tonemapping + volume-modulated exploration dark adaptation
  -> helmet glass
  -> interactive Visage controls
  -> present
```

Истинный blackout требует constrained exposure: meter может адаптироваться внутри узкого диапазона, но не имеет
права превращать отсутствие radiance в серую комнату.

## Управление параметрами

PF06 владеет небольшим интерактивным Visage/Nuklear tuning-окном. В нём оставлены семь осей, которые сейчас нужны
для подбора образа: физический exploration GI, независимая perceptual `Low-light visibility`, множитель энергии левого
ситуативного источника, density объёмной среды, `Surface pressure`, плотность теневой стены и радиус читаемой зоны.
Это canonical runtime state:
значения считываются после одного общего `nk_convert` и попадают в lighting UBO следующего кадра без пересборки
графа или pipeline. `Reset defaults`
возвращает исходные числа.

`I` переключает захват мыши между камерой и tuning UI. Переход обратно немедленно rebases cursor position, поэтому
GLFW warp не дёргает камеру. `U` полностью скрывает/возвращает UI; кнопка `Hide UI` делает то же самое и при скрытии
возвращает мышь камере. Пока UI скрыт, host пишет нулевое число draw commands во все rotating command buffers — это
не прозрачное окно, а отсутствие UI draw. `--no-ui` даёт тот же стартовый режим для dump/measurement.

Остальные интерактивные переключатели: `L` (blackout → exploration → safe), `F` (фонарь), `K` (shadow visibility),
`H` (helmet) и `T` (Reinhard → Hable → ACES).
Воспроизводимый CLI rail: `--fixed-camera --fixed-step --lighting=blackout|exploration|safe --frames=N
--dump=file.ppm`; также доступны `--exposure=`, `--low-light-visibility=`, `--pattern=`, `--pattern-speed=`,
`--volume-shadow=`, `--shadow-radius=`, `--bounce=` и `--left-source=`.
Остальные сформированные оси: `--medium-density=`, `--medium-anisotropy=`, `--god-rays=` и `--motes=`;
`--no-medium`, `--no-shadows` и `--no-helmet` дают runtime A/B. Output принимает `--tonemap=aces|hable|reinhard`,
`--contrast=`, `--saturation=`, `--black-crush=` и `--helmet=`. Для временного rail есть
`--flashlight-on-frame=N`, `--flashlight-off-frame=N` и
`--exploration-on-frame=N`: они запускают источник из нулевого envelope, позволяя dump'ом увидеть положение фронта.

## Срез 1 — lighting states

Первая работающая версия использует один instanced unit cube для 18 элементов тесной комнаты, дверного проёма,
короткого коридора, props и двух смысловых светильников. Fragment shader считает point/spot-like direct Lambert и
Blinn highlight попиксельно. Room-local irradiance — отдельный diffuse term, энергия которого зависит от реально
включённых weak/safe sources; при blackout direct, indirect и emission строго нулевые. При exploration source presence
shader гарантирует orientation floor `.085`, даже если authored bounce опущен ниже: это gameplay visibility contract,
а не настоящий bounce. Отдельный surface pressure уменьшает только indirect максимум на 30%, усиливается к периферии
и стирается direct radiance, поэтому не способен превратить читаемую поверхность в чёрную дыру.

HDR сцена и final image разделены: отдельный compose pass применяет фиксированный exposure, холодный grade и
ACES-like curve. Это место позднее принимает depth-bounded medium и helmet, не меняя surface-lighting material.
После filmic curve exploration-only dark adaptation (`.8` по умолчанию) раскрывает weak display signal через
square-root toe. Это не ambient и не auto exposure: RGB умножается только там, где уже есть ненулевая radiance;
source-presence/safe gates оставляют blackout и safe bit-identical при любом значении slider.
Важный graph contract: каждый graphics pass с одним subpass задаёт три состояния attachments — до, активное и после.
Пропуск активного состояния делал normal/depth read-only во время записи и приводил к падению Intel Vulkan driver
при создании pipeline; конфигурация исправлена, а validation rail чист.

В правой части кадра, свободной от диагностического overlay, повторные exploration dumps совпадают (`AE=0`), как и
обычный/validation запуск. Blackout → exploration даёт `AE=19132.1`, exploration → safe — `80470.2`, то есть три
режима являются наблюдаемыми runtime состояниями одного graph, а не pipeline variants.

## GI и распространение включения

Direct и room irradiance намеренно не используют одну формулу. Direct остаётся локальным и может складываться от
нескольких lights. GI получает `max` плавного presence всех источников, а не сумму их energy: один работающий фонарь
уже даёт всей комнате фиксированный минимальный diffuse level, но второй фонарь не поднимает ambient ещё раз. Так
игрок сохраняет общую ориентацию, а тёмная область вне direct light остаётся местом максимальной видимости pressure.
Exploration дополнительно имеет минимальный orientation floor `.085`: ползунок GI меняет художественный bounce выше
этой границы, но больше не может случайно превратить режим исследования в blackout. Medium сохраняет собственный
ambient scattering от присутствующих sources как независимый volumetric fill.

Источник имеет CPU envelope `0..1`, но shader получает пространственный reach. Включение использует quadratic
ease-out `p(t) = 1-(1-t)²`: front быстро даёт ближний ориентир, затем заметно замедляется на дальних поверхностях.
Включение и выключение намеренно асимметричны: фонарь раскрывается `1.8 s`, но после команды off полностью гаснет
примерно за `0.14 s` (9 fixed-step кадров). Та же spatial ease-out при выключении быстро убирает энергию, не меняя
форму распространения отдельным shader path. CLI rails на кадрах 12 и 60 после включения фонаря дают front `2.5 m`
и `9.6 m` соответственно при полной дальности `12 m`.

Room GI имеет фиксированную энергию, но не обязан иметь один неизменный цвет. Цвета bounce от активных типов lights
смешиваются с нормировкой на сумму presence, после чего общая интенсивность всё равно умножается только на `max`
presence. Поэтому вторая лампа может слегка изменить chroma комнаты, но не сделать ambient ярче. Текущая indoor
палитра ограничена тремя ролями: грязный холодный weak light, тёплая tungsten-like safe lamp и нейтрально-холодный
фонарь. Hull/metal сдвинуты из чистого синего в серо-коричневые значения; bounced safe light становится
коричнево-янтарным, а холодный свет после отражения заметно теряет насыщенность.

## Indoor и outdoor atmosphere profiles

Это должны быть два project-owned профиля одной технологии, а не два разных renderer pass:

- `indoor`: меньше частиц, тёмные motes, локальные room bounds, drift `~0.02..0.05 m/s`, сравнительно слабая дальняя
  дымка; частицы читаются главным образом внутри луча и помогают давлению, не превращая воздух в снег;
- `outdoor`: более частая морская взвесь, camera-local объём с recycle, медленнее `~0.005..0.025 m/s`, смесь тёмных
  близких частиц и бледного marine snow на средней дистанции; отдельная far-volume density закрывает масштаб океана.

Текущий executable показывает только indoor fixture, поэтому outdoor числа не стоит притворно настраивать в этой
комнате. Профиль и переключатель добавятся вместе с внешним объёмом за иллюминатором; shader/data layout при этом
можно оставить общими.

## Performance audit

Обычный запуск ограничен 60 FPS, поэтому возвращение к ровным `60` не означает исчезновение GPU cost. Следующая
таблица — исторический замер версии, где pattern ещё жил в surface shader; после переноса в medium строки
`pattern без medium` больше не описывают текущую топологию и оставлены только как основание half-resolution решения.
Uncapped rail на Intel Iris Xe, 1280×720, после 60 прогревочных кадров и на 300 samples показывал:

| Конфигурация | Frame time | FPS |
| --- | ---: | ---: |
| half-res pattern + medium, до shadow/helmet | `7.840 ms` | `127.5` |
| half-res medium, без pattern | `6.486 ms` | `154.2` |
| pattern, без medium | `4.867 ms` | `205.5` |
| без обоих | `4.610 ms` | `216.9` |

До оптимизации полный вариант стоил `14.462 ms`: full-resolution medium добавлял примерно `8.6 ms` над
surface-only baseline. Отдельный half-resolution MRT теперь интегрирует scattering/transmittance, а full-resolution
compose восстанавливает четыре соседних samples с весами по bilinear footprint и относительной linear depth.
Итоговые `7.840 ms` уменьшают стоимость всего кадра примерно на 46%, а изолированный medium overhead — примерно
на 78%, не размывая исходный surface color через границы объектов.

Surface-pattern сам по себе стоил меньше миллисекунды и не являлся главным performance fix. Следующий возможный
шаг medium — temporal/froxel reuse, но только если реальный consumer подтвердит необходимость: текущий
half-resolution путь уже является осознанной baseline-реализацией, а не неизмеренной догадкой.

PF03-style `gpu_timestamp_profiler` теперь подключён к единственному `graphics_ctx`. После ожидания fence текущего
frame-in-flight slot результаты предыдущего завершённого кадра накапливаются, а при `--frames` или выходе по Escape
печатаются `average/minimum/maximum` каждого graph pass и суммарные `average/minimum`. Пустой runtime pass также
остаётся в отчёте и честно показывает почти нулевую цену; изменение топологии по-прежнему требует graph generation.

Representative rail на Intel Iris Xe, `1280×720`, exploration, flashlight/shadows/medium/helmet включены, UI draw
отключён: `--uncapped --fixed-camera --fixed-step --no-ui --lighting=exploration --flashlight --frames=360`.
Получено 356 delayed samples:

| Pass | Average, ms | Minimum, ms | Maximum, ms |
| --- | ---: | ---: | ---: |
| `pf06_window_shadow` | `0.330` | `0.204` | `0.703` |
| `pf06_flashlight_shadow` | `0.241` | `0.164` | `0.563` |
| `pf06_lighting` | `2.366` | `1.502` | `5.817` |
| `pf06_medium` | `6.013` | `3.709` | `14.932` |
| `pf06_compose` | `0.624` | `0.418` | `1.517` |
| `pf06_motes` | `0.028` | `0.018` | `0.069` |
| `pf06_helmet` | `0.351` | `0.215` | `0.956` |
| `pf06_ui` | `0.002` | `0.001` | `0.005` |
| `pf06_present` | `0.130` | `0.084` | `0.321` |
| **Total on GPU** | **`10.137`** | **`6.393`** | — |

Wall-time этого прогона зафиксировался на `16.666 ms`, поэтому он смешан с 60 Hz/present и не описывает стоимость
render graph. Для решений о качестве полезнее GPU minimum: шум и чужая нагрузка преимущественно добавляют время.
Главный дальнейший кандидат на оптимизацию очевиден — `pf06_medium`; две shadow map вместе по минимуму стоят лишь
`0.368 ms`. Два повторных profiled fixed-step dump на frame 180 совпадают побитно (`AE=0`): query-команды не
вмешиваются в результат рендера.

## Depth-bounded medium и взвесь

Half-resolution pass читает reverse-Z depth и восстанавливает camera ray аналитически из фиксированного FOV.
Двадцать midpoint samples идут только до первого opaque surface (либо до 18 m): Beer–Lambert отдельно поглощает RGB,
single in-scattering получает room irradiance, слабый point source и flashlight cone. Крупный медленный noise и
второй filament scale создают обычную муть. Для каждого sample расстояние вдоль camera ray сравнивается с локальным
радиусом теневой стены. Default — `4.5 m` читаемой зоны и `2.4 m` smooth transition. Локальный радиус смещают
world-space signed field и broad noise, а filament иногда прорезает более прозрачный канал. После границы density
возрастает до `+78%`, scattering теряет до `64%`, а интеграл wall occupancy добавляет Beer–Lambert optical depth.
Свободный alpha-канал transmittance несёт bounded dark-adaptation visibility response. Default density `1.0` —
сильный baseline без heavy branch; safe light убирает художественную modulation, но не выключает саму среду.
Полноразмерный compose не апскейлит уже затонированную картинку: он bilateral-like восстанавливает только два
коэффициента среды и применяет их к точному full-resolution scene color.

Фонарик больше не camera-coincident: небольшое физическое смещение делает тени видимыми рядом с caster и требует
отдельной карты для объёма. И flashlight, и window map проверяют visibility sample position; одна проверка
переиспользуется двумя соседними midpoint steps. Это почти не меняет fixed image (`RMSE 0.025/255` относительно
20 проверок на источник), но убирает лишнюю половину texture compares.

Отдельный pass рисует 1536 стабильных procedural world-space motes. Instance id задаёт позицию, медленный vertical
wrap и drift; spherical billboard ограничен примерно двумя экранными пикселями, проходит reverse-Z test и не пишет
depth. Примерно 58% остаются тёмной пылью, 42% получают слабый грязно-бежевый scattering: иначе частица на почти
чёрном фоне принципиально не может быть видна. Весь pass всё ещё гасится без powered source и ослабляется safe
light, поэтому это не самостоятельные emissive sparks. Это дешёвая локальная взвесь, не particle simulation из
PF05 и не замена объёмной плотности.

Промежуточные volumetric-only pattern и sparse-pocket варианты давали красивые отдельные объёмы, но не имели ясной
функции и выглядели как беспорядочные почти чёрные облака. Текущий вариант использует noise только для нарушения
однородности обязательного дальнего горизонта: ближайшие объекты остаются опорными точками, а дальние силуэты
нестабильно растворяются. Почти непрозрачный характер начинает плавно возвращаться только при
`Shadow wall density > 1`. Surface pressure использует normal-relative tangent-frame value-noise и не зависит от
volume density.

При default `Shadow wall density=1.0`, `radius=4.5 m` включённая стена отличается от выключенной на
`AE=10026.6`, `RMSE=1026.11` в fixed frame 180 (`motes=0`). После temporal-morph follow-up неподвижная камера между
frame 180 и 900 даёт `AE=929.661` для surface-only и `AE=2117.31` для volume-only: оба рисунка медленно меняют форму,
а не являются статической грязью или простой прокруткой координат. При `bounce=0` полный pressure всё ещё даёт
`AE=327.072` относительно выключенного, поэтому GI больше не служит master visibility. `Low-light visibility=0→1`
бит-идентичен и в blackout, и в safe (`AE=0`); `Shadow wall density=0→2` там также даёт `AE=0`. Эти состояния нельзя
случайно подсветить perceptual curve или художественной средой.

## Срезы

1. **DONE** — `lighting states`: геометрия комнаты, per-pixel lights, room irradiance,
   blackout/exploration/safe и fixed dumps.
2. **DONE** — `medium`: depth-bounded absorption/in-scattering, flashlight volume и крупная муть.
3. **DONE** — `particles + pressure`: depth-tested suspended motes, surface pressure и subtle/heavy volume ranges.
4. **DONE** — `shadow maps`: offset flashlight + window light, surface PCF и volume visibility.
5. **DONE** — `helmet + output`: fixed constrained exposure, три tone curves, contrast/saturation/black crush и
   отдельное helmet glass с runtime/CLI A/B.
6. **DONE** — `tuning UI`: семь актуальных runtime slider, reset, mouse/camera mode и полное отключение UI.
7. **DONE** — `closing`: per-pass GPU timestamps, deterministic dumps, validation и зафиксированный baseline образа.

## Definition of Done

- blackout не показывает геометрию за счёт auto exposure;
- exploration проявляет предметы локальным direct+indirect светом и заметно отличается от safe;
- flashlight/window тени согласованы на surface и в объёме;
- атмосфера не светится через opaque geometry, а particles проходят world depth;
- low-light pattern исчезает в blackout и safe, достигая максимума только в слабом свете;
- каждый основной рычаг имеет UI slider и детерминированный CLI/dump A/B;
- полный кадр проходит Vulkan validation.

Отдельный animated-caster stress rail не является closing gate: PF06 не использует temporal shadow history, поэтому
оторванный ghosting алгоритмически отсутствует, а bias/aliasing движущихся production meshes нужно проверять уже на
их реальной геометрии. Аналогично outdoor atmosphere остаётся следующим content fixture, а не незакрытой половиной
indoor-комнаты.
