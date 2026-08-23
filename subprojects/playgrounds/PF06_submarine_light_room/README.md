# PF06 — submarine light room

Статус: **ACTIVE**, закрыты `lighting states`, depth-bounded medium, suspended motes, shadow maps, constrained
tonemapping и helmet baseline (2026-08-23).

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

Low-light shadow pattern теперь принадлежит participating medium, а не материалам. Уже используемые полем density
крупный noise и вытянутый filament формируют медленно движущиеся объёмные языки: внутри них немного возрастает
optical depth и ослабевает локальное in-scattering. Поэтому полоса читается тёмной толщей воды, не исчезает при
уменьшении surface GI и всё ещё затухает в safe light. Это художественная неоднородность среды, не shadow map.

Helmet pass выполняется после tonemapping и depth-tested motes. Screen-space superellipse задаёт стекло и мягкий
обод; radial refraction, холодный tint, внутренняя кромка и два arc-блика растут только к краям, поэтому центр почти
не искажается. Небольшая неоднородность также ограничена ободом и не имитирует постоянно грязную камеру.
`strength=0` — точный passthrough. Wet material highlights остаются в lighting pass, а не рисуются поверх экрана.

## Render flow

```text
shadow-casting light depth
  -> opaque depth + normal/material
  -> per-pixel direct light + room irradiance
  -> depth-bounded participating medium + shadowed god rays + low-light pattern
  -> suspended particles
  -> constrained exposure + tonemapping
  -> helmet glass
  -> interactive Visage controls
  -> present
```

Истинный blackout требует constrained exposure: meter может адаптироваться внутри узкого диапазона, но не имеет
права превращать отсутствие radiance в серую комнату.

## Управление параметрами

PF06 владеет небольшим интерактивным Visage/Nuklear tuning-окном. В нём оставлены четыре оси, которые сейчас нужны
для подбора образа: exploration GI, множитель энергии левого ситуативного источника, density объёмной среды и
contrast/amplitude volumetric shadow pattern. Это canonical runtime state: значения считываются после одного общего
`nk_convert` и попадают в lighting UBO следующего кадра без пересборки графа или pipeline. `Reset defaults`
возвращает исходные числа.

`I` переключает захват мыши между камерой и tuning UI. Переход обратно немедленно rebases cursor position, поэтому
GLFW warp не дёргает камеру. `U` полностью скрывает/возвращает UI; кнопка `Hide UI` делает то же самое и при скрытии
возвращает мышь камере. Пока UI скрыт, host пишет нулевое число draw commands во все rotating command buffers — это
не прозрачное окно, а отсутствие UI draw. `--no-ui` даёт тот же стартовый режим для dump/measurement.

Остальные интерактивные переключатели: `L` (blackout → exploration → safe), `F` (фонарь), `K` (shadow visibility),
`H` (helmet) и `T` (Reinhard → Hable → ACES).
Воспроизводимый CLI rail: `--fixed-camera --fixed-step --lighting=blackout|exploration|safe --frames=N
--dump=file.ppm`; также доступны `--exposure=`, `--pattern=`, `--pattern-speed=`, `--bounce=` и `--left-source=`.
Остальные сформированные оси: `--medium-density=`, `--medium-anisotropy=`, `--god-rays=` и `--motes=`;
`--no-medium`, `--no-shadows` и `--no-helmet` дают runtime A/B. Output принимает `--tonemap=aces|hable|reinhard`,
`--contrast=`, `--saturation=`, `--black-crush=` и `--helmet=`. Для временного rail есть
`--flashlight-on-frame=N`, `--flashlight-off-frame=N` и
`--exploration-on-frame=N`: они запускают источник из нулевого envelope, позволяя dump'ом увидеть положение фронта.

## Срез 1 — lighting states

Первая работающая версия использует один instanced unit cube для 18 элементов тесной комнаты, дверного проёма,
короткого коридора, props и двух смысловых светильников. Fragment shader считает point/spot-like direct Lambert и
Blinn highlight попиксельно. Room-local irradiance — отдельный diffuse term, энергия которого зависит от реально
включённых weak/safe sources; при blackout direct, indirect и emission строго нулевые. Ранний surface warped-ridge
pattern из этого shader удалён после появления объёмного варианта: коэффициент GI теперь меняет только читаемость
материалов и не является неявным master strength для атмосферного эффекта.

HDR сцена и final image разделены: отдельный compose pass применяет фиксированный exposure, холодный grade и
ACES-like curve. Это место позднее принимает depth-bounded medium и helmet, не меняя surface-lighting material.
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
игрок сохраняет общую ориентацию, а тёмная область вне direct light остаётся местом максимальной видимости medium.
При этом medium имеет собственный ambient scattering от присутствующих sources: это намеренный дополнительный
volumetric fill, который не умножается на surface bounce и может нести shadow pattern при `GI=0`.

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

Surface-pattern сам по себе стоил меньше миллисекунды и не являлся главным performance fix. Текущий volumetric
вариант не добавляет noise fetches к medium, но его итоговую стоимость нужно перемерить GPU timestamps вместе со
всем ray march. Следующий возможный шаг medium — temporal/froxel reuse, но только после появления реальной
необходимости: текущий half-resolution путь уже оставляет достаточный запас до shadow maps.

После добавления двух карт, surface PCF, shadowed volume и helmet representative полный rail с включённым фонарём
дал `7.795 ms / 128.3 FPS`. CPU wall-time вокруг present заметно колеблется, поэтому это проверка отсутствия возврата
к прежним `14.5 ms`, а не точная разбивка GPU pass'ов; для такой разбивки нужны timestamp queries.

## Depth-bounded medium и взвесь

Half-resolution pass читает reverse-Z depth и восстанавливает camera ray аналитически из фиксированного FOV.
Двадцать midpoint samples идут только до первого opaque surface (либо до 18 m): Beer–Lambert отдельно поглощает RGB,
single in-scattering получает room irradiance, слабый point source и flashlight cone. Крупный медленный noise и
второй filament scale меняют плотность и без дополнительных noise fetches формируют volumetric pattern; safe light
снижает художественное усиление среды и полностью убирает pattern, но не выключает саму среду.
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

Эксперимент с квантизацией shadow pattern удалён: уровни меняли значение затемнения, но почти не меняли читаемую
форму. Текущий непрерывный pattern строит ridges из двух уже вычисленных density fields, слегка повышает density,
подавляет scattering внутри полосы и сохраняет ближайший выраженный ridge как дополнительную optical depth. Последнее
важно: обычное среднее двадцати samples превращало чередующиеся полосы в однородный серый fog. Screen eccentricity
оставляет центр спокойнее и усиливает эффект в периферии.

Новая граница проверена с нулевым surface GI: pattern on/off всё равно отличается на `AE=837.933`. При обычном
GI отличие равно `1265.27`; при `--no-medium` pattern on/off бит-идентичны (`AE=0`), как и в safe mode (`AE=0`).
То есть effect действительно существует только в атмосфере и больше не модулирует surface shader.

## Срезы

1. **DONE** — `lighting states`: геометрия комнаты, per-pixel lights, room irradiance,
   blackout/exploration/safe и fixed dumps.
2. **DONE** — `medium`: depth-bounded absorption/in-scattering, flashlight volume и крупная муть.
3. **DONE** — `particles + pattern`: depth-tested suspended motes и volumetric low-light pattern с A/B.
4. **DONE** — `shadow maps`: offset flashlight + window light, surface PCF и volume visibility.
5. **DONE** — `helmet + output`: fixed constrained exposure, три tone curves, contrast/saturation/black crush и
   отдельное helmet glass с runtime/CLI A/B.
6. **DONE** — `tuning UI`: четыре актуальных runtime slider, reset, mouse/camera mode и полное отключение UI.
7. `closing`: moving-camera/caster stability, GPU timestamp timings и итоговая настройка образа.

## Definition of Done

- blackout не показывает геометрию за счёт auto exposure;
- exploration проявляет предметы локальным direct+indirect светом и заметно отличается от safe;
- flashlight/window тени согласованы на surface и в объёме;
- атмосфера не светится через opaque geometry, а particles проходят world depth;
- low-light pattern исчезает в blackout и safe, достигая максимума только в слабом свете;
- каждый основной рычаг имеет UI slider и детерминированный CLI/dump A/B;
- moving-camera/caster rails не показывают оторванного shadow ghosting;
- полный кадр проходит Vulkan validation.
